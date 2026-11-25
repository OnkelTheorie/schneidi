// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/MediaStorage.h"
#include "ui/SplitterMemory.h"

#include "app/Theme.h"
#include "core/I18n.h"
#include "core/NameMatch.h"
#include "engine/Background.h"
#include "engine/ProxyManager.h"
#include "engine/StorageThumbCache.h"
#include "engine/video/Thumbnails.h"
#include "ui/FileManager.h"
#include "ui/HoverScrub.h"
#include "ui/SearchHitDelegate.h"

#include <QActionGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEvent>
#include <QDir>
#include <QKeyEvent>
#include <QLineEdit>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QThread>
#include <QThreadPool>
#include <QShowEvent>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>
#include <utility>

namespace {

constexpr QSize kThumb{144, 81}; // like in the Media Pool
constexpr QSize kListIcon{48, 27};
constexpr int kPathRole = Qt::UserRole;
constexpr int kDirRole = Qt::UserRole + 1;
constexpr int kLoadedRole = Qt::UserRole + 2;
constexpr int kDeferredListMs = 1000; // the start folder of a panel not shown, listed after the window is up
constexpr int kDeepPauseMs = 200;     // typing with "With subfolders": search after this pause
constexpr int kDeepBatchMs = 150;     // hits come to the list at least this often while it searches

const char* sortId(MediaStorage::Sort s)
{
    switch (s) {
    case MediaStorage::Sort::Newest: return "newest";
    case MediaStorage::Sort::Oldest: return "oldest";
    case MediaStorage::Sort::Size: return "size";
    case MediaStorage::Sort::Name: break;
    }
    return "name";
}

MediaStorage::Sort sortFromId(const QString& id)
{
    for (const MediaStorage::Sort s :
         {MediaStorage::Sort::Name, MediaStorage::Sort::Newest, MediaStorage::Sort::Oldest, MediaStorage::Sort::Size})
        if (id == QLatin1String(sortId(s))) return s;
    return MediaStorage::Sort::Name;
}

const QStringList& videoExt()
{
    static const QStringList ext{"mp4", "mov", "mkv", "avi", "webm", "mts", "m2ts", "m4v", "mpg", "mpeg", "mxf", "wmv", "flv", "3gp"};
    return ext;
}
const QStringList& audioExt()
{
    static const QStringList ext{"mp3", "wav", "flac", "ogg", "opus", "m4a", "aac", "wma", "aiff", "aif"};
    return ext;
}
const QStringList& imageExt()
{
    static const QStringList ext{"png", "jpg", "jpeg", "webp", "bmp", "tif", "tiff", "gif"};
    return ext;
}

QString thumbKey(const QFileInfo& fi)
{
    return fi.absoluteFilePath() + '|' + QString::number(fi.lastModified().toMSecsSinceEpoch());
}

QPixmap framed(const QImage& img)
{
    QPixmap pm(kThumb);
    pm.fill(Qt::black);
    if (!img.isNull()) {
        const QImage s = img.scaled(kThumb, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPainter p(&pm);
        p.drawImage((kThumb.width() - s.width()) / 2, (kThumb.height() - s.height()) / 2, s);
    }
    return pm;
}

QPixmap placeholder(const QString& label, bool folder = false)
{
    QPixmap pm(kThumb);
    pm.fill(Theme::thumbBg);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    if (folder) {
        // plain folder icon
        const QRectF body(kThumb.width() / 2.0 - 30, 24, 60, 40);
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::mix(Theme::textDim, Theme::thumbBg, 0.3));
        p.drawRoundedRect(QRectF(body.left(), body.top() - 7, 24, 12), 2, 2);
        p.setBrush(Theme::textDim);
        p.drawRoundedRect(body, 3, 3);
        return pm;
    }
    p.setPen(Theme::textDim);
    p.drawText(pm.rect(), Qt::AlignCenter, label);
    return pm;
}

// Check mark on the thumbnail: the file is in the Media Pool already (big = list view, the icon is drawn a third)
void drawPoolMark(QPixmap& pm, bool big)
{
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const double d = big ? 36 : 18;
    const QRectF r(pm.width() - d - 4, 4, d, d);
    p.setPen(QPen(QColor(0x18, 0x18, 0x1b), big ? 3 : 1.5));
    p.setBrush(Theme::primary);
    p.drawEllipse(r);
    QPainterPath check;
    check.moveTo(r.left() + d * 0.27, r.top() + d * 0.53);
    check.lineTo(r.left() + d * 0.44, r.top() + d * 0.69);
    check.lineTo(r.left() + d * 0.74, r.top() + d * 0.34);
    p.setPen(QPen(Theme::onPrimary, d * 0.13, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(check);
}

// File list: dragging yields file URLs (Media Pool and timeline take them as if from the file manager)
class StorageList : public QListWidget {
public:
    using QListWidget::QListWidget;

protected:
    QStringList mimeTypes() const override { return {"text/uri-list"}; }
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        QList<QUrl> urls;
        for (QListWidgetItem* it : items)
            if (!it->data(kDirRole).toBool()) urls << QUrl::fromLocalFile(it->data(kPathRole).toString());
        if (urls.isEmpty()) return nullptr;
        auto* mime = new QMimeData;
        mime->setUrls(urls);
        return mime;
    }
};

} // namespace

bool MediaStorage::isMediaFile(const QString& path)
{
    return kindOf(path) != Kind::All;
}

MediaStorage::Kind MediaStorage::kindOf(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    if (videoExt().contains(ext)) return Kind::Video;
    if (audioExt().contains(ext)) return Kind::Audio;
    if (imageExt().contains(ext)) return Kind::Image;
    return Kind::All;
}

MediaStorage::MediaStorage(QWidget* parent) : QWidget(parent)
{
    setObjectName("Panel");
    m_thumbPool = new QThreadPool(this);
    // Several files at once, but leave most of the machine to the preview and everything else (low priority anyway)
    m_thumbPool->setMaxThreadCount(std::clamp(QThread::idealThreadCount() / 4, 1, 3));
    m_thumbPool->setThreadPriority(QThread::LowPriority);
    m_searchPool = new QThreadPool(this);
    m_searchPool->setMaxThreadCount(1);
    m_searchPool->setThreadPriority(QThread::LowPriority);
    m_deepTimer = new QTimer(this);
    m_deepTimer->setSingleShot(true);
    m_deepTimer->setInterval(kDeepPauseMs);
    connect(m_deepTimer, &QTimer::timeout, this, &MediaStorage::startDeepSearch);

    auto* title = new QLabel(T("Dateibrowser"));
    title->setObjectName("PanelTitle");
    auto* addBtn = new QToolButton;
    addBtn->setText(T("+ Ordner"));
    addBtn->setToolTip(T("Speicherort hinzufügen…"));
    connect(addBtn, &QToolButton::clicked, this, &MediaStorage::addLocation);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 4, 0);
    header->setSpacing(2);
    // Icon/list view like in the Media Pool (list fits into the narrow column of the Edit page)
    auto viewButton = [this](const QString& text, const QString& tip, bool list) {
        auto* b = new QToolButton;
        b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(true);
        b->setAutoRaise(true);
        connect(b, &QToolButton::clicked, this, [this, list] {
            setListMode(list);
            if (m_persistent) QSettings().setValue("mediaStorage/view", list ? "list" : "thumbnails");
        });
        return b;
    };
    m_thumbBtn = viewButton("▦", T("Miniaturansicht"), false);
    m_listBtn = viewButton("☰", T("Listenansicht"), true);

