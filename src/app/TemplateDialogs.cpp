// SPDX-License-Identifier: AGPL-3.0-only
#include "app/TemplateDialogs.h"

#include "app/Theme.h"
#include "core/I18n.h"
#include "core/TemplateFill.h"
#include "core/Timecode.h"
#include "ui/SlotText.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSet>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kTileW = 176, kTileH = 112;

QStringList splitTags(const QString& text)
{
    QStringList tags;
    for (const QString& t : text.split(',', Qt::SkipEmptyParts))
        if (const QString x = t.trimmed(); !x.isEmpty() && !tags.contains(x)) tags << x;
    return tags;
}

double seconds(int frames, double fps) { return frames / std::max(1.0, fps); }

} // namespace

// ---------------- Save as Template ----------------

SaveTemplateDialog::SaveTemplateDialog(const TemplateInfo& info, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(T("Als Vorlage speichern"));
    auto* lay = new QVBoxLayout(this);
    auto* hint = new QLabel(T("Eine Vorlage ist dieses Projekt mit seinen Platzhaltern (Clip › Als Platzhalter "
                              "markieren…). Neue Projekte aus ihr bekommen alles, was in ihr zu sehen ist."));
    hint->setWordWrap(true);
    hint->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    lay->addWidget(hint);
    auto* form = new QFormLayout;
    m_name = new QLineEdit(info.name);
    m_name->setObjectName("template_name");
    m_description = new QLineEdit(info.description);
    m_description->setObjectName("template_description");
    m_tags = new QLineEdit(info.tags.join(", "));
    m_tags->setObjectName("template_tags");
    m_tags->setPlaceholderText(T("z. B. short, gaming, musik"));
    form->addRow(T("Name"), m_name);
    form->addRow(T("Beschreibung"), m_description);
    form->addRow(T("Schlagwörter"), m_tags);
    lay->addLayout(form);
    m_copy = new QCheckBox(T("Medien in die Vorlage kopieren"));
    m_copy->setObjectName("template_copy_media");
    m_copy->setChecked(true);
    m_copy->setToolTip(T("Aus: die Vorlage verweist auf die Dateien, wo sie jetzt liegen (geht nur auf diesem Rechner)"));
    m_preview = new QCheckBox(T("Bild am Playhead als Vorschau"));
    m_preview->setObjectName("template_preview");
    m_preview->setChecked(true);
    lay->addWidget(m_copy);
    lay->addWidget(m_preview);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    QPushButton* ok = buttons->button(QDialogButtonBox::Save);
    ok->setEnabled(!m_name->text().trimmed().isEmpty());
    connect(m_name, &QLineEdit::textChanged, ok, [ok](const QString& t) { ok->setEnabled(!t.trimmed().isEmpty()); });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);
    resize(460, sizeHint().height());
}

QString SaveTemplateDialog::name() const { return m_name->text().trimmed(); }
QString SaveTemplateDialog::description() const { return m_description->text().trimmed(); }
QStringList SaveTemplateDialog::tags() const { return splitTags(m_tags->text()); }
bool SaveTemplateDialog::copyMedia() const { return m_copy->isChecked(); }
bool SaveTemplateDialog::preview() const { return m_preview->isChecked(); }

// ---------------- Template browser ----------------

