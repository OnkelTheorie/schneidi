#include "ui/MediaPool.h"

#include "app/Theme.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Timecode.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "engine/ProxyManager.h"
#include "ui/FileManager.h"
#include "ui/HoverScrub.h"
#include "ui/MediaStorage.h"
#include "ui/PoolFilterButton.h"
#include "engine/Background.h"

#include <QAction>
#include <QActionGroup>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace {

constexpr QSize kThumb{144, 81};
constexpr QSize kListIcon{64, 36};
constexpr int kColorBarH = 6; // Clip color as a stripe at the bottom of the thumbnail

class MediaList : public QListWidget {
public:
    using QListWidget::QListWidget;

protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        auto* data = new QMimeData;
        if (items.isEmpty()) return data;
        // Timeline/Viewer: first clip (resp. title); bin list: all clips
        data->setData(MediaPool::MimeType, items.first()->data(Qt::UserRole).toString().toUtf8());
        QStringList paths;
        for (QListWidgetItem* it : items) {
            const QString path = it->data(Qt::UserRole).toString();
            if (path != MediaPool::TitleItem) paths << path; // sequences too: can be moved into bins
        }
        if (!paths.isEmpty()) data->setData(MediaPool::ListMimeType, paths.join('\n').toUtf8());
        return data;
    }
    QStringList mimeTypes() const override { return {MediaPool::MimeType, MediaPool::ListMimeType}; }
};

// Bin list: drag clips or bins onto it = move (Delete removes the selected bin: MediaPool::keyActions)
class BinTree : public QTreeWidget {
public:
    std::function<void(const QStringList&, int)> mediaDropped;
    std::function<void(int, int)> binDropped;
    std::function<bool(int, int)> canMoveBin; // (bin, target)

    explicit BinTree(QWidget* parent = nullptr) : QTreeWidget(parent)
    {
        setDragEnabled(true);
        setAcceptDrops(true);
        setDropIndicatorShown(false);
        setDragDropMode(QAbstractItemView::DragDrop);
    }

protected:
    QStringList mimeTypes() const override { return {MediaPool::BinMimeType, MediaPool::ListMimeType}; }
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override
    {
        auto* data = new QMimeData;
        if (!items.isEmpty() && items.first()->data(0, Qt::UserRole).toInt() != 0)
            data->setData(MediaPool::BinMimeType, QByteArray::number(items.first()->data(0, Qt::UserRole).toInt()));
        return data;
    }
    Qt::DropActions supportedDropActions() const override { return Qt::MoveAction; }

    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (e->mimeData()->hasFormat(MediaPool::ListMimeType) || e->mimeData()->hasFormat(MediaPool::BinMimeType))
            e->acceptProposedAction();
        else
            e->ignore(); // Files from the file manager: Media Pool imports
    }
    void dragMoveEvent(QDragMoveEvent* e) override
    {
        QTreeWidgetItem* target = itemAt(e->position().toPoint());
        bool ok = target != nullptr;
        if (ok && e->mimeData()->hasFormat(MediaPool::BinMimeType))
            ok = canMoveBin(e->mimeData()->data(MediaPool::BinMimeType).toInt(), target->data(0, Qt::UserRole).toInt());
        setHover(ok ? target : nullptr);
        if (ok) {
            e->setDropAction(Qt::MoveAction);
            e->accept();
        } else {
            e->ignore();
        }
    }
    void dragLeaveEvent(QDragLeaveEvent* e) override
    {
        setHover(nullptr);
        QTreeWidget::dragLeaveEvent(e);
    }
    void dropEvent(QDropEvent* e) override
    {
        setHover(nullptr);
        QTreeWidgetItem* target = itemAt(e->position().toPoint());
        if (!target) return e->ignore();
        const int bin = target->data(0, Qt::UserRole).toInt();
        const QMimeData* mime = e->mimeData();
        e->setDropAction(Qt::MoveAction);
        e->accept();
        if (mime->hasFormat(MediaPool::BinMimeType))
            binDropped(mime->data(MediaPool::BinMimeType).toInt(), bin);
        else
            mediaDropped(QString::fromUtf8(mime->data(MediaPool::ListMimeType)).split('\n', Qt::SkipEmptyParts), bin);
    }

private:
    void setHover(QTreeWidgetItem* it)
    {
        if (it == m_hover) return;
        if (m_hover) m_hover->setBackground(0, QBrush());
        m_hover = it;
        if (m_hover) m_hover->setBackground(0, Theme::alpha(Theme::secondary, 90));
    }
    QTreeWidgetItem* m_hover = nullptr; // Drop target (highlighted)
};

QPixmap placeholderThumb(const QString& label)
{
    QPixmap pm(kThumb);
    pm.fill(Theme::thumbBg);
    QPainter p(&pm);
    p.setPen(Theme::textDim);
    p.drawText(pm.rect(), Qt::AlignCenter, label);
    return pm;
}

// Hover scrub picture in the thumbnail frame (black borders), position bar above the clip color stripe
QPixmap scrubThumb(const QImage& picture, const QPixmap& normal, double fraction)
{
    QPixmap pm(kThumb);
    if (picture.isNull()) {
        if (normal.isNull()) pm.fill(Theme::thumbBg);
        else pm = normal;
    } else {
        pm.fill(Qt::black);
        QPainter p(&pm);
        p.drawImage((kThumb.width() - picture.width()) / 2, (kThumb.height() - picture.height()) / 2, picture);
    }
    QPainter p(&pm);
    const int y = pm.height() - kColorBarH - 4;
    p.fillRect(QRect(0, y, pm.width(), 3), QColor(0, 0, 0, 150));
    p.fillRect(QRect(0, y, int(pm.width() * fraction + 0.5), 3), Theme::primary);
    return pm;
}

QPixmap swatch(const QColor& c)
{
    QPixmap pm(12, 12);
    pm.fill(c);
    return pm;
}

QPixmap flagIcon(const QColor& c)
{
    QPixmap pm(14, 14);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    MediaPool::drawFlag(p, QRectF(1, 1, 12, 12), c);
    return pm;
}

} // namespace