    // Sort: remembered
    m_sort = sortFromId(QSettings().value("mediaStorage/sort").toString());
    m_sortBtn = new QToolButton;
    m_sortBtn->setText("⇅");
    m_sortBtn->setToolTip(T("Sortieren nach"));
    m_sortBtn->setAutoRaise(true);
    m_sortBtn->setPopupMode(QToolButton::InstantPopup);
    auto* sortMenu = new QMenu(m_sortBtn);
    auto* sortGroup = new QActionGroup(sortMenu);
    const struct { Sort sort; const char* name; } sorts[] = {{Sort::Name, N_("Name")},
                                                              {Sort::Newest, N_("Neueste zuerst")},
                                                              {Sort::Oldest, N_("Älteste zuerst")},
                                                              {Sort::Size, N_("Größte zuerst")}};
    for (const auto& k : sorts) {
        QAction* a = sortMenu->addAction(T(k.name));
        a->setCheckable(true);
        a->setChecked(k.sort == m_sort);
        a->setData(int(k.sort));
        sortGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, sort = k.sort] { setSort(sort); });
    }
    m_sortBtn->setMenu(sortMenu);

    // Search: the magnifier shows the field (like the Media Pool's), Esc resp. another click clears and closes it
    m_search = new QLineEdit;
    m_search->setObjectName("storage_search");
    m_search->setPlaceholderText(T("Dateien suchen…"));
    m_search->setClearButtonEnabled(true);
    m_search->setVisible(false);
    m_search->setSizePolicy(QSizePolicy::Ignored, m_search->sizePolicy().verticalPolicy()); // never widens the panel
    m_search->installEventFilter(this);
    connect(m_search, &QLineEdit::textChanged, this, [this] {
        updateGrid();
        // A running deep search is dropped in any case: emptied text (deepActive() false now) would get its hits
        // into the plain listing
        cancelDeepSearch();
        m_deepHits.clear();
        showFiles();
        if (deepActive()) m_deepTimer->start();
    });
    m_searchBtn = new QToolButton;
    m_searchBtn->setObjectName("storage_search_button");
    m_searchBtn->setText("🔍︎"); // U+FE0E: as text, not as a colored emoji (Windows)
    m_searchBtn->setToolTip(T("Dateien suchen"));
    m_searchBtn->setAutoRaise(true);
    m_searchBtn->setCheckable(true);

    // Below the header while searching: subfolders too, quick filter by kind, how many were found
    m_deepBox = new QCheckBox(T("Mit Unterordnern"));
    m_deepBox->setObjectName("storage_search_deep");
    m_deepBox->setToolTip(T("Auch in allen Unterordnern suchen (versteckte Ordner nicht)"));
    connect(m_deepBox, &QCheckBox::toggled, this, [this] {
        cancelDeepSearch();
        m_deepHits.clear();
        updateGrid();
        showFiles();
        if (deepActive()) startDeepSearch();
    });
    m_kindBox = new QComboBox;
    m_kindBox->setObjectName("storage_search_kind");
    m_kindBox->addItem(T("Alle"), int(Kind::All));
    m_kindBox->addItem(T("Videos"), int(Kind::Video));
    m_kindBox->addItem(T("Audio"), int(Kind::Audio));
    m_kindBox->addItem(T("Bilder"), int(Kind::Image));
    m_kindBox->setToolTip(T("Nur diese Art von Dateien zeigen"));
    m_kindBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    connect(m_kindBox, &QComboBox::currentIndexChanged, this,
            [this] { setKindFilter(Kind(m_kindBox->currentData().toInt())); });
    m_searchStatus = new QLabel;
    m_searchStatus->setObjectName("storage_search_status");
    m_searchStatus->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    m_searchStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_searchRow = new QWidget;
    auto* searchLay = new QHBoxLayout(m_searchRow);
    searchLay->setContentsMargins(4, 0, 4, 2);
    searchLay->setSpacing(6);
    searchLay->addWidget(m_deepBox);
    searchLay->addWidget(m_kindBox);
    searchLay->addWidget(m_searchStatus, 1);
    m_searchRow->setVisible(false);

    // The open field takes the title's place: the header stays as narrow as before
    connect(m_searchBtn, &QToolButton::toggled, this, [this, title](bool on) {
        title->setVisible(!on);
        m_search->setVisible(on);
        m_searchRow->setVisible(on);
        if (on) {
            m_search->setFocus();
            return;
        }
        cancelDeepSearch();
        m_deepHits.clear();
        const QSignalBlocker blockText(m_search), blockKind(m_kindBox);
        m_search->clear();
        m_kindBox->setCurrentIndex(0);
        m_kind = Kind::All;
        updateGrid();
        showFiles();
    });

    header->addWidget(title);
    header->addStretch(1);
    header->addWidget(m_search, 100);
    header->addWidget(m_searchBtn);
    header->addWidget(m_thumbBtn);
    header->addWidget(m_listBtn);
    header->addWidget(m_sortBtn);
    header->addWidget(addBtn);
    for (QToolButton* b : {m_searchBtn, m_thumbBtn, m_listBtn, m_sortBtn})
        b->setStyleSheet("QToolButton { padding: 3px 3px; } QToolButton::menu-indicator { image: none; }");

    // Left: storage locations, subfolders are only read when expanded
    m_tree = new QTreeWidget;
    m_tree->setHeaderHidden(true);
    m_tree->setColumnCount(1);
    m_tree->setIndentation(12);
    m_tree->setMinimumWidth(70);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setStyleSheet(QString("QTreeWidget { background: %1; border: none; }").arg(Theme::panel.darker(112).name()));
    connect(m_tree, &QTreeWidget::itemExpanded, this, &MediaStorage::populateChildren);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* it) {
        if (it && !m_tree->signalsBlocked()) setFolder(it->data(0, kPathRole).toString());
    });
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &MediaStorage::showLocationMenu);

    // Right: path with "one level up", below it the folder contents
    auto* upBtn = new QToolButton;
    upBtn->setText("↑");
    upBtn->setToolTip(T("Übergeordneter Ordner"));
    upBtn->setAutoRaise(true);
    connect(upBtn, &QToolButton::clicked, this, [this] {
        QDir d(m_folder);
        if (!m_folder.isEmpty() && d.cdUp()) setFolder(d.absolutePath());
    });
    m_pathLabel = new QLabel;
    m_pathLabel->setStyleSheet(QString("color: %1; padding: 2px 4px;").arg(Theme::textDim.name()));
    m_pathLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_pathLabel->installEventFilter(this);
    auto* pathRow = new QHBoxLayout;
    pathRow->setContentsMargins(2, 0, 2, 0);
    pathRow->setSpacing(0);
    pathRow->addWidget(upBtn);
    pathRow->addWidget(m_pathLabel, 1);

    m_list = new StorageList;
    m_list->setItemDelegate(new SearchHitDelegate(m_list));
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setDragEnabled(true);
    m_list->setDragDropMode(QAbstractItemView::DragOnly);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
        const QString path = it->data(kPathRole).toString();
        if (it->data(kDirRole).toBool()) setFolder(path);
        else emit importRequested({path});
    });
    // Click on a file: look at it in the Viewer before importing (Ctrl/Shift only select)
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
        if (!it->data(kDirRole).toBool() && QGuiApplication::keyboardModifiers() == Qt::NoModifier)
            emit previewRequested(it->data(kPathRole).toString());
    });
    m_scrub = new HoverScrub(
        m_list, kThumb,
        [](QListWidgetItem* it) {
            const QString path = it->data(kPathRole).toString();
            const bool video = !it->data(kDirRole).toBool() && videoExt().contains(QFileInfo(path).suffix().toLower());
            return video ? ProxyManager::decodeSource(path) : QString();
        },
        [this](QListWidgetItem* it, const QImage& picture, double fraction) {
            if (fraction < 0) {
                m_hoverPath.clear();
                m_hoverPixmap = {};
            } else {
                m_hoverPath = it->data(kPathRole).toString();
                m_hoverPixmap = picture.isNull() ? m_shown.value(m_hoverPath) : framed(picture);
                QPainter p(&m_hoverPixmap); // position in the clip
                p.fillRect(QRect(0, kThumb.height() - 3, kThumb.width(), 3), QColor(0, 0, 0, 150));
                p.fillRect(QRect(0, kThumb.height() - 3, int(kThumb.width() * fraction + 0.5), 3), Theme::primary);
            }
            updateItem(it);
        });
    connect(m_list, &QListWidget::customContextMenuRequested, this, &MediaStorage::showFileMenu);

    auto* fileArea = new QWidget;
    auto* fileLay = new QVBoxLayout(fileArea);
    fileLay->setContentsMargins(0, 0, 0, 0);
    fileLay->setSpacing(0);
    fileLay->addLayout(pathRow);
    fileLay->addWidget(m_list, 1);

    auto* split = new QSplitter(Qt::Horizontal);
    split->addWidget(m_tree);
    split->addWidget(fileArea);
    split->setStretchFactor(1, 1);
    split->setCollapsible(1, false); // folder tree can be pushed shut completely in narrow columns
    SplitterMemory::remember(split, "mediaStorageTree", {110, 400}); // the user's drag comes back on the next start

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addLayout(header);
    lay->addWidget(m_searchRow);
    lay->addWidget(split, 1);

    // New/deleted files in the open folder (e.g. freshly copied from the phone) appear on their own
    m_watcher = new QFileSystemWatcher(this);
    auto* rescan = new QTimer(this);
    rescan->setSingleShot(true);
    rescan->setInterval(300);
    connect(rescan, &QTimer::timeout, this, &MediaStorage::rebuildFiles);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, rescan, qOverload<>(&QTimer::start));

    QSettings s;
    setListMode(s.value("mediaStorage/view").toString() == "list");
    if (s.contains("mediaStorage/locations")) {
        m_locations = s.value("mediaStorage/locations").toStringList();
    } else {
        // First start: Videos folder (if present) and home folder
        const QString movies = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
        if (!movies.isEmpty() && QFileInfo(movies).isDir()) m_locations << movies;
        m_locations << QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    }
    rebuildLocations();
    QString last = s.value("mediaStorage/folder").toString();
    if (last.isEmpty() || !QFileInfo(last).isDir()) last = m_locations.value(0);
    m_savedFolder = QDir::cleanPath(last);
    m_deferFiles = true;
    setFolder(last);
    m_deferFiles = false;
}

