#include "app/StartTemplatesPage.h"

#include "app/Theme.h"
#include "core/I18n.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

namespace {

// The template list asks for little room (a handful of rows) and grows with the dialog: the settings page with its
// wrapped hints keeps its buttons visible without scrolling (QListWidget asks for 192 px by default)
class TemplateList : public QListWidget {
public:
    QSize sizeHint() const override { return {QListWidget::sizeHint().width(), 110}; }
};

QLabel* hint(const QString& text)
{
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    return l;
}

} // namespace

StartTemplatesPage::StartTemplatesPage(QWidget* parent) : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(10);

    auto* start = new QGroupBox(T("Neue Projekte"));
    auto* form = new QFormLayout(start);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_default = new QComboBox;
    m_default->setObjectName("template_default");
    form->addRow(T("Starten mit"), m_default);
    m_ask = new QCheckBox(T("Bei „Neues Projekt“ jedes Mal nach der Vorlage fragen"));
    m_ask->setObjectName("template_ask");
    m_ask->setChecked(StartTemplates::askOnNew());
    form->addRow(m_ask);
    form->addRow(hint(T("„Neutral“ ist ein neues Projekt wie immer: nichts stumm, nichts umbenannt, das Format kommt "
                        "vom ersten Clip. Die gewählte Vorlage gilt auch für das Projekt, mit dem schneidi startet.")));
    lay->addWidget(start);

    auto* box = new QGroupBox(T("Vorlagen"));
    auto* v = new QVBoxLayout(box);
    m_list = new TemplateList;
    m_list->setObjectName("template_list");
    m_list->setMinimumHeight(110);
    v->addWidget(m_list, 1);
    m_details = new QLabel;
    m_details->setObjectName("template_details");
    m_details->setWordWrap(true);
    m_details->setTextFormat(Qt::PlainText);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(m_details);
    auto* buttons = new QHBoxLayout;
    auto* fromProject = new QPushButton(T("Aus dem Projekt…"));
    fromProject->setObjectName("template_from_project");
    fromProject->setToolTip(T("Das offene Projekt als Startvorlage speichern (wie Datei › Als Startvorlage speichern…)"));
    auto* copy = new QPushButton(T("Kopieren"));
    copy->setObjectName("template_copy");
    m_rename = new QPushButton(T("Umbenennen…"));
    m_rename->setObjectName("template_rename");
    m_remove = new QPushButton(T("Löschen"));
    m_remove->setObjectName("template_delete");
    auto* folder = new QPushButton(T("Ordner öffnen"));
    folder->setObjectName("template_folder");
    folder->setToolTip(QDir::toNativeSeparators(StartTemplates::dir()));
    for (QPushButton* b : {fromProject, copy, m_rename, m_remove}) buttons->addWidget(b);
    buttons->addStretch(1);
    buttons->addWidget(folder);
    v->addLayout(buttons);
    v->addWidget(hint(T("Eigene Vorlage: ein Projekt so einrichten, wie neue Projekte aussehen sollen (Spuren, Namen, "
                        "stumm, Format, Import-Vorgaben), und mit Datei › Als Startvorlage speichern… sichern. "
                        "Eingebaute Vorlagen lassen sich kopieren, aber nicht ändern.")));
    lay->addWidget(box, 1);

    connect(m_list, &QListWidget::currentRowChanged, this, &StartTemplatesPage::showDetails);
    connect(fromProject, &QPushButton::clicked, this, &StartTemplatesPage::saveRequested);
    connect(copy, &QPushButton::clicked, this, &StartTemplatesPage::copyCurrent);
    connect(m_rename, &QPushButton::clicked, this, &StartTemplatesPage::renameCurrent);
    connect(m_remove, &QPushButton::clicked, this, &StartTemplatesPage::removeCurrent);
    connect(folder, &QPushButton::clicked, this, [] {
        QDir().mkpath(StartTemplates::dir());
        QDesktopServices::openUrl(QUrl::fromLocalFile(StartTemplates::dir()));
    });
    const QString def = StartTemplates::defaultTemplate().id;
    fill(def, def);
}

QString StartTemplatesPage::defaultId() const { return m_default->currentData().toString(); }

bool StartTemplatesPage::askOnNew() const { return m_ask->isChecked(); }

void StartTemplatesPage::reload(const QString& defaultId)
{
    const StartTemplate* t = current();
    fill(defaultId.isEmpty() ? this->defaultId() : defaultId, t ? t->id : QString());
}

void StartTemplatesPage::fill(const QString& defaultId, const QString& select)
{
    m_templates = StartTemplates::all();
    m_default->clear();
    m_list->clear();
    for (const StartTemplate& t : m_templates) {
        m_default->addItem(t.displayName(), t.id);
        m_list->addItem(t.builtIn ? T("%1 (eingebaut)").arg(t.displayName()) : t.displayName());
    }
    // A default that is gone (deleted, renamed outside): Neutral
    m_default->setCurrentIndex(std::max(0, m_default->findData(defaultId)));
    const int row = m_default->findData(select);
    m_list->setCurrentRow(row >= 0 ? row : m_default->currentIndex());
    showDetails();
}