MediaPool::MediaPool(Project* project, Engine* engine, QWidget* parent)
    : QWidget(parent), m_project(project), m_engine(engine)
{
    setObjectName("Panel");
    setAcceptDrops(true);

    QSettings settings;
    m_viewMode = settings.value("mediaPool/view").toString() == "list" ? ViewMode::List : ViewMode::Thumbnails;
    const QString sort = settings.value("mediaPool/sort").toString();
    m_sortKey = sort == "date" ? SortKey::Date : sort == "duration" ? SortKey::Duration : SortKey::Name;
    m_sortAscending = settings.value("mediaPool/sortAscending", true).toBool();

    auto* title = new QLabel(T("Media Pool"));
    title->setObjectName("PanelTitle");
    auto* importBtn = new QToolButton;
    importBtn->setText(T("Importieren…"));
    connect(importBtn, &QToolButton::clicked, this, &MediaPool::importDialog);

    // View like DaVinci at the top right of the Media Pool: icon/list view, sorting
    auto viewButton = [this](const QString& text, const QString& tip, ViewMode mode) {
        auto* b = new QToolButton;
        b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(true);
        b->setAutoRaise(true);
        connect(b, &QToolButton::clicked, this, [this, mode] { setViewMode(mode); });
        return b;
    };
    m_thumbView = viewButton("▦", T("Miniaturansicht"), ViewMode::Thumbnails);
    m_listView = viewButton("☰", T("Listenansicht"), ViewMode::List);
    m_sortBtn = new QToolButton;
    m_sortBtn->setText("⇅");
    m_sortBtn->setToolTip(T("Sortieren nach"));
    m_sortBtn->setAutoRaise(true);
    m_sortBtn->setPopupMode(QToolButton::InstantPopup);
    auto* sortMenu = new QMenu(m_sortBtn);
    auto* keyGroup = new QActionGroup(sortMenu);
    const struct { SortKey key; const char* name; } keys[] = {
        {SortKey::Name, N_("Clipname")}, {SortKey::Date, N_("Änderungsdatum")}, {SortKey::Duration, N_("Dauer")}};
    for (const auto& k : keys) {
        QAction* a = sortMenu->addAction(T(k.name));
        a->setCheckable(true);
        a->setChecked(m_sortKey == k.key);
        keyGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, key = k.key] { setSort(key, m_sortAscending); });
    }
    sortMenu->addSeparator();
    auto* dirGroup = new QActionGroup(sortMenu);
    for (bool asc : {true, false}) {
        QAction* a = sortMenu->addAction(asc ? T("Aufsteigend") : T("Absteigend"));
        a->setCheckable(true);
        a->setChecked(m_sortAscending == asc);
        dirGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, asc] { setSort(m_sortKey, asc); });
    }
    m_sortBtn->setMenu(sortMenu);

    // Proxy generation is running: file + percent and cancel in the header (like the DaVinci progress display)
    m_proxyStatus = new QLabel;
    m_proxyStatus->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    m_proxyStatus->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_proxyStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); // do not push the pool wider
    m_proxyCancel = new QToolButton;
    m_proxyCancel->setText(T("Abbrechen"));
    m_proxyCancel->setToolTip(T("Proxy-Erzeugung abbrechen"));
    connect(m_proxyCancel, &QToolButton::clicked, m_engine->proxies(), &ProxyManager::cancelAll);

    // Search: magnifier shows the field, Esc resp. another click clears and closes it
    m_search = new QLineEdit;
    m_search->setPlaceholderText(T("Suchen…"));
    m_search->setClearButtonEnabled(true);
    m_search->setVisible(false);
    m_search->setMinimumWidth(60);
    connect(m_search, &QLineEdit::textChanged, this, [this] { rebuildClips(); });
    m_searchBtn = new QToolButton;
    m_searchBtn->setText("🔍︎"); // U+FE0E: as text, not as a colored emoji (Windows)
    m_searchBtn->setToolTip(T("Clips suchen"));
    m_searchBtn->setAutoRaise(true);
    m_searchBtn->setCheckable(true);
    connect(m_searchBtn, &QToolButton::toggled, this, [this](bool on) {
        m_search->setVisible(on);
        if (on) m_search->setFocus();
        else m_search->clear();
    });
    m_filterBtn = new PoolFilterButton;
    connect(m_filterBtn, &PoolFilterButton::changed, this, [this] { rebuildClips(); });

    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 4, 0);
    header->setSpacing(2);
    header->addWidget(title);
    header->addWidget(m_proxyStatus, 1);
    header->addWidget(m_proxyCancel);
    header->addWidget(m_search, 1);
    header->addWidget(m_searchBtn);
    header->addWidget(m_filterBtn);
    header->addWidget(m_thumbView);
    header->addWidget(m_listView);
    header->addWidget(m_sortBtn);
    header->addWidget(importBtn);

    // Bin list on the left (Master on top, sub-bins indented)
    auto* bins = new BinTree;
    m_bins = bins;
    m_bins->setHeaderHidden(true);
    m_bins->setColumnCount(1);
    m_bins->setIndentation(12);
    m_bins->setRootIsDecorated(true);
    m_bins->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_bins->setContextMenuPolicy(Qt::CustomContextMenu);
    m_bins->setMinimumWidth(70);
    m_bins->setStyleSheet(QString("QTreeWidget { background: %1; border: none; }").arg(Theme::panel.darker(112).name()));
    bins->mediaDropped = [this](const QStringList& items, int bin) {
        QStringList paths;
        QVector<int> sequences;
        for (const QString& it : items) {
            if (const int id = sequenceOfItem(it)) sequences << id;
            else paths << it;
        }
        m_project->moveMediaToBin(paths, bin, sequences);
    };
    bins->binDropped = [this](int id, int parent) { m_project->moveBin(id, parent); };
    bins->canMoveBin = [this](int id, int target) {
        const MediaBin* b = m_project->bin(id);
        return b && b->parent != target && !m_project->binInside(target, id);
    };
    connect(m_bins, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* it) {
        if (!it || m_bins->signalsBlocked()) return;
        const int id = it->data(0, Qt::UserRole).toInt();
        if (id == m_currentBin) return;
        m_currentBin = id;
        rebuildClips();
    });
    connect(m_bins, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* it) { // Rename via double-click
        const int id = it->data(0, Qt::UserRole).toInt();
        const QString name = it->text(0).trimmed();
        if (id == 0) return;
        if (name.isEmpty() || name == m_project->binName(id)) {
            QSignalBlocker block(m_bins);
            it->setText(0, m_project->binName(id));
            return;
        }
        m_project->renameBin(id, name);
    });
    connect(m_bins, &QTreeWidget::customContextMenuRequested, this, &MediaPool::showBinMenu);

    m_binTitle = new QLabel;
    m_binTitle->setStyleSheet(QString("color: %1; padding: 2px 6px;").arg(Theme::textDim.name()));
    m_binTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); // long filter texts do not widen the pool
    m_filterReset = new QToolButton;
    m_filterReset->setText("✕ " + T("Filter zurücksetzen"));
    m_filterReset->setAutoRaise(true);
    m_filterReset->setVisible(false);
    connect(m_filterReset, &QToolButton::clicked, this, [this] { setFilter({}); });
    auto* titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(0, 0, 4, 0);
    titleRow->setSpacing(2);
    titleRow->addWidget(m_binTitle, 1);
    titleRow->addWidget(m_filterReset);

    m_list = new MediaList;
    m_list->setMovement(QListView::Static);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setDragEnabled(true);
    m_list->setDragDropMode(QAbstractItemView::DragOnly);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection); // several clips for proxy commands, bins, colors
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListWidget::customContextMenuRequested, this, &MediaPool::showContextMenu);
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
        const QString path = it->data(Qt::UserRole).toString();
        if (const int seq = sequenceOfItem(path)) emit sequenceOpenRequested(seq);
        else if (path != TitleItem) emit sourceRequested(path);
    });

    auto* clipArea = new QWidget;
    auto* clipLay = new QVBoxLayout(clipArea);
    clipLay->setContentsMargins(0, 0, 0, 0);
    clipLay->setSpacing(0);
    clipLay->addLayout(titleRow);
    clipLay->addWidget(m_list, 1);

    auto* split = new QSplitter(Qt::Horizontal);
    split->addWidget(m_bins);
    split->addWidget(clipArea);
    split->setStretchFactor(1, 1);
    split->setSizes({110, 400});
    split->setChildrenCollapsible(false);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addLayout(header);
    lay->addWidget(split, 1);

    // Keys (bindable, MainWindow registers them): they act only in their widget, Delete in the bin list never
    // reaches the timeline
    m_removeBinKey = new QAction(T("Ausgewählten Bin entfernen"), this);
    m_removeBinKey->setShortcut(QKeySequence(Qt::Key_Delete));
    m_removeBinKeyAlt = new QAction(T("Ausgewählten Bin entfernen (Rücktaste)"), this);
    m_removeBinKeyAlt->setShortcut(QKeySequence(Qt::Key_Backspace));
    for (QAction* a : {m_removeBinKey, m_removeBinKeyAlt})
        connect(a, &QAction::triggered, this, [this] {
            if (QTreeWidgetItem* it = m_bins->currentItem()) removeBin(it->data(0, Qt::UserRole).toInt());
        });
    m_closeSearchKey = new QAction(T("Suche schließen"), this);
    m_closeSearchKey->setShortcut(QKeySequence(Qt::Key_Escape));
    connect(m_closeSearchKey, &QAction::triggered, this, [this] { m_searchBtn->setChecked(false); }); // clears and closes

    // Hover scrub over video thumbnails (proxy if there is one: much faster to decode)
    m_scrub = new HoverScrub(
        m_list, kThumb,
        [this](QListWidgetItem* it) {
            const QString path = it->data(Qt::UserRole).toString();
            const MediaInfo* m = m_project->mediaInfo(path);
            return m && m->hasVideo && !m->isImage && !m_offline.contains(path) ? ProxyManager::decodeSource(path) : QString();
        },
        [this](QListWidgetItem* it, const QImage& picture, double fraction) {
            const QString path = it->data(Qt::UserRole).toString();
            if (fraction < 0) m_scrubPicture.remove(path);
            else m_scrubPicture.insert(path, scrubThumb(picture, m_thumbs.value(path), fraction));
            updateItem(path);
        });

    m_thumbPool = new QThreadPool(this);
    m_thumbPool->setMaxThreadCount(2);
    m_thumbPool->setThreadPriority(QThread::LowPriority);
    connect(m_project, &Project::mediaChanged, this, &MediaPool::rebuild);
    connect(m_project, &Project::formatChanged, this, [this] { // thumbnails in the new aspect ratio
        ++m_thumbGeneration;
        m_thumbs.clear();
        m_thumbPending.clear();
        rebuildClips();
    });
    connect(m_project, &Project::timelineChanged, this, &MediaPool::updateSequenceItems);
    // Organization changed: rebuild collected after the current event (may come from itemChanged of the
    // bin list – the item must not be deleted there)
    connect(m_project, &Project::poolChanged, this, [this] {
        if (m_rebuildQueued) return;
        m_rebuildQueued = true;
        QTimer::singleShot(0, this, [this] {
            m_rebuildQueued = false;
            rebuildBins();
            rebuildClips();
        });
    });
    ProxyManager* proxies = m_engine->proxies();
    connect(proxies, &ProxyManager::progress, this, [this](const QString& path, int) { updateItem(path); });
    connect(proxies, &ProxyManager::proxyChanged, this, &MediaPool::updateItem);
    connect(proxies, &ProxyManager::queueChanged, this, &MediaPool::updateProxyStatus);
    connect(proxies, &ProxyManager::failed, this, [this](const QString& path, const QString& msg) {
        QMessageBox::warning(this, T("Proxy-Medien"), QFileInfo(path).fileName() + "\n" + msg);
    });
    applyViewMode();
    rebuild();
    updateProxyStatus();
}