void MediaStorage::showEvent(QShowEvent* event)
{
    listPendingFiles();
    QWidget::showEvent(event);
}

void MediaStorage::listPendingFiles()
{
    if (m_filesPending) rebuildFiles();
}

MediaStorage::~MediaStorage()
{
    // Finish a running thumbnail while this object is complete (it reports back to it)
    m_thumbPool->clear();
    m_thumbPool->waitForDone();
    cancelDeepSearch();
    m_searchPool->waitForDone();
}

void MediaStorage::setListMode(bool list)
{
    m_listMode = list;
    m_thumbBtn->setChecked(!list);
    m_listBtn->setChecked(list);
    m_scrub->setEnabled(!list); // the list view's icons are too small for it
    if (list) {
        m_list->setViewMode(QListView::ListMode);
        m_list->setIconSize(kListIcon);
        m_list->setGridSize(QSize());
        m_list->setWordWrap(false);
        m_list->setSpacing(1);
    } else {
        m_list->setViewMode(QListView::IconMode);
        m_list->setIconSize(kThumb);
        m_list->setWordWrap(true);
        m_list->setSpacing(0);
    }
    // setViewMode resets these
    m_list->setMovement(QListView::Static);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setDragEnabled(true);
    m_list->setDragDropMode(QAbstractItemView::DragOnly);
    updateGrid();
    for (QListWidgetItem* it : std::as_const(m_items)) updateItem(it); // pool mark in the size of the view
}