TemplateBrowserDialog::TemplateBrowserDialog(QWidget* parent, std::function<QImage(const QString&, const QSize&)> thumbnail)
    : QDialog(parent), m_thumbnail(std::move(thumbnail))
{
    setWindowTitle(T("Neu aus Vorlage"));
    auto* lay = new QVBoxLayout(this);
    auto* filters = new QHBoxLayout;
    m_aspect = new QComboBox;
    m_aspect->setObjectName("template_aspect_filter");
    m_tag = new QComboBox;
    m_tag->setObjectName("template_tag_filter");
    filters->addWidget(new QLabel(T("Format")));
    filters->addWidget(m_aspect);
    filters->addSpacing(12);
    filters->addWidget(new QLabel(T("Schlagwort")));
    filters->addWidget(m_tag);
    filters->addStretch(1);
    lay->addLayout(filters);

    m_list = new QListWidget;
    m_list->setObjectName("template_tiles");
    m_list->setViewMode(QListView::IconMode);
    m_list->setIconSize(QSize(kTileW, kTileH));
    m_list->setGridSize(QSize(kTileW + 24, kTileH + 54));
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    m_list->setWordWrap(true);
    m_list->setUniformItemSizes(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    lay->addWidget(m_list, 1);
    m_details = new QLabel;
    m_details->setWordWrap(true);
    m_details->setMinimumHeight(40);
    m_details->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    lay->addWidget(m_details);

    auto* buttons = new QHBoxLayout;
    m_import = new QPushButton(T("Importieren…"));
    m_import->setObjectName("template_import");
    m_export = new QPushButton(T("Exportieren…"));
    m_export->setObjectName("template_export");
    m_import->setToolTip(T("Eine Vorlage aus einer ZIP-Datei übernehmen"));
    m_export->setToolTip(T("Die gewählte Vorlage als ZIP-Datei speichern (zum Weitergeben)"));
    m_rename = new QPushButton(T("Umbenennen…"));
    m_rename->setObjectName("template_rename");
    m_remove = new QPushButton(T("Löschen"));
    m_remove->setObjectName("template_delete");
    m_edit = new QPushButton(T("Bearbeiten"));
    m_edit->setObjectName("template_edit");
    m_edit->setToolTip(T("Öffnet die Vorlage selbst; Speichern ändert sie"));
    m_new = new QPushButton(T("Neu"));
    m_new->setObjectName("template_new");
    m_new->setDefault(true);
    m_new->setToolTip(T("Neues, unbenanntes Projekt aus der Vorlage"));
    auto* cancel = new QPushButton(T("Abbrechen"));
    for (QPushButton* b : {m_import, m_export, m_rename, m_remove}) buttons->addWidget(b);
    buttons->addStretch(1);
    for (QPushButton* b : {m_edit, m_new, cancel}) buttons->addWidget(b);
    lay->addLayout(buttons);

    connect(m_new, &QPushButton::clicked, this, [this] {
        if (const Item* it = current()) choose(it->content ? Choice::NewContent : Choice::NewStart);
    });
    connect(m_list, &QListWidget::itemDoubleClicked, m_new, &QPushButton::click);
    connect(m_edit, &QPushButton::clicked, this, [this] { choose(Choice::Edit); });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_rename, &QPushButton::clicked, this, &TemplateBrowserDialog::renameCurrent);
    connect(m_remove, &QPushButton::clicked, this, &TemplateBrowserDialog::removeCurrent);
    connect(m_import, &QPushButton::clicked, this, &TemplateBrowserDialog::importZip);
    connect(m_export, &QPushButton::clicked, this, &TemplateBrowserDialog::exportZip);
    connect(m_list, &QListWidget::currentRowChanged, this, &TemplateBrowserDialog::updateButtons);
    connect(m_aspect, &QComboBox::activated, this, &TemplateBrowserDialog::fill);
    connect(m_tag, &QComboBox::activated, this, &TemplateBrowserDialog::fill);
    reload();
    resize(820, 600);
}

void TemplateBrowserDialog::reload()
{
    m_items.clear();
    for (const ContentTemplate& t : TemplateLibrary::all()) {
        Item it;
        it.t = t;
        it.name = t.name;
        it.aspect = t.aspect;
        it.tags = t.tags;
        m_items << it;
    }
    for (const StartTemplate& s : StartTemplates::all()) {
        Item it;
        it.content = false;
        it.s = s;
        it.name = s.displayName();
        it.aspect = TemplateLibrary::aspectLabel(s.data.format.width, s.data.format.height);
        m_items << it;
    }
    // Filters: the shapes and tags there are
    const QString aspect = m_aspect->currentData().toString(), tag = m_tag->currentData().toString();
    QStringList aspects, tags;
    for (const Item& it : m_items) {
        if (!aspects.contains(it.aspect)) aspects << it.aspect;
        for (const QString& t : it.tags)
            if (!tags.contains(t, Qt::CaseInsensitive)) tags << t;
    }
    tags.sort(Qt::CaseInsensitive);
    m_aspect->clear();
    m_aspect->addItem(T("Alle"), QString());
    for (const QString& a : aspects) m_aspect->addItem(a, a);
    m_aspect->setCurrentIndex(std::max(0, m_aspect->findData(aspect)));
    m_tag->clear();
    m_tag->addItem(T("Alle"), QString());
    for (const QString& t : tags) m_tag->addItem(t, t);
    m_tag->setCurrentIndex(std::max(0, m_tag->findData(tag)));
    fill();
}