void MediaPool::importDialog()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this, T("Medien importieren"), QString(),
        T("Medien (*.mp4 *.mov *.mkv *.avi *.webm *.mts *.m4v *.mp3 *.wav *.flac *.ogg *.m4a *.aac "
        "*.png *.jpg *.jpeg *.webp *.bmp *.srt);;Alle Dateien (*)"));
    importFiles(files);
}

QStringList MediaPool::addFiles(const QStringList& paths, int bin)
{
    // Probing opens every file (recordings with several audio streams: ~0.5 s each) – side by side in threads,
    // otherwise the window stood still for seconds when importing a dozen clips. Added in the given order.
    QVector<MediaInfo> infos(paths.size());
    if (paths.size() == 1) {
        infos[0] = m_engine->probe(paths[0]);
    } else {
        const ProjectFormat format = m_engine->format();
        QThreadPool pool;
        pool.setMaxThreadCount(std::clamp(Background::threads(QThread::idealThreadCount() / 2), 1, int(paths.size())));
        MediaInfo* out = infos.data(); // detached once here, the threads only write their own element
        for (int i = 0; i < paths.size(); ++i)
            pool.start([out, &format, &paths, i] { out[i] = Engine::probe(format, paths[i]); });
        pool.waitForDone();
    }
    QStringList failed;
    for (MediaInfo& info : infos) {
        if (info.length <= 0 || (!info.hasVideo && !info.hasAudio)) {
            failed << info.name;
            continue;
        }
        info.bin = bin;
        m_project->addMedia(info);
    }
    return failed;
}

void MediaPool::importFiles(const QStringList& paths)
{
    // Folder (e.g. dragged from the file manager) including subfolders as bins
    QStringList files, subtitles;
    for (const QString& path : paths) {
        if (QFileInfo(path).isDir()) importFolder(path);
        else if (QFileInfo(path).suffix().compare("srt", Qt::CaseInsensitive) == 0) subtitles << path;
        else files << path;
    }
    if (!subtitles.isEmpty()) emit subtitleFilesImported(subtitles);
    if (files.isEmpty()) return;
    const QStringList failed = addFiles(files, m_currentBin); // like DaVinci: import lands in the selected bin
    if (!failed.isEmpty())
        QMessageBox::warning(this, T("Import"), T("Nicht lesbar:") + "\n" + failed.join('\n'));
}