// Icon view: the name's lines, and with hits below the folder their folder under it
void MediaStorage::updateGrid()
{
    if (m_listMode) return;
    // two lines of name stay (the folder line and a little air below it)
    const int folderLine = deepActive() ? SearchHitDelegate::subtitleHeight(m_list->font()) + 6 : 0;
    m_list->setGridSize(QSize(kThumb.width() + 16, kThumb.height() + 36 + folderLine));
}

// Shorten the path from the left: the end (current folder) is what matters
void MediaStorage::updatePathLabel()
{
    const QString native = QDir::toNativeSeparators(m_folder);
    m_pathLabel->setText(m_pathLabel->fontMetrics().elidedText(native, Qt::ElideLeft, std::max(20, m_pathLabel->width() - 10)));
    m_pathLabel->setToolTip(native);
}

bool MediaStorage::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == m_pathLabel && e->type() == QEvent::Resize) updatePathLabel();
    // Esc in the search field clears and closes it; the field takes the key itself (no window key with Esc fires)
    if (obj == m_search && (e->type() == QEvent::ShortcutOverride || e->type() == QEvent::KeyPress)
        && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape && static_cast<QKeyEvent*>(e)->modifiers() == Qt::NoModifier) {
        e->accept();
        if (e->type() == QEvent::KeyPress) m_searchBtn->setChecked(false);
        return true;
    }
    return QWidget::eventFilter(obj, e);
}

void MediaStorage::saveLocations()
{
    if (m_persistent) QSettings().setValue("mediaStorage/locations", m_locations);
}

void MediaStorage::addLocation()
{
    const QString dir = QFileDialog::getExistingDirectory(this, T("Speicherort hinzufügen"), m_folder);
    if (dir.isEmpty()) return;
    const QString clean = QDir::cleanPath(dir);
    if (!m_locations.contains(clean)) {
        m_locations << clean;
        saveLocations();
        rebuildLocations();
    }
    setFolder(clean);
}

void MediaStorage::removeLocation(QTreeWidgetItem* item)
{
    if (!item || item->parent()) return;
    m_locations.removeAll(item->data(0, kPathRole).toString());
    saveLocations();
    rebuildLocations();
    selectFolderInTree(m_folder);
}

void MediaStorage::rebuildLocations()
{
    QSignalBlocker block(m_tree);
    m_tree->clear();
    for (const QString& loc : m_locations) {
        auto* it = new QTreeWidgetItem(m_tree, {QDir(loc).dirName().isEmpty() ? loc : QDir(loc).dirName()});
        it->setData(0, kPathRole, loc);
        it->setToolTip(0, QDir::toNativeSeparators(loc));
        it->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        if (!QFileInfo(loc).isDir()) it->setForeground(0, Theme::textDim); // e.g. unplugged USB stick
    }
}

void MediaStorage::populateChildren(QTreeWidgetItem* item)
{
    if (!item || item->data(0, kLoadedRole).toBool()) return;
    item->setData(0, kLoadedRole, true);
    const QString path = item->data(0, kPathRole).toString();
    const QFileInfoList dirs =
        QDir(path).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase | QDir::LocaleAware);
    QSignalBlocker block(m_tree);
    for (const QFileInfo& fi : dirs) {
        auto* child = new QTreeWidgetItem(item, {fi.fileName()});
        child->setData(0, kPathRole, fi.absoluteFilePath());
        child->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    }
    if (dirs.isEmpty()) item->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicator);
}

