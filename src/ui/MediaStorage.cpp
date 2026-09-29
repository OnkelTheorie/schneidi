#include "ui/MediaStorage.h"
#include "engine/Bundle.h"

#include "app/Theme.h"
#include "core/I18n.h"

#include <QDateTime>
#include <QEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QProcess>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace {

constexpr QSize kThumb{144, 81}; // wie im Media Pool
constexpr QSize kListIcon{48, 27};
constexpr int kPathRole = Qt::UserRole;
constexpr int kDirRole = Qt::UserRole + 1;
constexpr int kLoadedRole = Qt::UserRole + 2;

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
        // schlichtes Ordnersymbol
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

// Dateiliste: Ziehen liefert Datei-URLs (Media Pool und Timeline nehmen sie wie aus dem Dateimanager)
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
    const QString ext = QFileInfo(path).suffix().toLower();
    return videoExt().contains(ext) || audioExt().contains(ext) || imageExt().contains(ext);
}

MediaStorage::MediaStorage(QWidget* parent) : QWidget(parent)
{
    setObjectName("Panel");

    auto* title = new QLabel("Media Storage");
    title->setObjectName("PanelTitle");
    auto* addBtn = new QToolButton;
    addBtn->setText(T("+ Ordner"));
    addBtn->setToolTip(T("Speicherort hinzufügen…"));
    connect(addBtn, &QToolButton::clicked, this, &MediaStorage::addLocation);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 4, 0);
    header->setSpacing(2);
    // Symbol-/Listenansicht wie im Media Pool (Liste passt in die schmale Spalte der Edit-Seite)
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

    header->addWidget(title);
    header->addStretch(1);
    header->addWidget(m_thumbBtn);
    header->addWidget(m_listBtn);
    header->addWidget(addBtn);

    // Links: Speicherorte, Unterordner werden erst beim Aufklappen gelesen
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

    // Rechts: Pfad mit „eine Ebene hoch“, darunter der Ordnerinhalt
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
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setDragEnabled(true);
    m_list->setDragDropMode(QAbstractItemView::DragOnly);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
        const QString path = it->data(kPathRole).toString();
        if (it->data(kDirRole).toBool()) setFolder(path);
        else emit importRequested({path});
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
    split->setSizes({110, 400});
    split->setCollapsible(1, false); // Ordnerbaum lässt sich in schmalen Spalten ganz zuschieben

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addLayout(header);
    lay->addWidget(split, 1);

    // Neue/gelöschte Dateien im offenen Ordner (z. B. frisch vom Handy kopiert) erscheinen von selbst
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
        // Erster Start: Videos-Ordner (falls vorhanden) und persönlicher Ordner
        const QString movies = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
        if (!movies.isEmpty() && QFileInfo(movies).isDir()) m_locations << movies;
        m_locations << QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    }
    rebuildLocations();
    QString last = s.value("mediaStorage/folder").toString();
    if (last.isEmpty() || !QFileInfo(last).isDir()) last = m_locations.value(0);
    m_savedFolder = QDir::cleanPath(last);
    setFolder(last);
}

MediaStorage::~MediaStorage()
{
    if (m_ffmpeg) {
        m_ffmpeg->disconnect(this);
        m_ffmpeg->kill();
        m_ffmpeg->waitForFinished(1000);
    }
}

void MediaStorage::setListMode(bool list)
{
    m_listMode = list;
    m_thumbBtn->setChecked(!list);
    m_listBtn->setChecked(list);
    if (list) {
        m_list->setViewMode(QListView::ListMode);
        m_list->setIconSize(kListIcon);
        m_list->setGridSize(QSize());
        m_list->setWordWrap(false);
        m_list->setSpacing(1);
    } else {
        m_list->setViewMode(QListView::IconMode);
        m_list->setIconSize(kThumb);
        m_list->setGridSize(QSize(kThumb.width() + 16, kThumb.height() + 36));
        m_list->setWordWrap(true);
        m_list->setSpacing(0);
    }
    // setViewMode setzt diese zurück
    m_list->setMovement(QListView::Static);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setDragEnabled(true);
    m_list->setDragDropMode(QAbstractItemView::DragOnly);
}

// Pfad von links kürzen: das Ende (aktueller Ordner) ist das Wichtige
void MediaStorage::updatePathLabel()
{
    const QString native = QDir::toNativeSeparators(m_folder);
    m_pathLabel->setText(m_pathLabel->fontMetrics().elidedText(native, Qt::ElideLeft, std::max(20, m_pathLabel->width() - 10)));
    m_pathLabel->setToolTip(native);
}

bool MediaStorage::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == m_pathLabel && e->type() == QEvent::Resize) updatePathLabel();
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
        if (!QFileInfo(loc).isDir()) it->setForeground(0, Theme::textDim); // z. B. abgezogener USB-Stick
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

