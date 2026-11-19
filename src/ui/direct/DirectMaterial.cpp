// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/direct/DirectMaterial.h"

#include "app/InputBindings.h"
#include "app/Theme.h"
#include "core/I18n.h"
#include "ui/ElidedLabel.h"
#include "ui/MediaPool.h"
#include "ui/direct/DirectText.h"

#include <QAction>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QSet>
#include <QUrl>
#include <QVBoxLayout>

namespace {
const QSize kThumb(48, 27);

// The analysis state of one clip as four small dots (sound, picture, events, speech): filled = done, a ring in the
// primary color = running, a dim ring = none; the tooltip names them
class AnalysisDots : public QWidget {
public:
    explicit AnalysisDots(const QJsonObject& analysis) : m_analysis(analysis)
    {
        setToolTip(DirectText::analysis(analysis));
        const int d = fontMetrics().height() / 2;
        setFixedSize(4 * d + 3 * (d / 2) + 2, d + 2);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const int d = height() - 2;
        int x = 1;
        for (const char* key : {"sound", "picture", "events", "speech"}) {
            const QString state = m_analysis.value(key).toString();
            const QRectF r(x + 0.5, 1.5, d - 1, d - 1);
            if (state == "done") {
                p.setPen(Qt::NoPen);
                p.setBrush(Theme::audioClip.lighter(130));
            } else {
                p.setPen(QPen(state == "running" ? Theme::primary : Theme::textFaint, 1.2));
                p.setBrush(Qt::NoBrush);
            }
            p.drawEllipse(r);
            x += d + d / 2;
        }
    }

private:
    QJsonObject m_analysis;
};
} // namespace

DirectMaterial::DirectMaterial(QWidget* parent) : QWidget(parent)
{
    setObjectName("direct_material");
    setAcceptDrops(true);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 4, 6, 6);
    lay->setSpacing(4);
    auto* title = new ElidedLabel;
    title->setFullText(T("Material"));
    title->setObjectName("PanelTitle");
    lay->addWidget(title);
    m_hint = new QLabel(T("Clips aus dem Medienpool oder einem Dateimanager hierher ziehen."));
    m_hint->setWordWrap(true);
    m_hint->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    lay->addWidget(m_hint);
    m_list = new QListWidget;
    m_list->setObjectName("direct_material_list");
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setAcceptDrops(false); // the drop goes to the whole column
    m_list->installEventFilter(this);
    lay->addWidget(m_list, 1);
    m_useTimeline = new QPushButton(T("Aktuelle Timeline als Material"));
    m_useTimeline->setObjectName("direct_use_timeline");
    m_useTimeline->setCheckable(true);
    m_useTimeline->setToolTip(T("Die offene Timeline ist das Material (zum Beispiel ein Gespräch ohne Pausen)"));
    connect(m_useTimeline, &QPushButton::toggled, this, &DirectMaterial::setUseTimeline);
    m_clear = new QPushButton(T("Leeren"));
    m_clear->setObjectName("direct_material_clear");
    connect(m_clear, &QPushButton::clicked, this, &DirectMaterial::clear);
    lay->addWidget(m_useTimeline);
    lay->addWidget(m_clear);
    // Delete / Backspace: out of this list (MainWindow hands in the registered action, setRemoveAction)
    m_remove = new QAction(T("Aus dem Material nehmen"), this);
    m_remove->setShortcuts({QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)});
    m_remove->setShortcutContext(Qt::WidgetShortcut);
    connect(m_remove, &QAction::triggered, this, &DirectMaterial::removeSelected);
    rebuild();
}

void DirectMaterial::setRemoveAction(QAction* action)
{
    if (!action || action == m_remove) return;
    if (m_remove->parent() == this) delete m_remove;
    m_remove = action;
    m_remove->setParent(this); // its panel (InputBindings::scopeOf)
    connect(m_remove, &QAction::triggered, this, &DirectMaterial::removeSelected);
}

void DirectMaterial::setThumbs(DirectThumbs* thumbs)
{
    m_thumbs = thumbs;
    rebuild();
}

QStringList DirectMaterial::files() const { return m_files; }

void DirectMaterial::setUseTimeline(bool on)
{
    if (m_useTimeline->isChecked() != on) {
        m_useTimeline->setChecked(on); // comes back here through toggled
        return;
    }
    if (m_timeline == on) return;
    m_timeline = on;
    rebuild();
    emit changed();
}

int DirectMaterial::addFiles(const QStringList& files)
{
    QSet<QString> have(m_files.begin(), m_files.end());
    int added = 0;
    for (const QString& f : files) {
        const QFileInfo fi(f);
        if (!fi.isFile()) continue;
        const QString path = fi.absoluteFilePath();
        if (have.contains(path)) continue;
        have.insert(path);
        m_files << path;
        ++added;
    }
    if (!added) return 0;
    if (m_timeline) {
        const QSignalBlocker b(m_useTimeline); // files dropped: they are the material now (one changed() below)
        m_useTimeline->setChecked(false);
        m_timeline = false;
    }
    rebuild();
    emit changed();
    return added;
}

void DirectMaterial::removeSelected()
{
    if (m_timeline) return;
    QStringList gone;
    for (QListWidgetItem* item : m_list->selectedItems()) gone << item->data(Qt::UserRole).toString();
    if (gone.isEmpty()) return;
    for (const QString& f : gone) m_files.removeAll(f);
    rebuild();
    emit changed();
}