// Mark folder in the tree (expand under the matching location), without triggering setFolder again
void MediaStorage::selectFolderInTree(const QString& dir)
{
    QTreeWidgetItem* best = nullptr;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* top = m_tree->topLevelItem(i);
        const QString loc = top->data(0, kPathRole).toString();
        if (dir == loc || dir.startsWith(loc.endsWith('/') ? loc : loc + '/')) {
            if (!best || loc.size() > best->data(0, kPathRole).toString().size()) best = top;
        }
    }
    QSignalBlocker block(m_tree);
    if (!best) {
        m_tree->setCurrentItem(nullptr);
        m_tree->clearSelection();
        return;
    }
    QTreeWidgetItem* it = best;
    const QString rel = QDir(it->data(0, kPathRole).toString()).relativeFilePath(dir);
    if (rel != ".") {
        for (const QString& part : rel.split('/', Qt::SkipEmptyParts)) {
            populateChildren(it);
            it->setExpanded(true);
            QTreeWidgetItem* next = nullptr;
            for (int i = 0; i < it->childCount() && !next; ++i)
                if (it->child(i)->text(0) == part) next = it->child(i);
            if (!next) break;
            it = next;
        }
    }
    m_tree->setCurrentItem(it);
    m_tree->scrollToItem(it);
}

void MediaStorage::setFolder(const QString& dir)
{
    const QString clean = dir.isEmpty() ? QString() : QDir::cleanPath(dir);
    if (!m_watcher->directories().isEmpty()) m_watcher->removePaths(m_watcher->directories());
    m_folder = clean;
    if (!clean.isEmpty() && QFileInfo(clean).isDir()) m_watcher->addPath(clean);
    updatePathLabel();
    selectFolderInTree(clean);
    if (m_persistent && clean != m_savedFolder) {
        m_savedFolder = clean;
        QSettings().setValue("mediaStorage/folder", clean);
    }
    m_list->scrollToTop();
    if (m_deferFiles && !isVisible()) {
        // Not shown yet: listed when it shows, else shortly after the start (the thumbnails load in the background)
        m_filesPending = true;
        QTimer::singleShot(kDeferredListMs, this, &MediaStorage::listPendingFiles);
        return;
    }
    rebuildFiles();
}

QStringList MediaStorage::mediaIn(const QString& dir) const
{
    QStringList out;
    const QFileInfoList files =
        QDir(dir).entryInfoList(QDir::Files, QDir::Name | QDir::IgnoreCase | QDir::LocaleAware);
    for (const QFileInfo& fi : files)
        if (isMediaFile(fi.fileName())) out << fi.absoluteFilePath();
    return out;
}

void MediaStorage::rebuildFiles()
{
    m_filesPending = false;
    m_dirEntries.clear();
    m_fileEntries.clear();
    if (!m_folder.isEmpty()) {
        const QDir dir(m_folder);
        m_dirEntries = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase | QDir::LocaleAware);
        for (const QFileInfo& fi : dir.entryInfoList(QDir::Files, QDir::Name | QDir::IgnoreCase | QDir::LocaleAware))
            if (isMediaFile(fi.fileName())) m_fileEntries << fi;
    }
    if (deepActive()) {
        // the folder changed (another one, or files came and went): search below it again
        cancelDeepSearch();
        m_deepHits.clear();
        showFiles();
        startDeepSearch();
        return;
    }
    showFiles();
}

bool MediaStorage::sortsBefore(const QFileInfo& a, const QFileInfo& b) const
{
    switch (m_sort) {
    case Sort::Newest:
    case Sort::Oldest: {
        const QDateTime ta = a.lastModified(), tb = b.lastModified();
        if (ta != tb) return m_sort == Sort::Newest ? ta > tb : ta < tb;
        break;
    }
    case Sort::Size:
        if (a.size() != b.size()) return a.size() > b.size();
        break;
    case Sort::Name: break;
    }
    const int c = I18n::compareNames(a.fileName(), b.fileName());
    return c != 0 ? c < 0 : a.absoluteFilePath() < b.absoluteFilePath();
}

bool MediaStorage::kindMatches(const QString& path) const
{
    return m_kind == Kind::All || kindOf(path) == m_kind;
}

bool MediaStorage::deepActive() const
{
    return m_searchBtn->isChecked() && m_deepBox->isChecked() && !NameMatch::queryWords(m_search->text()).isEmpty();
}

void MediaStorage::showFiles()
{
    const QStringList selected = selectedFiles();
    m_scrub->forget();
    m_hoverPath.clear();
    m_list->clear();
    m_items.clear();
    m_shown.clear();
    m_thumbQueue.clear();
    m_thumbOnDisk.clear();
    m_shownFiles.clear();
    m_dirRows = 0;
    const QStringList words = m_searchBtn->isChecked() ? NameMatch::queryWords(m_search->text()) : QStringList();
    const bool deep = deepActive();
    if (!m_folder.isEmpty() && !deep) {
        for (const QFileInfo& fi : std::as_const(m_dirEntries)) {
            if (!NameMatch::matches(words, fi.fileName())) continue;
            auto* it = new QListWidgetItem(QIcon(placeholder({}, true)), fi.fileName());
            it->setData(kPathRole, fi.absoluteFilePath());
            it->setData(kDirRole, true);
            it->setData(SearchHitDelegate::kHighlightRole,
                        SearchHitDelegate::highlights(NameMatch::ranges(words, fi.fileName())));
            it->setToolTip(T("Ordner öffnen (Doppelklick)"));
            m_list->addItem(it);
            ++m_dirRows;
        }
    }
    QFileInfoList files;
    for (const QFileInfo& fi : deep ? std::as_const(m_deepHits) : std::as_const(m_fileEntries))
        if (kindMatches(fi.fileName()) && (deep || NameMatch::matches(words, fi.fileName()))) files << fi;
    std::stable_sort(files.begin(), files.end(), [this](const QFileInfo& a, const QFileInfo& b) { return sortsBefore(a, b); });
    const QDir root(m_folder);
    for (const QFileInfo& fi : std::as_const(files)) {
        QString sub;
        if (deep) {
            const QString rel = root.relativeFilePath(fi.absolutePath());
            sub = QDir::toNativeSeparators(rel == "." ? root.dirName() : root.dirName() + '/' + rel);
        }
        addFileItem(fi, m_list->count(), sub, words);
        if (selected.contains(fi.absoluteFilePath())) m_items.value(fi.absoluteFilePath())->setSelected(true);
    }
    updateSearchStatus();
    nextThumb();
}