namespace {
// Are there media files in the folder (or below, depth like importFolderInto)?
bool containsMedia(const QString& dir, int depth)
{
    if (depth > 8) return false;
    const QDir d(dir);
    for (const QFileInfo& fi : d.entryInfoList(QDir::Files))
        if (MediaStorage::isMediaFile(fi.fileName())) return true;
    for (const QFileInfo& sub : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks))
        if (containsMedia(sub.absoluteFilePath(), depth + 1)) return true;
    return false;
}
} // namespace

void MediaPool::importFolder(const QString& dir)
{
    if (!containsMedia(dir, 0)) return; // otherwise an empty undo step would remain and the project would count as modified
    QStringList failed;
    QUndoStack* undo = m_project->undoStack();
    undo->beginMacro(T("Ordner importieren"));
    importFolderInto(dir, m_currentBin, 0, &failed);
    undo->endMacro();
    if (!failed.isEmpty())
        QMessageBox::warning(this, T("Import"), T("Nicht lesbar:") + "\n" + failed.join('\n'));
}

// Only creates the bin if there is media in the folder (or below); returns whether anything was imported
bool MediaPool::importFolderInto(const QString& dir, int parentBin, int depth, QStringList* failed)
{
    if (depth > 8) return false; // protection against very deep trees
    const QDir d(dir);
    QStringList files;
    for (const QFileInfo& fi : d.entryInfoList(QDir::Files, QDir::Name | QDir::IgnoreCase | QDir::LocaleAware))
        if (MediaStorage::isMediaFile(fi.fileName())) files << fi.absoluteFilePath();
    const QFileInfoList subdirs =
        d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name | QDir::IgnoreCase | QDir::LocaleAware);
    if (files.isEmpty() && subdirs.isEmpty()) return false;

    const int bin = m_project->addBin(parentBin, d.dirName());
    bool any = false;
    if (!files.isEmpty()) {
        const QStringList bad = addFiles(files, bin);
        *failed << bad;
        any = bad.size() < files.size();
    }
    for (const QFileInfo& sub : subdirs) any = importFolderInto(sub.absoluteFilePath(), bin, depth + 1, failed) || any;
    if (!any) m_project->removeBin(bin); // nothing usable inside
    return any;
}

void MediaPool::newBin()
{
    m_editBin = m_project->addBin(m_currentBin);
}

void MediaPool::setCurrentBin(int id)
{
    if (id != 0 && !m_project->bin(id)) id = 0;
    m_currentBin = id;
    rebuildBins();
    rebuildClips();
}

void MediaPool::setViewMode(ViewMode mode)
{
    m_viewMode = mode;
    applyViewMode();
    rebuildClips();
    saveSettings();
}

void MediaPool::setSort(SortKey key, bool ascending)
{
    m_sortKey = key;
    m_sortAscending = ascending;
    rebuildClips();
    saveSettings();
}

void MediaPool::setSearch(const QString& text)
{
    m_searchBtn->setChecked(!text.isEmpty());
    m_search->setText(text);
}

const PoolFilter& MediaPool::filter() const
{
    return m_filterBtn->filter();
}

void MediaPool::setFilter(const PoolFilter& filter)
{
    m_filterBtn->setFilter(filter); // changed() rebuilds
}

QVector<MediaPool::KeyAction> MediaPool::keyActions() const
{
    return {{m_removeBinKey, "pool_remove_bin", QKeySequence(Qt::Key_Delete), m_bins},
            {m_removeBinKeyAlt, "pool_remove_bin_alt", QKeySequence(Qt::Key_Backspace), m_bins},
            {m_closeSearchKey, "pool_close_search", QKeySequence(Qt::Key_Escape), m_search}};
}

void MediaPool::saveSettings() const
{
    if (!m_saveSettings) return;
    QSettings s;
    s.setValue("mediaPool/view", m_viewMode == ViewMode::List ? "list" : "thumbnails");
    s.setValue("mediaPool/sort", m_sortKey == SortKey::Date ? "date" : m_sortKey == SortKey::Duration ? "duration" : "name");
    s.setValue("mediaPool/sortAscending", m_sortAscending);
}

void MediaPool::applyViewMode()
{
    const bool list = m_viewMode == ViewMode::List;
    m_thumbView->setChecked(!list);
    m_listView->setChecked(list);
    if (list) {
        m_list->setViewMode(QListView::ListMode);
        m_list->setIconSize(kListIcon);
        m_list->setGridSize(QSize());
        m_list->setWordWrap(false);
        m_list->setSpacing(1);
    } else {
        m_list->setViewMode(QListView::IconMode);
        m_list->setIconSize(kThumb);
        m_list->setGridSize(QSize(kThumb.width() + 16, kThumb.height() + 52)); // two text lines + the item border
        m_list->setWordWrap(true);
        m_list->setSpacing(0);
    }
    m_scrub->setEnabled(!list); // the list view's icons are too small for it
    m_list->setMovement(QListView::Static);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setDragEnabled(true);
    m_list->setDragDropMode(QAbstractItemView::DragOnly);
}

MediaPool::~MediaPool()
{
    // Finish running thumbnail jobs while this object is complete (they report back to it)
    m_thumbPool->clear();
    m_thumbPool->waitForDone();
}

void MediaPool::rebuild()
{
    // Keep the thumbnails of media still there (computing all anew per added file made importing crawl);
    // removed media lose theirs, so a re-import reads the file again
    QSet<QString> paths;
    for (const MediaInfo& m : m_project->media()) paths.insert(m.path);
    for (auto it = m_thumbs.begin(); it != m_thumbs.end();) it = paths.contains(it.key()) ? std::next(it) : m_thumbs.erase(it);
    rebuildBins();
    rebuildClips();
}

void MediaPool::requestThumb(const QString& path, int frame)
{
    if (m_thumbPending.contains(path)) return;
    m_thumbPending.insert(path);
    const ProjectFormat format = m_project->format();
    const int generation = m_thumbGeneration;
    m_thumbPool->start([this, format, path, frame, generation] {
        Background::lowerThisThread(Background::Level::Low); // own pool: its threads do nothing else
        Background::yieldToForeground();
        const QImage img = Engine::thumbnail(format, path, frame, kThumb);
        QMetaObject::invokeMethod(this, [this, path, img, generation] { thumbReady(path, img, generation); },
                                  Qt::QueuedConnection);
    });
}