QIcon TemplateBrowserDialog::tileIcon(const Item& item) const
{
    QPixmap pm(kTileW, kTileH);
    pm.fill(Theme::thumbBg);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    // The picture's shape inside the tile
    const int w = item.content ? item.t.width : item.s.data.format.width;
    const int h = item.content ? item.t.height : item.s.data.format.height;
    QSizeF shape(w > 0 ? w : 16, h > 0 ? h : 9);
    shape.scale(kTileW - 8, kTileH - 8, Qt::KeepAspectRatio);
    const QRectF box((kTileW - shape.width()) / 2, (kTileH - shape.height()) / 2, shape.width(), shape.height());
    QImage picture;
    if (item.content && !item.t.preview.isEmpty()) {
        if (item.t.preview.endsWith(".png", Qt::CaseInsensitive)) picture.load(item.t.preview);
        else if (m_thumbnail) picture = m_thumbnail(item.t.preview, box.size().toSize());
    }
    if (!picture.isNull()) {
        p.drawImage(box, picture);
    } else {
        p.fillRect(box, Theme::panel);
        p.setPen(QPen(item.content ? Theme::secondary : Theme::textFaint, 1, item.content ? Qt::DashLine : Qt::SolidLine));
        p.drawRect(box.adjusted(0.5, 0.5, -0.5, -0.5));
        p.setPen(Theme::textDim);
        p.drawText(box, Qt::AlignCenter, item.aspect);
    }
    // Corner badge: number of fields (content) or "start template"
    QFont f = p.font();
    f.setPointSizeF(7);
    f.setBold(true);
    p.setFont(f);
    const QString badge = item.content ? QStringLiteral("◇ %1").arg(item.t.slotIds.size()) : T("Start");
    const int bw = QFontMetrics(f).horizontalAdvance(badge) + 10;
    const QRectF br(box.right() - bw - 3, box.top() + 3, bw, 15);
    const QColor bg = item.content ? Theme::secondary : Theme::control;
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(br, 3, 3);
    p.setPen(Theme::readableOn(bg));
    p.drawText(br, Qt::AlignCenter, badge);
    p.end();
    return QIcon(pm);
}

void TemplateBrowserDialog::fill()
{
    const QString selected = current() ? current()->name : QString();
    const QString aspect = m_aspect->currentData().toString(), tag = m_tag->currentData().toString();
    m_list->clear();
    for (int i = 0; i < m_items.size(); ++i) {
        const Item& it = m_items[i];
        if (!aspect.isEmpty() && it.aspect != aspect) continue;
        if (!tag.isEmpty() && !it.tags.contains(tag, Qt::CaseInsensitive)) continue;
        QString sub;
        QStringList tip;
        if (it.content) {
            const double fps = it.t.fps > 0 ? it.t.fps : 30;
            sub = QString("%1 · %2 · %3").arg(it.aspect, SlotText::seconds(seconds(it.t.length, fps)),
                                              T("%1 Felder").arg(it.t.slotIds.size()));
            tip << (it.t.builtIn ? T("Eingebaute Vorlage") : T("Eigene Vorlage"));
            if (!it.t.description.isEmpty()) tip << it.t.description;
            if (!it.t.tags.isEmpty()) tip << T("Schlagwörter: %1").arg(it.t.tags.join(", "));
            tip << QString("%1 × %2").arg(it.t.width).arg(it.t.height);
        } else {
            sub = QString("%1 · %2").arg(it.aspect, T("Startvorlage"));
            tip << T("Startvorlage: nur Format und Spuren") << it.s.displayDescription();
        }
        auto* li = new QListWidgetItem(tileIcon(it), it.name + "\n" + sub);
        li->setData(Qt::UserRole, i);
        li->setToolTip(tip.join("\n"));
        m_list->addItem(li);
        if (it.name == selected) m_list->setCurrentItem(li);
    }
    if (!m_list->currentItem() && m_list->count() > 0) m_list->setCurrentRow(0);
    updateButtons();
}

const TemplateBrowserDialog::Item* TemplateBrowserDialog::current() const
{
    const QListWidgetItem* li = m_list->currentItem();
    if (!li) return nullptr;
    const int i = li->data(Qt::UserRole).toInt();
    return i >= 0 && i < m_items.size() ? &m_items[i] : nullptr;
}

void TemplateBrowserDialog::updateButtons()
{
    const Item* it = current();
    const bool own = it && it->content && !it->t.builtIn;
    m_new->setEnabled(it);
    m_edit->setEnabled(own);
    m_rename->setEnabled(own);
    m_remove->setEnabled(own);
    m_export->setEnabled(it && it->content);
    if (!it) {
        m_details->clear();
        return;
    }
    if (it->content) {
        QString text = QString("<b>%1</b>").arg(it->name.toHtmlEscaped());
        if (!it->t.description.isEmpty()) text += " – " + it->t.description.toHtmlEscaped();
        text += "<br>" + T("Felder: %1").arg(it->t.slotIds.isEmpty() ? T("keine") : it->t.slotIds.join(", ").toHtmlEscaped());
        m_details->setText(text);
    } else {
        m_details->setText(QString("<b>%1</b> – %2").arg(it->name.toHtmlEscaped(), it->s.displayDescription().toHtmlEscaped()));
    }
}