void MediaStorage::addFileItem(const QFileInfo& fi, int row, const QString& subtitle, const QStringList& words)
{
    const QString path = fi.absoluteFilePath();
    if (m_items.contains(path)) return;
    const QString ext = fi.suffix().toLower();
    QPixmap pm = m_thumbs.value(thumbKey(fi));
    if (pm.isNull()) {
        if (audioExt().contains(ext)) {
            pm = placeholder(T("♪ Audio"));
        } else {
            pm = placeholder("…");
            queueThumb(path);
            if (QFileInfo::exists(StorageThumbCache::fileFor(fi))) m_thumbOnDisk.insert(path);
        }
    }
    auto* it = new QListWidgetItem(fi.fileName());
    it->setData(kPathRole, path);
    if (!words.isEmpty())
        it->setData(SearchHitDelegate::kHighlightRole, SearchHitDelegate::highlights(NameMatch::ranges(words, fi.fileName())));
    if (!subtitle.isEmpty()) it->setData(SearchHitDelegate::kSubtitleRole, subtitle);
    m_list->insertItem(row, it);
    m_shownFiles.insert(row - m_dirRows, fi);
    m_items.insert(path, it);
    m_shown.insert(path, pm);
    updateItem(it);
}

QStringList MediaStorage::shownFiles() const
{
    QStringList out;
    for (const QFileInfo& fi : m_shownFiles) out << fi.absoluteFilePath();
    return out;
}

void MediaStorage::cancelDeepSearch()
{
    m_deepTimer->stop();
    if (m_deepCancel) *m_deepCancel = true;
    m_deepCancel.reset();
    ++m_deepGeneration; // what the old one still sends is dropped
    m_deepRunning = false;
}