void MediaPool::thumbReady(const QString& path, const QImage& img, int generation)
{
    if (generation != m_thumbGeneration) return; // format changed meanwhile: requested anew
    m_thumbPending.remove(path);
    const MediaInfo* info = m_project->mediaInfo(path);
    if (!info) return; // removed meanwhile
    m_thumbs.insert(path, img.isNull() ? placeholderThumb(info->hasAudio ? T("♪ Audio") : "?") : QPixmap::fromImage(img));
    updateItem(path);
}

void MediaPool::rebuildBins()
{
    if (m_currentBin != 0 && !m_project->bin(m_currentBin)) m_currentBin = 0;
    // Remember collapsed bins (new ones are expanded)
    QSet<int> collapsed;
    for (QTreeWidgetItemIterator it(m_bins); *it; ++it)
        if (!(*it)->isExpanded() && (*it)->childCount() > 0) collapsed.insert((*it)->data(0, Qt::UserRole).toInt());

    const QSignalBlocker block(m_bins);
    m_bins->clear();
    const QIcon folder = style()->standardIcon(QStyle::SP_DirIcon);
    QTreeWidgetItem* current = nullptr;
    QTreeWidgetItem* edit = nullptr;
    std::function<void(QTreeWidgetItem*, int)> addChildren = [&](QTreeWidgetItem* parent, int parentId) {
        for (int id : m_project->childBins(parentId)) {
            auto* it = new QTreeWidgetItem(parent, {m_project->binName(id)});
            it->setData(0, Qt::UserRole, id);
            it->setIcon(0, folder);
            it->setFlags(it->flags() | Qt::ItemIsEditable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
            if (id == m_currentBin) current = it;
            if (id == m_editBin) edit = it;
            addChildren(it, id);
            it->setExpanded(!collapsed.contains(id));
        }
    };
    auto* master = new QTreeWidgetItem(m_bins, {m_project->binName(0)});
    master->setData(0, Qt::UserRole, 0);
    master->setIcon(0, folder);
    master->setFlags((master->flags() | Qt::ItemIsDropEnabled) & ~Qt::ItemIsDragEnabled);
    addChildren(master, 0);
    master->setExpanded(!collapsed.contains(0));
    m_bins->setCurrentItem(current ? current : master);
    m_editBin = 0;
    if (edit) {
        // New bin: expand parents and edit the name right away (like DaVinci)
        for (QTreeWidgetItem* p = edit->parent(); p; p = p->parent()) p->setExpanded(true);
        m_bins->scrollToItem(edit);
        QTimer::singleShot(0, m_bins, [this, id = edit->data(0, Qt::UserRole).toInt()] {
            for (QTreeWidgetItemIterator it(m_bins); *it; ++it)
                if ((*it)->data(0, Qt::UserRole).toInt() == id) m_bins->editItem(*it);
        });
    }
}

void MediaPool::rebuildClips()
{
    // Selection is kept (e.g. after clip color/flag for several clips)
    QSet<QString> selected;
    for (QListWidgetItem* it : m_list->selectedItems()) selected.insert(it->data(Qt::UserRole).toString());
    const int scroll = m_list->verticalScrollBar()->value();
    m_scrub->forget();
    m_scrubPicture.clear();
    m_list->clear();
    m_items.clear();
    const QString search = m_search->text().trimmed();
    const PoolFilter& filter = m_filterBtn->filter();
    // Search and filter look into the sub-bins too
    const bool narrowed = !search.isEmpty() || filter.active();
    const QString bin = m_project->binName(m_currentBin);
    QString title = search.isEmpty() ? bin : T("Suche in „%1“: %2").arg(bin, search);
    if (filter.active())
        title = search.isEmpty() ? T("Filter in „%1“: %2").arg(bin, filter.summary()) : title + " · " + filter.summary();
    m_binTitle->setText(title);
    m_binTitle->setToolTip(title);
    m_filterReset->setVisible(filter.active());
    const bool list = m_viewMode == ViewMode::List;
    auto found = [&](int itemBin, const QString& name) {
        return narrowed ? m_project->binInside(itemBin, m_currentBin) && name.contains(search, Qt::CaseInsensitive)
                        : itemBin == m_currentBin;
    };

    if (m_currentBin == 0 && !narrowed) {
        // Title generator at the front of the Master (in DaVinci under Effects > Titles > "Text"); drag = 5 s title
        QPixmap titleThumb(kThumb);
        titleThumb.fill(Theme::titleClip.darker(135));
        {
            QPainter p(&titleThumb);
            QFont f = p.font();
            f.setPixelSize(34);
            f.setBold(true);
            p.setFont(f);
            p.setPen(QColor(0xf0, 0xf0, 0xf0));
            p.drawText(titleThumb.rect(), Qt::AlignCenter, "T");
        }
        auto* titleItem = new QListWidgetItem(QIcon(titleThumb), T("Text\nTitel"));
        titleItem->setData(Qt::UserRole, QString(TitleItem));
        titleItem->setToolTip(T("Titel – in die Timeline ziehen"));
        m_list->addItem(titleItem);
    }

    // Timelines and compound clips of the bin (like DaVinci among the clips; here at the front, by name)
    QVector<const Sequence*> seqs;
    for (const Sequence& q : m_project->sequences())
        if (found(q.bin, q.name) && filter.matches(q)) seqs << &q;
    std::stable_sort(seqs.begin(), seqs.end(), [&](const Sequence* a, const Sequence* b) {
        const int c = I18n::compareNames(a->name, b->name);
        return m_sortAscending ? c < 0 : c > 0;
    });
    for (const Sequence* q : seqs) {
        QPixmap thumb(kThumb);
        thumb.fill((q->compound ? Theme::compoundClip : Theme::videoClip).darker(170));
        {
            QPainter p(&thumb);
            p.setRenderHint(QPainter::Antialiasing);
            // Icon: timeline = track bars, compound clip = stacked layers, multicam clip = grid of angles
            const QColor fg(0xe8, 0xe8, 0xec);
            if (q->multicam) {
                for (int i = 0; i < 4; ++i) {
                    const QRectF r(34 + (i % 2) * 31, 14 + (i / 2) * 27, 29, 25);
                    p.setPen(QPen(i == 0 ? Theme::multicamActive : fg, i == 0 ? 2.0 : 1.0));
                    p.setBrush(Theme::compoundClip.darker(110 + 20 * i));
                    p.drawRect(r);
                }
            } else if (q->compound) {
                for (int i = 2; i >= 0; --i) {
                    const QRectF r(46 + i * 8, 20 + (2 - i) * 8, 36, 24);
                    p.setPen(QPen(fg, 1.2));
                    p.setBrush(Theme::compoundClip.darker(100 + 25 * i));
                    p.drawRoundedRect(r, 2, 2);
                }
            } else {
                p.setPen(Qt::NoPen);
                const QColor bars[] = {Theme::videoClip, Theme::videoClip.lighter(120), Theme::audioClip};
                const int x0[] = {30, 50, 30}, w[] = {60, 70, 84};
                for (int i = 0; i < 3; ++i) {
                    p.setBrush(bars[i]);
                    p.drawRoundedRect(QRectF(x0[i], 18 + i * 16, w[i], 11), 2, 2);
                }
                p.setBrush(Theme::playhead);
                p.drawRect(QRectF(72, 12, 2, 58)); // Playhead
            }
        }
        const QString item = sequenceItem(q->id);
        auto* it = new QListWidgetItem(QIcon(thumb), QString());
        it->setData(Qt::UserRole, item);
        it->setToolTip(q->multicam ? T("Multicam-Clip – in die Timeline ziehen legt ihn ab, Doppelklick öffnet die Winkel")
                       : q->compound ? T("Compound Clip – Doppelklick öffnet ihn, in die Timeline ziehen legt ihn ab")
                                     : T("Timeline – Doppelklick öffnet sie, in eine andere Timeline ziehen verschachtelt sie"));
        m_list->addItem(it);
    }
    updateSequenceItems();

    QVector<const MediaInfo*> media;
    for (const MediaInfo& m : m_project->media())
        if (found(m.bin, m.name) && filter.matches(m)) media << &m;
    QHash<QString, QDateTime> dates;
    if (m_sortKey == SortKey::Date)
        for (const MediaInfo* m : media) dates.insert(m->path, QFileInfo(m->path).lastModified());
    std::stable_sort(media.begin(), media.end(), [&](const MediaInfo* a, const MediaInfo* b) {
        int c = 0;
        if (m_sortKey == SortKey::Date) {
            const QDateTime da = dates.value(a->path), db = dates.value(b->path);
            c = da < db ? -1 : db < da ? 1 : 0;
        } else if (m_sortKey == SortKey::Duration) {
            c = a->length < b->length ? -1 : a->length > b->length ? 1 : 0;
        }
        if (c == 0) c = I18n::compareNames(a->name, b->name);
        return m_sortAscending ? c < 0 : c > 0;
    });

    for (const MediaInfo* m : media) {
        const bool offline = !QFileInfo::exists(m->path); // like DaVinci "Media Offline"
        if (!offline && m_offline.remove(m->path)) m_thumbs.remove(m->path); // file is back
        if (!m_thumbs.contains(m->path)) {
            if (offline) {
                m_thumbs.insert(m->path, placeholderThumb(T("Media Offline")));
                m_offline.insert(m->path);
            } else if (m->hasVideo) {
                m_thumbs.insert(m->path, placeholderThumb(QString())); // until the worker delivers
                requestThumb(m->path, m->isImage ? 0 : m->length / 3);
            } else {
                m_thumbs.insert(m->path, placeholderThumb(m->hasAudio ? T("♪ Audio") : "?"));
            }
        }
        const QString duration = Timecode::format(m->length, m_project->fps());
        auto* item = new QListWidgetItem(list ? m->name + "    " + duration : m->name + "\n" + duration);
        item->setData(Qt::UserRole, m->path);
        m_list->addItem(item);
        m_items.insert(m->path, item);
        updateItem(m->path);
    }
    for (int i = 0; i < m_list->count(); ++i)
        if (selected.contains(m_list->item(i)->data(Qt::UserRole).toString())) m_list->item(i)->setSelected(true);
    m_list->verticalScrollBar()->setValue(scroll);
}

void MediaPool::drawFlag(QPainter& p, const QRectF& r, const QColor& color)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const double x = r.left() + r.width() * 0.2;
    p.setPen(QPen(QColor(0x10, 0x10, 0x12), 1.6));
    p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
    QPainterPath flag;
    flag.moveTo(x, r.top() + 0.5);
    flag.lineTo(r.right(), r.top() + r.height() * 0.3);
    flag.lineTo(x, r.top() + r.height() * 0.6);
    flag.closeSubpath();
    p.setPen(QPen(QColor(0, 0, 0, 160), 0.8));
    p.setBrush(color);
    p.drawPath(flag);
    p.restore();
}