// Ordner im Baum markieren (unter dem passenden Speicherort aufklappen), ohne setFolder erneut auszulösen
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
    QStringList selected = selectedFiles();
    m_list->clear();
    m_items.clear();
    m_thumbQueue.clear();
    if (m_folder.isEmpty()) return;

    const QDir dir(m_folder);
    for (const QFileInfo& fi : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase | QDir::LocaleAware)) {
        auto* it = new QListWidgetItem(QIcon(placeholder({}, true)), fi.fileName());
        it->setData(kPathRole, fi.absoluteFilePath());
        it->setData(kDirRole, true);
        it->setToolTip(T("Ordner öffnen (Doppelklick)"));
        m_list->addItem(it);
    }
    for (const QString& path : mediaIn(m_folder)) {
        const QFileInfo fi(path);
        const QString ext = fi.suffix().toLower();
        QPixmap pm = m_thumbs.value(thumbKey(fi));
        if (pm.isNull()) {
            if (audioExt().contains(ext)) {
                pm = placeholder(T("♪ Audio"));
            } else {
                pm = placeholder("…");
                queueThumb(path);
            }
        }
        auto* it = new QListWidgetItem(QIcon(pm), fi.fileName());
        it->setData(kPathRole, path);
        it->setToolTip(QDir::toNativeSeparators(path) + "\n" + T("In Media Pool oder Timeline ziehen, Doppelklick importiert"));
        m_list->addItem(it);
        m_items.insert(path, it);
        if (selected.contains(path)) it->setSelected(true);
    }
    nextThumb();
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
        menu.addAction(T("In den Media Pool importieren"), this, [this, files] { emit importRequested(files); });
    }
    if (at && at->data(kDirRole).toBool()) {
        const QString dir = at->data(kPathRole).toString();
        menu.addAction(T("Ordner öffnen"), this, [this, dir] { setFolder(dir); });
        const QStringList media = mediaIn(dir);
        QAction* a = menu.addAction(T("Ordnerinhalt in den Media Pool importieren"), this, [this, media] { emit importRequested(media); });
        a->setEnabled(!media.isEmpty());
        menu.addAction(T("Ordner mit Unterordnern als Bins importieren"), this, [this, dir] { emit folderImportRequested(dir); });
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
        if (!m_folder.isEmpty())
            menu.addAction(T("Ordner mit Unterordnern als Bins importieren"), this,
                           [this, dir = m_folder] { emit folderImportRequested(dir); });
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
        QAction* a = menu.addAction(T("Ordnerinhalt in den Media Pool importieren"), this, [this, media] { emit importRequested(media); });
        a->setEnabled(!media.isEmpty());
        menu.addAction(T("Ordner mit Unterordnern als Bins importieren"), this, [this, dir] { emit folderImportRequested(dir); });
    }
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void MediaStorage::queueThumb(const QString& path)
{
    if (!m_thumbQueue.contains(path) && path != m_thumbPath) m_thumbQueue << path;
}

void MediaStorage::nextThumb()
{
    while (!m_ffmpeg && !m_thumbQueue.isEmpty()) {
        const QString path = m_thumbQueue.takeFirst();
        if (!m_items.contains(path)) continue; // Ordner inzwischen gewechselt
        const QString ext = QFileInfo(path).suffix().toLower();
        if (imageExt().contains(ext)) {
            QImageReader reader(path);
            reader.setAutoTransform(true);
            const QSize size = reader.size();
            if (size.isValid()) reader.setScaledSize(size.scaled(kThumb, Qt::KeepAspectRatio));
            const QImage img = reader.read();
            thumbDone(path, img.isNull() ? placeholder("?") : framed(img));
            continue;
        }
        // Video: ein Bild bei 1 s (bei sehr kurzen Clips das erste), ffmpeg dreht Handyvideos selbst richtig
        m_thumbPath = path;
        m_ffmpeg = new QProcess(this);
        auto run = [this, path](bool atStart) {
            QStringList args{"-hide_banner", "-nostdin", "-loglevel", "error"};
            if (!atStart) args << "-ss" << "1";
            args << "-i" << path << "-frames:v" << "1" << "-vf"
                 << QString("scale=%1:%2:force_original_aspect_ratio=decrease").arg(kThumb.width()).arg(kThumb.height())
                 << "-f" << "image2pipe" << "-c:v" << "png" << "-";
            m_ffmpeg->start(Bundle::tool("ffmpeg"), args);
        };
        connect(m_ffmpeg, &QProcess::finished, this, [this, path, run] {
            QImage img;
            img.loadFromData(m_ffmpeg->readAllStandardOutput(), "PNG");
            if (img.isNull() && !m_ffmpeg->property("atStart").toBool()) {
                m_ffmpeg->setProperty("atStart", true);
                run(true); // kürzer als 1 s
                return;
            }
            m_ffmpeg->deleteLater();
            m_ffmpeg = nullptr;
            m_thumbPath.clear();
            thumbDone(path, img.isNull() ? placeholder("?") : framed(img));
            nextThumb();
        });
        connect(m_ffmpeg, &QProcess::errorOccurred, this, [this, path](QProcess::ProcessError e) {
            if (e != QProcess::FailedToStart) return; // Absturz o. Ä. meldet finished
            m_ffmpeg->deleteLater();
            m_ffmpeg = nullptr;
            m_thumbPath.clear();
            // kein ffmpeg -> gar nicht erst weiter versuchen
            for (const QString& p : std::exchange(m_thumbQueue, {}) << path) thumbDone(p, placeholder(T("Video")));
        });
        run(false);
    }
}

void MediaStorage::thumbDone(const QString& path, const QPixmap& pm)
{
    m_thumbs.insert(thumbKey(QFileInfo(path)), pm);
    if (QListWidgetItem* it = m_items.value(path)) it->setIcon(QIcon(pm));
}