// Below the current folder, in a thread of its own: breadth first (the near folders first), hidden folders and
// symbolic links to folders skipped (no loops), at most kMaxDeepHits hits; they come in batches to addDeepHits
void MediaStorage::startDeepSearch()
{
    cancelDeepSearch();
    m_deepHits.clear();
    m_deepCapped = false;
    if (!deepActive() || m_folder.isEmpty()) {
        updateSearchStatus();
        return;
    }
    m_deepRunning = true;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_deepCancel = cancel;
    const int generation = m_deepGeneration;
    const QStringList words = NameMatch::queryWords(m_search->text());
    const Kind kind = m_kind;
    const QString root = m_folder;
    updateSearchStatus();
    m_searchPool->start([this, cancel, generation, words, kind, root] {
        QFileInfoList batch;
        QElapsedTimer sinceSent;
        sinceSent.start();
        int found = 0;
        bool capped = false;
        auto send = [&](bool done) {
            QMetaObject::invokeMethod(
                this,
                [this, generation, batch, done, capped] {
                    if (generation != m_deepGeneration) return; // an older search
                    addDeepHits(batch);
                    if (done) {
                        m_deepRunning = false;
                        m_deepCapped = capped;
                        m_deepCancel.reset();
                        updateSearchStatus();
                    }
                },
                Qt::QueuedConnection);
            batch.clear();
            sinceSent.restart();
        };
        QStringList dirs{root};
        while (!dirs.isEmpty() && !*cancel && !capped) {
            const QString dir = dirs.takeFirst();
            // No QDir::Hidden: hidden files and folders are left out (dot names; on Windows the hidden attribute)
            QDirIterator it(dir, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
            while (it.hasNext() && !*cancel) {
                const QFileInfo fi = it.nextFileInfo();
                if (fi.isDir()) {
                    if (!fi.isSymLink()) dirs << fi.absoluteFilePath();
                    continue;
                }
                const Kind k = kindOf(fi.fileName());
                if (k == Kind::All || (kind != Kind::All && k != kind)) continue;
                if (!NameMatch::matches(words, fi.fileName())) continue;
                (void)fi.size(); // read here, not on the UI thread when sorting
                (void)fi.lastModified();
                batch << fi;
                if (++found >= kMaxDeepHits) {
                    capped = true;
                    break;
                }
                if (sinceSent.elapsed() >= kDeepBatchMs) send(false);
            }
        }
        if (!*cancel) send(true);
    });
}

void MediaStorage::addDeepHits(const QFileInfoList& hits)
{
    if (hits.isEmpty()) return;
    const QStringList words = NameMatch::queryWords(m_search->text());
    const QDir root(m_folder);
    for (const QFileInfo& fi : hits) {
        m_deepHits << fi;
        if (!kindMatches(fi.fileName())) continue;
        // into its place by the sort order (the list stays sorted while hits come in)
        const auto at = std::upper_bound(m_shownFiles.cbegin(), m_shownFiles.cend(), fi,
                                         [this](const QFileInfo& a, const QFileInfo& b) { return sortsBefore(a, b); });
        const QString rel = root.relativeFilePath(fi.absolutePath());
        const QString sub = QDir::toNativeSeparators(rel == "." ? root.dirName() : root.dirName() + '/' + rel);
        addFileItem(fi, m_dirRows + int(at - m_shownFiles.cbegin()), sub, words);
    }
    updateSearchStatus();
    nextThumb();
}

void MediaStorage::updateSearchStatus()
{
    if (!m_searchBtn->isChecked() || NameMatch::queryWords(m_search->text()).isEmpty()) {
        m_searchStatus->clear();
        return;
    }
    const int n = int(m_shownFiles.size());
    QString text;
    if (m_deepRunning) text = n ? T("Sucht … %1 gefunden").arg(n) : T("Sucht …");
    else if (deepActive() && m_deepCapped) text = T("Die ersten %1 Treffer – genauer suchen").arg(n);
    else if (n == 0 && m_list->count() == 0) text = T("Nichts gefunden");
    else text = T("%1 Treffer").arg(n);
    m_searchStatus->setText(text);
    m_searchStatus->setToolTip(text);
}

void MediaStorage::setSearch(const QString& text)
{
    m_searchBtn->setChecked(!text.isEmpty());
    m_search->setText(text);
}

bool MediaStorage::searchRunning() const
{
    return m_deepRunning || m_deepTimer->isActive();
}

QString MediaStorage::search() const
{
    return m_search->text();
}

void MediaStorage::setSearchSubfolders(bool on)
{
    m_deepBox->setChecked(on);
}

bool MediaStorage::searchSubfolders() const
{
    return m_deepBox->isChecked();
}

void MediaStorage::setKindFilter(Kind kind)
{
    if (const int i = m_kindBox->findData(int(kind)); i >= 0 && i != m_kindBox->currentIndex()) {
        m_kindBox->setCurrentIndex(i); // comes back here
        return;
    }
    if (kind == m_kind) return;
    m_kind = kind;
    if (deepActive()) {
        // the search below filters by kind itself (the cap counts what is shown)
        cancelDeepSearch();
        m_deepHits.clear();
        showFiles();
        startDeepSearch();
    } else {
        showFiles();
    }
}

void MediaStorage::setSort(Sort sort)
{
    for (QAction* a : m_sortBtn->menu()->actions())
        if (a->data().toInt() == int(sort)) a->setChecked(true);
    if (sort == m_sort) return;
    m_sort = sort;
    if (m_persistent) QSettings().setValue("mediaStorage/sort", sortId(sort));
    showFiles();
}

QStringList MediaStorage::selectedFiles() const
{
    QStringList out;
    for (QListWidgetItem* it : m_list->selectedItems())
        if (!it->data(kDirRole).toBool()) out << it->data(kPathRole).toString();
    return out;
}

void MediaStorage::showFileMenu(const QPoint& pos)
{
    QListWidgetItem* at = m_list->itemAt(pos);
    const QStringList files = selectedFiles();
    QMenu menu(this);
    if (!files.isEmpty()) {
        menu.addAction(T("In den Medienpool importieren"), this, [this, files] { emit importRequested(files); });
    }
    if (at && !at->data(kDirRole).toBool()) {
        const QString file = at->data(kPathRole).toString();
        menu.addAction(T("Mit Standardprogramm öffnen"), this, [file] { FileManager::open(file); });
        menu.addAction(T("Im Dateimanager zeigen"), this, [file] { FileManager::reveal(file); });
    }
    if (at && at->data(kDirRole).toBool()) {
        const QString dir = at->data(kPathRole).toString();
        menu.addAction(T("Ordner öffnen"), this, [this, dir] { setFolder(dir); });
        menu.addAction(T("Im Dateimanager zeigen"), this, [dir] { FileManager::reveal(dir); });
        const QStringList media = mediaIn(dir);
        QAction* a = menu.addAction(T("Ordnerinhalt in den Medienpool importieren"), this, [this, media] { emit importRequested(media); });
        a->setEnabled(!media.isEmpty());
        menu.addAction(T("Ordner mit Unterordnern importieren"), this, [this, dir] { emit folderImportRequested(dir); });
        menu.addAction(T("Diesen Ordner überwachen…"), this, [this, dir] { emit watchFolderRequested(dir); });
        if (!m_locations.contains(dir))
            menu.addAction(T("Als Speicherort hinzufügen"), this, [this, dir] {
                m_locations << dir;
                saveLocations();
                rebuildLocations();
                selectFolderInTree(m_folder);
            });
    } else {
        const QStringList media = mediaIn(m_folder);
        QAction* a = menu.addAction(T("Alle Clips dieses Ordners importieren"), this, [this, media] { emit importRequested(media); });
        a->setEnabled(!media.isEmpty());
        if (!m_folder.isEmpty()) {
            menu.addAction(T("Ordner mit Unterordnern importieren"), this,
                           [this, dir = m_folder] { emit folderImportRequested(dir); });
            menu.addAction(T("Diesen Ordner überwachen…"), this, [this, dir = m_folder] { emit watchFolderRequested(dir); });
            menu.addAction(T("Ordner im Dateimanager öffnen"), this, [dir = m_folder] { FileManager::open(dir); });
        }
    }
    menu.addSeparator();
    menu.addAction(T("Aktualisieren"), this, &MediaStorage::rebuildFiles);
    menu.exec(m_list->viewport()->mapToGlobal(pos));
}

void MediaStorage::showLocationMenu(const QPoint& pos)
{
    QTreeWidgetItem* it = m_tree->itemAt(pos);
    QMenu menu(this);
    menu.addAction(T("Speicherort hinzufügen…"), this, &MediaStorage::addLocation);
    if (it && !it->parent())
        menu.addAction(T("Speicherort entfernen"), this, [this, it] { removeLocation(it); });
    if (it) {
        const QString dir = it->data(0, kPathRole).toString();
        if (it->parent() && !m_locations.contains(dir))
            menu.addAction(T("Als Speicherort hinzufügen"), this, [this, dir] {
                m_locations << dir;
                saveLocations();
                rebuildLocations();
                selectFolderInTree(m_folder);
            });
        const QStringList media = mediaIn(dir);
        QAction* a = menu.addAction(T("Ordnerinhalt in den Medienpool importieren"), this, [this, media] { emit importRequested(media); });
        a->setEnabled(!media.isEmpty());
        menu.addAction(T("Ordner mit Unterordnern importieren"), this, [this, dir] { emit folderImportRequested(dir); });
        menu.addAction(T("Diesen Ordner überwachen…"), this, [this, dir] { emit watchFolderRequested(dir); });
        menu.addAction(T("Im Dateimanager zeigen"), this, [dir] { FileManager::reveal(dir); });
    }
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void MediaStorage::queueThumb(const QString& path)
{
    if (!m_thumbQueue.contains(path) && !m_thumbRunning.contains(path)) m_thumbQueue << path;
}

int MediaStorage::nextInView(bool onDiskOnly) const
{
    // What is shown first, then what is nearest to it: scrolled down, the clips coming into view load next and not
    // the ones above that nobody looks at. A list not shown (the panel off, the window not up yet: a new folder starts at
    // the top) simply in its order: asking a hidden list for item places lays out every item, once per thumbnail (a
    // folder of ~150 clips kept the start ~120 ms busy right after the first paint, the panel was not even on)
    if (!m_list->isVisible()) {
        for (int i = 0; i < m_thumbQueue.size(); ++i)
            if (!onDiskOnly || m_thumbOnDisk.contains(m_thumbQueue[i])) return i;
        return -1;
    }
    const QRect view = m_list->viewport()->rect();
    int best = -1;
    int bestDistance = std::numeric_limits<int>::max();
    for (int i = 0; i < m_thumbQueue.size(); ++i) {
        if (onDiskOnly && !m_thumbOnDisk.contains(m_thumbQueue[i])) continue;
        const QListWidgetItem* it = m_items.value(m_thumbQueue[i]);
        if (!it) return i; // gone, dropped at once
        const QRect r = m_list->visualItemRect(it);
        const int distance = r.intersects(view) ? 0
                             : r.bottom() < view.top() ? view.top() - r.bottom()
                                                       : r.top() - view.bottom();
        if (distance == 0) return i;
        if (distance < bestDistance) {
            best = i;
            bestDistance = distance;
        }
    }
    return best;
}

void MediaStorage::nextThumb()
{
    while (m_thumbRunning.size() < m_thumbPool->maxThreadCount() && !m_thumbQueue.isEmpty()) {
        // Pictures on disk first (a small JPEG each, also while the preview is busy), then decoding - later if the
        // preview works (it comes first)
        int index = nextInView(true);
        if (index < 0) {
            if (Background::foregroundBusy()) {
                if (!m_thumbRetry) {
                    m_thumbRetry = true;
                    QTimer::singleShot(200, this, [this] {
                        m_thumbRetry = false;
                        nextThumb();
                    });
                }
                return;
            }
            index = nextInView(false);
        }
        const QString path = m_thumbQueue.takeAt(index);
        const bool onDisk = m_thumbOnDisk.remove(path);
        if (!m_items.contains(path)) continue; // folder changed in the meantime
        // Video: the key frame at 1 s (for very short clips the first one), upright; pictures too in that thread (a
        // folder of big PNGs, which decode at full size, blocked the window for all of them in one go)
        const bool image = imageExt().contains(QFileInfo(path).suffix().toLower());
        m_thumbRunning.insert(path);
        const QString source = image ? path : ProxyManager::decodeSource(path);
        m_thumbPool->start([this, path, source, image, onDisk] {
            Background::lowerThisThread(Background::Level::Low); // own pool: its threads do nothing else
            const QFileInfo file(path);
            QImage img = onDisk ? StorageThumbCache::load(file) : QImage();
            if (img.isNull()) {
                if (image) {
                    QImageReader reader(source);
                    reader.setAutoTransform(true);
                    const QSize size = reader.size();
                    if (size.isValid()) reader.setScaledSize(size.scaled(kThumb, Qt::KeepAspectRatio));
                    img = reader.read();
                    // as framed() shows it: transparent parts over black (JPEG has no alpha), at most kThumb large
                    if (!img.isNull() && (img.width() > kThumb.width() || img.height() > kThumb.height()))
                        img = img.scaled(kThumb, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    if (img.hasAlphaChannel()) {
                        QImage opaque(img.size(), QImage::Format_RGB32);
                        opaque.fill(Qt::black);
                        QPainter(&opaque).drawImage(0, 0, img);
                        img = opaque;
                    }
                } else {
                    img = Video::thumbnailAt(source, 1.0, kThumb);
                }
                if (!img.isNull()) StorageThumbCache::store(file, img);
            }
            QMetaObject::invokeMethod(
                this,
                [this, path, img] {
                    m_thumbRunning.remove(path);
                    thumbDone(path, img.isNull() ? placeholder("?") : framed(img));
                    nextThumb();
                },
                Qt::QueuedConnection);
        });
    }
}

void MediaStorage::thumbDone(const QString& path, const QPixmap& pm)
{
    m_thumbs.insert(thumbKey(QFileInfo(path)), pm);
    if (QListWidgetItem* it = m_items.value(path)) {
        m_shown.insert(path, pm);
        updateItem(it);
    }
}

void MediaStorage::updateItem(QListWidgetItem* it)
{
    const QString path = it->data(kPathRole).toString();
    QPixmap pm = path == m_hoverPath ? m_hoverPixmap : m_shown.value(path);
    QString tip = QDir::toNativeSeparators(path) + "\n"
                  + T("Klick zeigt die Datei in der Vorschau, Doppelklick importiert, in Medienpool oder Timeline ziehen");
    if (inPool(path)) {
        drawPoolMark(pm, m_listMode);
        tip += "\n" + T("Schon im Medienpool");
    }
    it->setIcon(QIcon(pm));
    it->setToolTip(tip);
}

QString MediaStorage::poolKey(const QString& path)
{
    const QString clean = QDir::cleanPath(path);
#ifdef Q_OS_WIN
    return clean.toLower(); // the same file in another spelling
#else
    return clean;
#endif
}

void MediaStorage::setPoolFiles(const QStringList& paths)
{
    QSet<QString> keys;
    keys.reserve(paths.size());
    for (const QString& p : paths) keys.insert(poolKey(p));
    if (keys == m_inPool) return;
    m_inPool = std::move(keys);
    for (QListWidgetItem* it : std::as_const(m_items)) updateItem(it);
}

bool MediaStorage::inPool(const QString& path) const
{
    return m_inPool.contains(poolKey(path));
}