void TemplateBrowserDialog::choose(Choice c)
{
    const Item* it = current();
    if (!it) return;
    if (c == Choice::Edit && (!it->content || it->t.builtIn)) return;
    m_choice = c;
    m_chosenContent = it->t;
    m_chosenStart = it->s;
    accept();
}

void TemplateBrowserDialog::renameCurrent()
{
    const Item* it = current();
    if (!it || !it->content || it->t.builtIn) return;
    const QString id = it->t.id;
    bool ok = false;
    const QString name = QInputDialog::getText(this, T("Vorlage umbenennen"), T("Neuer Name:"), QLineEdit::Normal,
                                               it->name, &ok).trimmed();
    if (!ok || name.isEmpty() || name == it->name) return;
    QString error;
    if (!TemplateLibrary::rename(id, name, &error)) {
        QMessageBox::warning(this, T("Vorlage umbenennen"), error);
        return;
    }
    reload();
    select(name);
}

void TemplateBrowserDialog::removeCurrent()
{
    const Item* it = current();
    if (!it || !it->content || it->t.builtIn) return;
    if (QMessageBox::question(this, T("Vorlage löschen"), T("Die Vorlage „%1“ mit ihren Medien löschen?").arg(it->name))
        != QMessageBox::Yes)
        return;
    QString error;
    if (!TemplateLibrary::remove(it->t.id, &error)) {
        QMessageBox::warning(this, T("Vorlage löschen"), error);
        return;
    }
    reload();
}

// ---- ZIP ----

namespace {

// Where the file dialogs start: the folder of the last ZIP, else the documents
QString& lastZipFolder()
{
    static QString folder;
    if (folder.isEmpty()) folder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return folder;
}

void zipFailed(QWidget* parent, const QString& title, const QString& text, const QString& details)
{
    QMessageBox box(QMessageBox::Warning, title, text, QMessageBox::Ok, parent);
    box.setDetailedText(details);
    box.exec();
}

} // namespace

void TemplateBrowserDialog::importZip()
{
    const QString zip = QFileDialog::getOpenFileName(this, T("Vorlage importieren"), lastZipFolder(),
                                                     T("ZIP-Dateien (*.zip)"));
    if (zip.isEmpty()) return;
    lastZipFolder() = QFileInfo(zip).absolutePath();
    importFrom(zip);
}

void TemplateBrowserDialog::exportZip()
{
    const Item* it = current();
    if (!it || !it->content) return;
    const QString zip = QFileDialog::getSaveFileName(this, T("Vorlage exportieren"),
                                                     QDir(lastZipFolder()).filePath(it->t.id + ".zip"),
                                                     T("ZIP-Dateien (*.zip)"));
    if (zip.isEmpty()) return;
    lastZipFolder() = QFileInfo(zip).absolutePath();
    exportTo(zip.endsWith(".zip", Qt::CaseInsensitive) ? zip : zip + ".zip");
}