void MediaPool::updateItem(const QString& path)
{
    QListWidgetItem* item = m_items.value(path);
    if (!item) return;
    const MediaInfo* info = m_project->mediaInfo(path);
    const ProxyManager* proxies = m_engine->proxies();
    QPixmap pm = m_scrubPicture.contains(path) ? m_scrubPicture.value(path) : m_thumbs.value(path);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QString tip = path;
    if (const int pct = proxies->progressOf(path); pct >= 0) {
        // In progress/waiting: darkened, percent in the middle, bar at the bottom
        p.fillRect(pm.rect(), QColor(0, 0, 0, 140));
        QFont f = p.font();
        f.setPixelSize(13);
        f.setBold(true);
        p.setFont(f);
        p.setPen(QColor(0xf0, 0xf0, 0xf0));
        const bool running = proxies->current() == path;
        p.drawText(pm.rect(), Qt::AlignCenter, running ? QString("Proxy %1 %").arg(pct) : T("Proxy wartet"));
        const QRect bar(6, pm.height() - 10, pm.width() - 12, 4);
        p.fillRect(bar, QColor(0x50, 0x50, 0x56));
        p.fillRect(QRect(bar.left(), bar.top(), bar.width() * pct / 100, bar.height()), Theme::primary);
        tip += "\n" + (running ? T("Proxy wird erzeugt: %1 %").arg(pct) : T("Proxy wartet auf Erzeugung"));
    } else if (proxies->hasProxy(path)) {
        // Small proxy icon at the top right
        const QRectF badge(pm.width() - 22, 4, 18, 14);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x18, 0x18, 0x1b, 220));
        p.drawRoundedRect(badge, 3, 3);
        QFont f = p.font();
        f.setPixelSize(10);
        f.setBold(true);
        p.setFont(f);
        p.setPen(Theme::primary);
        p.drawText(badge, Qt::AlignCenter, "P");
        tip += "\n" + T("Proxy vorhanden");
    }
    if (info) {
        // Clip color as a stripe at the bottom (in list view the image is scaled down -> thicker stripe)
        if (const TrackColorInfo* c = trackColorInfo(info->clipColor)) {
            const int h = m_viewMode == ViewMode::List ? kColorBarH * 2 : kColorBarH;
            p.fillRect(QRect(0, pm.height() - h, pm.width(), h), QColor::fromRgba(c->rgb));
            tip += "\n" + T("Clipfarbe: %1").arg(T(c->name));
        }
        // Flags at the top left next to each other (like DaVinci on the thumbnail)
        const double fs = m_viewMode == ViewMode::List ? 26 : 14;
        double x = 4;
        QStringList names;
        for (const QString& id : info->flags) {
            const FlagColorInfo* f = flagColorInfo(id);
            if (!f) continue;
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0x18, 0x18, 0x1b, 170));
            p.drawRoundedRect(QRectF(x - 2, 2, fs + 2, fs + 2), 2, 2);
            drawFlag(p, QRectF(x, 3, fs - 1, fs), QColor::fromRgba(f->rgb));
            x += fs + 2;
            names << T(f->name);
        }
        if (!names.isEmpty()) tip += "\n" + T("Flags: %1").arg(names.join(", "));
        // Several audio streams (OBS): count at the bottom left, names in the tooltip
        if (const int n = info->audioStreamCount(); n > 1) {
            QFont f = p.font();
            f.setPixelSize(m_viewMode == ViewMode::List ? 20 : 10);
            f.setBold(true);
            p.setFont(f);
            const QString text = QString("\u266A %1").arg(n);
            const QFontMetrics fm(f);
            const int bh = fm.height() + 2, bottom = pm.height() - (m_viewMode == ViewMode::List ? kColorBarH * 2 : kColorBarH) - 3;
            const QRectF badge(4, bottom - bh, fm.horizontalAdvance(text) + 8, bh);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0x18, 0x18, 0x1b, 220));
            p.drawRoundedRect(badge, 3, 3);
            p.setPen(Theme::audioClip.lighter(140));
            p.drawText(badge, Qt::AlignCenter, text);
            QStringList streams;
            for (int k = 0; k < n; ++k) streams << info->audioStreamName(k);
            tip += "\n" + T("%1 Audiospuren: %2").arg(n).arg(streams.join(", "));
        }
    }
    p.end();
    item->setIcon(QIcon(pm));
    item->setToolTip(tip);
}