const StartTemplate* StartTemplatesPage::current() const
{
    const int row = m_list->currentRow();
    return row >= 0 && row < m_templates.size() ? &m_templates[row] : nullptr;
}

void StartTemplatesPage::showDetails()
{
    const StartTemplate* t = current();
    m_rename->setEnabled(t && !t->builtIn);
    m_remove->setEnabled(t && !t->builtIn);
    if (!t) {
        m_details->clear();
        return;
    }
    QStringList lines;
    if (!t->displayDescription().isEmpty()) lines << t->displayDescription() << QString();
    lines << StartTemplates::summary(*t);
    m_details->setText(lines.join('\n'));
}

void StartTemplatesPage::copyCurrent()
{
    const StartTemplate* t = current();
    if (!t) return;
    StartTemplate copy = *t;
    copy.description = t->displayDescription();
    // A free name: "Podcast (Kopie)", "Podcast (Kopie 2)" …
    for (int n = 1;; ++n) {
        copy.name = n == 1 ? T("%1 (Kopie)").arg(t->displayName()) : T("%1 (Kopie %2)").arg(t->displayName()).arg(n);
        if (!QFileInfo::exists(QDir(StartTemplates::dir()).filePath(StartTemplates::idForName(copy.name) + ".json")))
            break;
    }
    QString error;
    if (!StartTemplates::save(copy, &error)) {
        QMessageBox::warning(this, T("Vorlage kopieren"), T("Die Vorlage konnte nicht gespeichert werden:\n%1").arg(error));
        return;
    }
    fill(defaultId(), copy.id);
}

void StartTemplatesPage::renameCurrent()
{
    const StartTemplate* t = current();
    if (!t || t->builtIn) return;
    const QString id = t->id;
    bool ok = false;
    const QString name = QInputDialog::getText(this, T("Vorlage umbenennen"), T("Neuer Name:"), QLineEdit::Normal,
                                               t->name, &ok).trimmed();
    if (!ok || name.isEmpty() || name == t->name) return;
    const QString def = defaultId();
    QString error;
    if (!StartTemplates::rename(id, name, &error)) {
        QMessageBox::warning(this, T("Vorlage umbenennen"), error);
        return;
    }
    const QString newId = StartTemplates::idForName(name);
    fill(def == id ? newId : def, newId); // the chosen default follows
}

void StartTemplatesPage::removeCurrent()
{
    const StartTemplate* t = current();
    if (!t || t->builtIn) return;
    const QString id = t->id;
    if (QMessageBox::question(this, T("Vorlage löschen"), T("Die Vorlage „%1“ löschen?").arg(t->name))
        != QMessageBox::Yes)
        return;
    if (!StartTemplates::remove(id)) {
        QMessageBox::warning(this, T("Vorlage löschen"), T("Die Datei ließ sich nicht löschen."));
        return;
    }
    const QString def = defaultId();
    fill(def == id ? QString::fromLatin1(StartTemplates::NeutralId) : def, def);
}

// ---- Save as start template ----

SaveStartTemplateDialog::SaveStartTemplateDialog(const StartTemplate& t, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(T("Als Startvorlage speichern"));
    auto* lay = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_name = new QLineEdit(t.name);
    m_name->setObjectName("template_name");
    m_description = new QLineEdit(t.description);
    m_description->setObjectName("template_description");
    m_description->setPlaceholderText(T("optional"));
    form->addRow(T("Name"), m_name);
    form->addRow(T("Beschreibung"), m_description);
    lay->addLayout(form);

    auto* what = new QGroupBox(T("Neue Projekte bekommen"));
    auto* wl = new QVBoxLayout(what);
    auto* summary = new QLabel(StartTemplates::summary(t).join('\n'));
    summary->setWordWrap(true);
    summary->setTextFormat(Qt::PlainText);
    wl->addWidget(summary);
    wl->addWidget(hint(T("Aus diesem Projekt: Format, die Spuren der offenen Timeline mit Namen, Farben, Zuständen und "
                         "Mixer (ohne Clips), Master und die eigenen Import-Vorgaben des Projekts. Ob der erste Clip "
                         "das Format noch ändern darf, steht in den Projekteinstellungen.")));
    lay->addWidget(what);

    m_default = new QCheckBox(T("Neue Projekte starten mit dieser Vorlage"));
    m_default->setObjectName("template_make_default");
    lay->addWidget(m_default);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);
    QPushButton* ok = buttons->button(QDialogButtonBox::Ok);
    const auto update = [this, ok] { ok->setEnabled(!name().isEmpty()); };
    connect(m_name, &QLineEdit::textChanged, this, update);
    update();
    resize(480, sizeHint().height());
}

QString SaveStartTemplateDialog::name() const { return m_name->text().trimmed(); }

QString SaveStartTemplateDialog::description() const { return m_description->text().trimmed(); }

bool SaveStartTemplateDialog::makeDefault() const { return m_default->isChecked(); }