std::optional<TemplateLibrary::ZipClash> TemplateBrowserDialog::askClash(const QString& zip)
{
    if (m_clashAnswer) return m_clashAnswer();
    QMessageBox box(QMessageBox::Question, T("Vorlage importieren"),
                    T("Eine Vorlage mit dem Namen aus „%1“ gibt es schon.").arg(QFileInfo(zip).fileName()),
                    QMessageBox::NoButton, this);
    QPushButton* replace = box.addButton(T("Ersetzen"), QMessageBox::DestructiveRole);
    QPushButton* both = box.addButton(T("Beide behalten"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(both);
    box.exec();
    if (box.clickedButton() == replace) return TemplateLibrary::ZipClash::Replace;
    if (box.clickedButton() == both) return TemplateLibrary::ZipClash::KeepBoth;
    return std::nullopt;
}

bool TemplateBrowserDialog::importFrom(const QString& zip)
{
    QString error, code;
    std::optional<ContentTemplate> t = TemplateLibrary::importZip(zip, TemplateLibrary::ZipClash::Fail, &error, &code);
    if (!t && code == "EXISTS") {
        const std::optional<TemplateLibrary::ZipClash> answer = askClash(zip);
        if (!answer) return false;
        t = TemplateLibrary::importZip(zip, *answer, &error, &code);
    }
    if (!t) {
        const QString file = QFileInfo(zip).fileName();
        zipFailed(this, T("Vorlage importieren"),
                  code == "BAD_ZIP"        ? T("„%1“ ist keine lesbare ZIP-Datei.").arg(file)
                  : code == "BAD_TEMPLATE" ? T("In „%1“ ist keine schneidi-Vorlage.").arg(file)
                                           : T("Die Vorlage konnte nicht importiert werden."),
                  error);
        return false;
    }
    reload();
    select(t->name);
    return true;
}

bool TemplateBrowserDialog::exportTo(const QString& zip)
{
    const Item* it = current();
    if (!it || !it->content) return false;
    QString error, code;
    if (!TemplateLibrary::exportZip(it->t.dir, zip, true, &error, &code)) {
        zipFailed(this, T("Vorlage exportieren"), T("Die Vorlage konnte nicht exportiert werden."), error);
        return false;
    }
    m_details->setText(T("Exportiert: %1").arg(QDir::toNativeSeparators(zip).toHtmlEscaped()));
    return true;
}

QStringList TemplateBrowserDialog::shownNames() const
{
    QStringList names;
    for (int i = 0; i < m_list->count(); ++i) names << m_list->item(i)->text().section('\n', 0, 0);
    return names;
}

bool TemplateBrowserDialog::select(const QString& name)
{
    for (int i = 0; i < m_list->count(); ++i)
        if (m_list->item(i)->text().section('\n', 0, 0) == name) {
            m_list->setCurrentRow(i);
            return true;
        }
    return false;
}

// ---------------- Check Fields ----------------

SlotCheckDialog::SlotCheckDialog(const ProjectData& d, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(T("Felder prüfen"));
    auto* lay = new QVBoxLayout(this);
    m_tree = new QTreeWidget;
    m_tree->setObjectName("slot_check_list");
    m_tree->setRootIsDecorated(false);
    m_tree->setHeaderLabels({T("Feld"), T("Art"), T("Timeline"), T("Start"), T("Länge"), T("Inhalt"), T("Zustand")});
    const double fps = std::max(1.0, d.format.rate.fps());
    const QVector<TemplateFill::SlotRef> refs = TemplateFill::listSlots(d);
    for (const TemplateFill::SlotRef& r : refs) {
        // What it holds now, checked against its rules
        const TemplateFill::Value v = r.slot.kind == SlotKind::Text ? TemplateFill::Value::ofText(r.text)
                                                                     : TemplateFill::Value::ofFile(r.media);
        const QVector<TemplateFill::Problem> problems = TemplateFill::check(d, {{r.slot.id, v}});
        QStringList state;
        for (const TemplateFill::Problem& p : problems) state << SlotText::problem(p);
        m_problems += int(problems.size());
        QString timeline;
        for (const Sequence& s : d.sequences)
            if (s.id == r.sequence) timeline = s.name;
        auto* item = new QTreeWidgetItem(
            {r.slot.label.isEmpty() ? r.slot.id : QString("%1 (%2)").arg(r.slot.label, r.slot.id), SlotText::kindName(r.slot.kind),
             timeline, Timecode::format(r.start, int(std::lround(fps))), SlotText::seconds(seconds(r.length, fps)),
             r.slot.kind == SlotKind::Text ? r.text : QFileInfo(r.media).fileName(),
             state.isEmpty() ? T("in Ordnung") : state.join(", ")});
        item->setToolTip(5, r.slot.kind == SlotKind::Text ? r.text : r.media);
        if (!r.slot.hint.isEmpty()) item->setToolTip(0, r.slot.hint);
        if (!state.isEmpty()) item->setForeground(6, Theme::warning);
        m_tree->addTopLevelItem(item);
    }
    m_tree->header()->setStretchLastSection(true);
    for (int c = 0; c < m_tree->columnCount() - 1; ++c) m_tree->resizeColumnToContents(c);
    // One and many: own texts (the repo's way, T() has no plural forms)
    const int n = int(refs.size());
    const QString fields = n == 1 ? T("1 Feld") : T("%1 Felder").arg(n);
    const QString problems = m_problems == 1 ? T("1 Problem") : T("%1 Probleme").arg(m_problems);
    auto* summary = new QLabel(refs.isEmpty() ? T("Das Projekt hat keine Platzhalter (Clip › Als Platzhalter markieren…).")
                               : m_problems == 0 ? T("%1, alles in Ordnung.").arg(fields)
                                                 : T("%1, %2.").arg(fields, problems));
    summary->setObjectName("slot_check_summary");
    lay->addWidget(summary);
    lay->addWidget(m_tree, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);
    resize(760, 360);
}