void MediaPool::updateProxyStatus()
{
    const ProxyManager* proxies = m_engine->proxies();
    const int n = proxies->pendingCount();
    m_proxyStatus->setVisible(n > 0);
    m_proxyCancel->setVisible(n > 0);
    if (n == 0) return;
    QString text = T("Proxy: %1 – %2 %").arg(QFileInfo(proxies->current()).fileName())
                       .arg(proxies->progressOf(proxies->current()));
    if (n > 1) text += " " + T("(+%1 wartend)").arg(n - 1);
    m_proxyStatus->setText(text);
    m_proxyStatus->setToolTip(text);
}

QStringList MediaPool::selectedMedia() const
{
    QStringList paths;
    for (QListWidgetItem* it : m_list->selectedItems()) {
        const QString path = it->data(Qt::UserRole).toString();
        if (path != TitleItem && !sequenceOfItem(path)) paths << path;
    }
    return paths;
}

QVector<int> MediaPool::selectedSequences() const
{
    QVector<int> ids;
    for (QListWidgetItem* it : m_list->selectedItems())
        if (const int id = sequenceOfItem(it->data(Qt::UserRole).toString())) ids << id;
    return ids;
}

QString MediaPool::sequenceItem(int id)
{
    return QStringLiteral("schneidi:seq:%1").arg(id);
}

int MediaPool::sequenceOfItem(const QString& item)
{
    static const QString prefix = QStringLiteral("schneidi:seq:");
    return item.startsWith(prefix) ? item.mid(prefix.size()).toInt() : 0;
}

void MediaPool::updateSequenceItems()
{
    const bool list = m_viewMode == ViewMode::List;
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem* it = m_list->item(i);
        const int id = sequenceOfItem(it->data(Qt::UserRole).toString());
        const Sequence* q = id ? m_project->sequence(id) : nullptr;
        if (!q) continue;
        const QString duration = Timecode::format(TimelineOps::endFrame(q->timeline), m_project->fps());
        const QString text = list ? q->name + "    " + duration : q->name + "\n" + duration;
        if (it->text() != text) it->setText(text);
        QFont f = it->font();
        f.setBold(id == m_project->currentSequence()); // highlight the opened timeline
        it->setFont(f);
    }
}

void MediaPool::newTimeline()
{
    m_project->addTimeline({}, m_currentBin);
}

void MediaPool::renameSequence(int id)
{
    const Sequence* q = m_project->sequence(id);
    if (!q) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, q->compound ? T("Compound Clip umbenennen") : T("Timeline umbenennen"),
                                               T("Name:"), QLineEdit::Normal, q->name, &ok);
    if (ok) m_project->renameSequence(id, name);
}

void MediaPool::addClipColorMenu(QMenu* menu, Project* project, const QStringList& paths)
{
    QMenu* colors = menu->addMenu(T("Clipfarbe"));
    colors->setEnabled(!paths.isEmpty());
    QSet<QString> current;
    for (const QString& path : paths)
        if (const MediaInfo* m = project->mediaInfo(path)) current.insert(m->clipColor);
    for (const auto& i : kTrackColors) {
        QAction* a = colors->addAction(QIcon(swatch(QColor::fromRgba(i.rgb))), T(i.name));
        a->setCheckable(true);
        a->setChecked(current == QSet<QString>{QString::fromLatin1(i.id)});
        QObject::connect(a, &QAction::triggered, project,
                         [project, paths, id = QString::fromLatin1(i.id)] { project->setClipColor(paths, id); });
    }
    colors->addSeparator();
    QAction* clear = colors->addAction(T("Farbe entfernen"));
    clear->setEnabled(!(current.isEmpty() || current == QSet<QString>{QString()}));
    QObject::connect(clear, &QAction::triggered, project, [project, paths] { project->setClipColor(paths, {}); });
}

void MediaPool::addFlagsMenu(QMenu* menu, Project* project, const QStringList& paths)
{
    QMenu* flags = menu->addMenu(T("Flags"));
    flags->setEnabled(!paths.isEmpty());
    QVector<const MediaInfo*> infos;
    for (const QString& path : paths)
        if (const MediaInfo* m = project->mediaInfo(path)) infos << m;
    bool any = false;
    for (const auto& i : kFlagColors) {
        const QString id = QString::fromLatin1(i.id);
        const bool all = !infos.isEmpty() && std::all_of(infos.cbegin(), infos.cend(), [&](const MediaInfo* m) {
            return m->flags.contains(id);
        });
        any = any || std::any_of(infos.cbegin(), infos.cend(), [&](const MediaInfo* m) { return m->flags.contains(id); });
        QAction* a = flags->addAction(QIcon(flagIcon(QColor::fromRgba(i.rgb))), T(i.name));
        a->setCheckable(true);
        a->setChecked(all);
        // If all have the flag: remove, otherwise add to all
        QObject::connect(a, &QAction::triggered, project, [project, paths, id, all] { project->setFlag(paths, id, !all); });
    }
    flags->addSeparator();
    QAction* clear = flags->addAction(T("Alle Flags entfernen"));
    clear->setEnabled(any);
    QObject::connect(clear, &QAction::triggered, project, [project, paths] { project->clearFlags(paths); });
}