void DirectMaterial::clear()
{
    if (m_files.isEmpty() && !m_timeline) return;
    m_files.clear();
    m_state.clear();
    m_timeline = false;
    {
        const QSignalBlocker b(m_useTimeline);
        m_useTimeline->setChecked(false);
    }
    rebuild();
    emit changed();
}

QJsonObject DirectMaterial::materialJson() const
{
    if (m_timeline) return {{"timeline", "@current"}};
    return {{"files", QJsonArray::fromStringList(m_files)}};
}

void DirectMaterial::setMaterialState(const QJsonArray& material)
{
    for (const QJsonValue& v : material) {
        const QJsonObject m = v.toObject();
        m_state.insert(m.value("file").toString(), m);
    }
    rebuild();
}

bool DirectMaterial::mayCarryFiles(const QMimeData* mime)
{
    if (!mime) return false;
    if (mime->hasFormat(MediaPool::ListMimeType)) return true;
    if (mime->hasUrls())
        for (const QUrl& u : mime->urls())
            if (u.isLocalFile()) return true;
    return false;
}

QStringList DirectMaterial::filesOf(const QMimeData* mime)
{
    QStringList out;
    if (!mime) return out;
    if (mime->hasFormat(MediaPool::ListMimeType)) {
        for (const QString& p : QString::fromUtf8(mime->data(MediaPool::ListMimeType)).split('\n', Qt::SkipEmptyParts))
            if (QFileInfo(p).isFile()) out << p; // timelines of the pool are no files
    }
    if (out.isEmpty() && mime->hasUrls())
        for (const QUrl& u : mime->urls())
            if (u.isLocalFile() && QFileInfo(u.toLocalFile()).isFile()) out << u.toLocalFile();
    return out;
}

void DirectMaterial::dragEnterEvent(QDragEnterEvent* e)
{
    if (mayCarryFiles(e->mimeData())) e->acceptProposedAction();
}

void DirectMaterial::dragMoveEvent(QDragMoveEvent* e)
{
    if (mayCarryFiles(e->mimeData())) e->acceptProposedAction();
}

void DirectMaterial::dropEvent(QDropEvent* e)
{
    const QStringList files = filesOf(e->mimeData());
    if (files.isEmpty()) return;
    e->acceptProposedAction();
    addFiles(files);
}

bool DirectMaterial::eventFilter(QObject* obj, QEvent* e)
{
    // Delete removes from the list and never reaches the window (timeline clips)
    if (obj == m_list && (e->type() == QEvent::ShortcutOverride || e->type() == QEvent::KeyPress)
        && InputBindings::matches(m_remove, static_cast<QKeyEvent*>(e))) {
        e->accept();
        if (e->type() == QEvent::KeyPress) m_remove->trigger();
        return e->type() == QEvent::KeyPress;
    }
    return QWidget::eventFilter(obj, e);
}

void DirectMaterial::rebuild()
{
    const int gen = m_gen.next();
    Q_UNUSED(gen);
    m_list->clear();
    // One small row per clip: picture, name, length, the analysis dots
    const auto addRow = [this](const QString& key, const QString& name, bool enabled) {
        auto* item = new QListWidgetItem(m_list);
        item->setData(Qt::UserRole, key);
        item->setData(Qt::AccessibleTextRole, name);
        if (!enabled) item->setFlags(item->flags() & ~Qt::ItemIsEnabled); // kept for later, not the material now
        auto* row = new QWidget;
        row->setObjectName("direct_material_row");
        row->setEnabled(enabled);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(4, 3, 4, 3);
        h->setSpacing(6);
        auto* pic = new QLabel;
        pic->setFixedSize(kThumb);
        pic->setStyleSheet(QString("background: %1;").arg(Theme::thumbBg.name()));
        pic->setScaledContents(true);
        if (const QImage img = m_pictures.value(key); !img.isNull()) pic->setPixmap(QPixmap::fromImage(img));
        h->addWidget(pic);
        auto* text = new QVBoxLayout;
        text->setSpacing(0);
        auto* title = new ElidedLabel;
        title->setObjectName("direct_material_name");
        title->setFullText(name);
        text->addWidget(title);
        auto* info = new QHBoxLayout;
        info->setSpacing(6);
        const QJsonObject st = m_state.value(key);
        auto* length = new QLabel(st.isEmpty() ? QString("–") : DirectText::clock(st.value("length_s").toDouble()));
        length->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
        info->addWidget(length);
        if (!st.isEmpty()) info->addWidget(new AnalysisDots(st.value("analysis").toObject()));
        info->addStretch(1);
        text->addLayout(info);
        h->addLayout(text, 1);
        row->setToolTip(key.startsWith('@') ? name : QDir::toNativeSeparators(key));
        item->setSizeHint(row->sizeHint());
        m_list->setItemWidget(item, row);
        if (m_thumbs && m_thumbs->hasDecoder() && QFileInfo(key).isFile() && m_pictures.value(key).isNull()) {
            QPointer<QLabel> target(pic);
            m_thumbs->request(key, std::max(0.0, st.value("length_s").toDouble() * 0.3), kThumb * devicePixelRatioF(), m_gen,
                              this, [this, key, target](const QImage& img) {
                                  m_pictures.insert(key, img);
                                  if (target) target->setPixmap(QPixmap::fromImage(img));
                              });
        }
    };
    if (m_timeline) addRow("@current", T("Aktuelle Timeline"), true);
    for (const QString& f : m_files) addRow(f, QFileInfo(f).fileName(), !m_timeline);
    m_hint->setVisible(m_files.isEmpty() && !m_timeline);
    m_clear->setEnabled(!m_files.isEmpty() || m_timeline);
}