void MediaPool::showContextMenu(const QPoint& pos)
{
    // Right-click on a non-selected clip selects only that one (like DaVinci)
    if (QListWidgetItem* it = m_list->itemAt(pos); it && !it->isSelected()) {
        m_list->clearSelection();
        it->setSelected(true);
    }
    const QStringList paths = selectedMedia();
    const QVector<int> seqs = selectedSequences();
    QMenu menu(this);
    if (seqs.size() == 1) { // Timeline/compound clip like DaVinci
        const int id = seqs.first();
        menu.addAction(T("In Timeline öffnen"), this, [this, id] { emit sequenceOpenRequested(id); });
        menu.addAction(T("Umbenennen…"), this, [this, id] { renameSequence(id); });
        menu.addAction(T("Duplizieren"), this, [this, id] { m_project->duplicateSequence(id); });
        QAction* del = menu.addAction(T("Löschen"), this, [this, id] { m_project->removeSequence(id); });
        del->setEnabled(m_project->canRemoveSequence(id));
        if (!del->isEnabled())
            del->setToolTip(m_project->sequenceUsed(id) ? T("Wird noch als Clip in einer Timeline benutzt")
                                                        : T("Die letzte Timeline bleibt"));
        menu.setToolTipsVisible(true);
        menu.addSeparator();
    }
    menu.addAction(T("Neue Timeline"), this, &MediaPool::newTimeline);
    menu.addAction(T("Neuer Bin"), this, &MediaPool::newBin);
    if (!paths.isEmpty()) {
        menu.addAction(T("Neuer Bin mit ausgewählten Clips"), this, [this, paths] {
            QUndoStack* undo = m_project->undoStack();
            undo->beginMacro(T("Neuer Bin mit ausgewählten Clips"));
            const int id = m_project->addBin(m_currentBin);
            m_project->moveMediaToBin(paths, id);
            undo->endMacro();
            m_editBin = id;
        });
        QStringList recordings; // video/audio files (no stills) become the angles, in the order shown
        for (int row = 0; row < m_list->count(); ++row) {
            const QString path = m_list->item(row)->data(Qt::UserRole).toString();
            if (!m_list->item(row)->isSelected() || !paths.contains(path)) continue;
            if (const MediaInfo* m = m_project->mediaInfo(path); m && !m->isImage) recordings << path;
        }
        menu.addAction(T("Multicam-Clip aus ausgewählten Clips erstellen…"), this,
                       [this, recordings] { emit multicamRequested(recordings); })
            ->setEnabled(!recordings.isEmpty());
        menu.addSeparator();
        addClipColorMenu(&menu, m_project, paths);
        addFlagsMenu(&menu, m_project, paths);
        menu.addSeparator();

        ProxyManager* proxies = m_engine->proxies();
        QStringList canGenerate, withProxy, pending;
        for (const QString& path : paths) {
            const MediaInfo* m = m_project->mediaInfo(path);
            if (proxies->isPending(path)) pending << path;
            else if (proxies->hasProxy(path)) withProxy << path;
            else if (m && m->hasVideo && !m->isImage && QFileInfo::exists(path)) canGenerate << path;
        }
        menu.addAction(T("Proxy-Medien erzeugen"), this, [proxies, canGenerate] { proxies->generate(canGenerate); })
            ->setEnabled(!canGenerate.isEmpty());
        menu.addAction(T("Proxy-Medien löschen"), this, [proxies, withProxy] { proxies->remove(withProxy); })
            ->setEnabled(!withProxy.isEmpty());
        if (!pending.isEmpty())
            menu.addAction(T("Proxy-Erzeugung abbrechen"), this, [proxies, pending] { proxies->cancel(pending); });
        menu.addSeparator();
        QString file = paths.first(); // the one right-clicked, else the first selected
        if (QListWidgetItem* it = m_list->itemAt(pos); it && paths.contains(it->data(Qt::UserRole).toString()))
            file = it->data(Qt::UserRole).toString();
        const bool exists = QFileInfo::exists(file);
        menu.addAction(T("Mit Standardprogramm öffnen"), this, [file] { FileManager::open(file); })->setEnabled(exists);
        menu.addAction(T("Im Dateimanager zeigen"), this, [file] { FileManager::reveal(file); })->setEnabled(exists);
    }
    menu.addSeparator();
    menu.addAction(T("Medien importieren…"), this, &MediaPool::importDialog);
    menu.exec(m_list->viewport()->mapToGlobal(pos));
}

void MediaPool::showBinMenu(const QPoint& pos)
{
    QTreeWidgetItem* it = m_bins->itemAt(pos);
    if (it) m_bins->setCurrentItem(it); // selects the bin (like DaVinci)
    const int id = it ? it->data(0, Qt::UserRole).toInt() : 0;
    QMenu menu(this);
    menu.addAction(T("Neuer Bin"), this, &MediaPool::newBin);
    if (id != 0) {
        menu.addAction(T("Bin umbenennen"), this, [this, it] { m_bins->editItem(it); });
        menu.addAction(T("Bin entfernen"), this, [this, id] { removeBin(id); });
    }
    menu.exec(m_bins->viewport()->mapToGlobal(pos));
}

void MediaPool::removeBin(int id)
{
    const MediaBin* b = m_project->bin(id);
    if (!b) return;
    const QString name = b->name;
    const int parent = b->parent;
    int clips = 0;
    for (const MediaInfo& m : m_project->media())
        if (m.bin != 0 && m_project->binInside(m.bin, id)) ++clips;
    // DaVinci deletes the clips too; here they (and sub-bins) move into the parent bin -> nothing gets lost
    if (clips > 0
        && QMessageBox::question(this, T("Bin entfernen"),
                                 T("Bin „%1“ enthält %2 Clip(s). Sie werden in „%3“ verschoben.")
                                     .arg(name, QString::number(clips), m_project->binName(parent)),
                                 QMessageBox::Ok | QMessageBox::Cancel) != QMessageBox::Ok)
        return;
    if (m_currentBin != 0 && m_project->binInside(m_currentBin, id)) m_currentBin = parent;
    m_project->removeBin(id);
}

void MediaPool::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MediaPool::dropEvent(QDropEvent* e)
{
    QStringList paths;
    for (const QUrl& url : e->mimeData()->urls())
        if (url.isLocalFile()) paths << url.toLocalFile();
    importFiles(paths);
}
