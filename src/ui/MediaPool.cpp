#include "ui/MediaPool.h"

#include "app/Theme.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Timecode.h"
#include "engine/Engine.h"
#include "engine/ProxyManager.h"

#include <QActionGroup>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
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
constexpr int kColorBarH = 6; // Clipfarbe als Streifen unten im Vorschaubild

class MediaList : public QListWidget {
public:
    using QListWidget::QListWidget;

protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        auto* data = new QMimeData;
        if (items.isEmpty()) return data;
        // Timeline/Viewer: erster Clip (bzw. Titel); Bin-Liste: alle Clips
        data->setData(MediaPool::MimeType, items.first()->data(Qt::UserRole).toString().toUtf8());
        QStringList paths;
        for (QListWidgetItem* it : items) {
            const QString path = it->data(Qt::UserRole).toString();
            if (path != MediaPool::TitleItem) paths << path;
        }
        if (!paths.isEmpty()) data->setData(MediaPool::ListMimeType, paths.join('\n').toUtf8());
        return data;
    }
    QStringList mimeTypes() const override { return {MediaPool::MimeType, MediaPool::ListMimeType}; }
};

// Bin-Liste: Clips oder Bins daraufziehen = verschieben; Entf entfernt den gewählten Bin
class BinTree : public QTreeWidget {
public:
    std::function<void(const QStringList&, int)> mediaDropped;
    std::function<void(int, int)> binDropped;
    std::function<bool(int, int)> canMoveBin; // (Bin, Ziel)
    std::function<void()> deletePressed;

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
            e->ignore(); // Dateien aus dem Dateimanager: Media Pool importiert
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
    bool event(QEvent* e) override
    {
        // Entf/Rücktaste gehören hier dem Bin, nicht dem Timeline-Löschen (Fenster-Kürzel)
        if (e->type() == QEvent::ShortcutOverride && state() != EditingState) {
            const int key = static_cast<QKeyEvent*>(e)->key();
            if (key == Qt::Key_Delete || key == Qt::Key_Backspace) {
                e->accept();
                return true;
            }
        }
        return QTreeWidget::event(e);
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && state() != EditingState) {
            deletePressed();
            return;
        }
        QTreeWidget::keyPressEvent(e);
    }

private:
    void setHover(QTreeWidgetItem* it)
    {
        if (it == m_hover) return;
        if (m_hover) m_hover->setBackground(0, QBrush());
        m_hover = it;
        if (m_hover) m_hover->setBackground(0, QColor(Theme::accent.red(), Theme::accent.green(), Theme::accent.blue(), 90));
    }
    QTreeWidgetItem* m_hover = nullptr; // Drop-Ziel (hervorgehoben)
};

QPixmap placeholderThumb(const QString& label)
{
    QPixmap pm(kThumb);
    pm.fill(QColor(0x18, 0x18, 0x1b));
    QPainter p(&pm);
    p.setPen(QColor(0x8c, 0x8c, 0x94));
    p.drawText(pm.rect(), Qt::AlignCenter, label);
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

    auto* title = new QLabel("Media Pool");
    title->setObjectName("PanelTitle");
    auto* importBtn = new QToolButton;
    importBtn->setText(T("Importieren…"));
    connect(importBtn, &QToolButton::clicked, this, &MediaPool::importDialog);

    // Ansicht wie DaVinci oben rechts im Media Pool: Symbol-/Listenansicht, Sortierung
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

    // Proxy-Erzeugung läuft: Datei + Prozent und Abbrechen im Kopf (wie DaVinci-Fortschrittsanzeige)
    m_proxyStatus = new QLabel;
    m_proxyStatus->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    m_proxyStatus->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_proxyStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); // Pool nicht breiter drücken
    m_proxyCancel = new QToolButton;
    m_proxyCancel->setText(T("Abbrechen"));
    m_proxyCancel->setToolTip(T("Proxy-Erzeugung abbrechen"));
    connect(m_proxyCancel, &QToolButton::clicked, m_engine->proxies(), &ProxyManager::cancelAll);

    // Suche: Lupe blendet das Feld ein, Esc bzw. erneuter Klick leert und schließt es
    m_search = new QLineEdit;
    m_search->setPlaceholderText(T("Suchen…"));
    m_search->setClearButtonEnabled(true);
    m_search->setVisible(false);
    m_search->setMinimumWidth(60);
    m_search->installEventFilter(this);
    connect(m_search, &QLineEdit::textChanged, this, [this] { rebuildClips(); });
    m_searchBtn = new QToolButton;
    m_searchBtn->setText("🔍");
    m_searchBtn->setToolTip(T("Clips suchen"));
    m_searchBtn->setAutoRaise(true);
    m_searchBtn->setCheckable(true);
    connect(m_searchBtn, &QToolButton::toggled, this, [this](bool on) {
        m_search->setVisible(on);
        if (on) m_search->setFocus();
        else m_search->clear();
    });

    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 4, 0);
    header->setSpacing(2);
    header->addWidget(title);
    header->addWidget(m_proxyStatus, 1);
    header->addWidget(m_proxyCancel);
    header->addWidget(m_search, 1);
    header->addWidget(m_searchBtn);
    header->addWidget(m_thumbView);
    header->addWidget(m_listView);
    header->addWidget(m_sortBtn);
    header->addWidget(importBtn);

    // Bin-Liste links (Master oben, Unter-Bins eingerückt)
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
    bins->mediaDropped = [this](const QStringList& paths, int bin) { m_project->moveMediaToBin(paths, bin); };
    bins->binDropped = [this](int id, int parent) { m_project->moveBin(id, parent); };
    bins->canMoveBin = [this](int id, int target) {
        const MediaBin* b = m_project->bin(id);
        return b && b->parent != target && !m_project->binInside(target, id);
    };
    bins->deletePressed = [this] {
        if (QTreeWidgetItem* it = m_bins->currentItem()) removeBin(it->data(0, Qt::UserRole).toInt());
    };
    connect(m_bins, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* it) {
        if (!it || m_bins->signalsBlocked()) return;
        const int id = it->data(0, Qt::UserRole).toInt();
        if (id == m_currentBin) return;
        m_currentBin = id;
        rebuildClips();
    });
    connect(m_bins, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* it) { // Umbenennen per Doppelklick
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

    m_list = new MediaList;
    m_list->setMovement(QListView::Static);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setDragEnabled(true);
    m_list->setDragDropMode(QAbstractItemView::DragOnly);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection); // mehrere Clips für Proxy-Befehle, Bins, Farben
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListWidget::customContextMenuRequested, this, &MediaPool::showContextMenu);
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
        const QString path = it->data(Qt::UserRole).toString();
        if (path != TitleItem) emit sourceRequested(path);
    });

    auto* clipArea = new QWidget;
    auto* clipLay = new QVBoxLayout(clipArea);
    clipLay->setContentsMargins(0, 0, 0, 0);
    clipLay->setSpacing(0);
    clipLay->addWidget(m_binTitle);
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

    connect(m_project, &Project::mediaChanged, this, &MediaPool::rebuild);
    // Organisation geändert: gesammelt nach dem aktuellen Ereignis neu aufbauen (kann aus itemChanged der
    // Bin-Liste kommen – das Element darf dort nicht gelöscht werden)
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
        "*.png *.jpg *.jpeg *.webp *.bmp);;Alle Dateien (*)"));
    importFiles(files);
}

void MediaPool::importFiles(const QStringList& paths)
{
    QStringList failed;
    for (const QString& path : paths) {
        MediaInfo info = m_engine->probe(path);
        if (info.length <= 0 || (!info.hasVideo && !info.hasAudio)) {
            failed << info.name;
            continue;
        }
        info.bin = m_currentBin; // wie DaVinci: Import landet im gewählten Bin
        m_project->addMedia(info);
    }
    if (!failed.isEmpty())
        QMessageBox::warning(this, "Import", T("Nicht lesbar:") + "\n" + failed.join('\n'));
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

bool MediaPool::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == m_search && e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape) {
        m_searchBtn->setChecked(false); // leert und schließt
        return true;
    }
    return QWidget::eventFilter(obj, e);
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
        m_list->setGridSize(QSize(kThumb.width() + 16, kThumb.height() + 48));
        m_list->setWordWrap(true);
        m_list->setSpacing(0);
    }
    m_list->setMovement(QListView::Static);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setDragEnabled(true);
    m_list->setDragDropMode(QAbstractItemView::DragOnly);
}

void MediaPool::rebuild()
{
    m_thumbs.clear(); // neue Medien bzw. neues Projektformat (Seitenverhältnis) -> Vorschaubilder neu
    rebuildBins();
    rebuildClips();
}

void MediaPool::rebuildBins()
{
    if (m_currentBin != 0 && !m_project->bin(m_currentBin)) m_currentBin = 0;
    // Zugeklappte Bins merken (neue sind aufgeklappt)
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
        // Neuer Bin: Eltern aufklappen und Namen gleich bearbeiten (wie DaVinci)
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
    // Auswahl bleibt erhalten (z. B. nach Clipfarbe/Flag für mehrere Clips)
    QSet<QString> selected;
    for (QListWidgetItem* it : m_list->selectedItems()) selected.insert(it->data(Qt::UserRole).toString());
    const int scroll = m_list->verticalScrollBar()->value();
    m_list->clear();
    m_items.clear();
    const QString search = m_search->text().trimmed();
    m_binTitle->setText(search.isEmpty() ? m_project->binName(m_currentBin)
                                         : T("Suche in „%1“: %2").arg(m_project->binName(m_currentBin), search));
    const bool list = m_viewMode == ViewMode::List;

    if (m_currentBin == 0 && search.isEmpty()) {
        // Titel-Generator vorne im Master (in DaVinci unter Effects > Titles > "Text"); ziehen = 5-s-Titel
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

    QVector<const MediaInfo*> media;
    for (const MediaInfo& m : m_project->media()) {
        if (search.isEmpty() ? m.bin == m_currentBin
                             : m_project->binInside(m.bin, m_currentBin) && m.name.contains(search, Qt::CaseInsensitive))
            media << &m;
    }
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
        if (c == 0) c = QString::localeAwareCompare(a->name, b->name);
        return m_sortAscending ? c < 0 : c > 0;
    });

    for (const MediaInfo* m : media) {
        if (!m_thumbs.contains(m->path)) {
            QPixmap thumb;
            const bool offline = !QFileInfo::exists(m->path); // wie DaVinci "Media Offline"
            if (offline) thumb = placeholderThumb("Media Offline");
            else if (m->hasVideo) {
                const QImage img = m_engine->thumbnail(m->path, m->isImage ? 0 : m->length / 3, kThumb);
                if (!img.isNull()) thumb = QPixmap::fromImage(img);
            }
            if (thumb.isNull()) thumb = placeholderThumb(m->hasAudio ? T("♪ Audio") : "?");
            m_thumbs.insert(m->path, thumb);
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
    QPixmap pm = m_thumbs.value(path);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QString tip = path;
    if (const int pct = proxies->progressOf(path); pct >= 0) {
        // In Arbeit/wartend: abgedunkelt, Prozent in der Mitte, Balken unten
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
        p.fillRect(QRect(bar.left(), bar.top(), bar.width() * pct / 100, bar.height()), Theme::accent);
        tip += "\n" + (running ? T("Proxy wird erzeugt: %1 %").arg(pct) : T("Proxy wartet auf Erzeugung"));
    } else if (proxies->hasProxy(path)) {
        // Kleines Proxy-Symbol oben rechts
        const QRectF badge(pm.width() - 22, 4, 18, 14);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x18, 0x18, 0x1b, 220));
        p.drawRoundedRect(badge, 3, 3);
        QFont f = p.font();
        f.setPixelSize(10);
        f.setBold(true);
        p.setFont(f);
        p.setPen(Theme::accent);
        p.drawText(badge, Qt::AlignCenter, "P");
        tip += "\n" + T("Proxy vorhanden");
    }
    if (info) {
        // Clipfarbe als Streifen unten (in der Listenansicht wird das Bild verkleinert -> Streifen dicker)
        if (const TrackColorInfo* c = trackColorInfo(info->clipColor)) {
            const int h = m_viewMode == ViewMode::List ? kColorBarH * 2 : kColorBarH;
            p.fillRect(QRect(0, pm.height() - h, pm.width(), h), QColor::fromRgba(c->rgb));
            tip += "\n" + T("Clipfarbe: %1").arg(T(c->name));
        }
        // Flags oben links nebeneinander (wie DaVinci am Vorschaubild)
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
        if (path != TitleItem) paths << path;
    }
    return paths;
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
        // Haben alle das Flag: entfernen, sonst allen hinzufügen
        QObject::connect(a, &QAction::triggered, project, [project, paths, id, all] { project->setFlag(paths, id, !all); });
    }
    flags->addSeparator();
    QAction* clear = flags->addAction(T("Alle Flags entfernen"));
    clear->setEnabled(any);
    QObject::connect(clear, &QAction::triggered, project, [project, paths] { project->clearFlags(paths); });
}

void MediaPool::showContextMenu(const QPoint& pos)
{
    // Rechtsklick auf einen nicht ausgewählten Clip wählt nur diesen (wie DaVinci)
    if (QListWidgetItem* it = m_list->itemAt(pos); it && !it->isSelected()) {
        m_list->clearSelection();
        it->setSelected(true);
    }
    const QStringList paths = selectedMedia();
    QMenu menu(this);
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
    }
    menu.addSeparator();
    menu.addAction(T("Medien importieren…"), this, &MediaPool::importDialog);
    menu.exec(m_list->viewport()->mapToGlobal(pos));
}

void MediaPool::showBinMenu(const QPoint& pos)
{
    QTreeWidgetItem* it = m_bins->itemAt(pos);
    if (it) m_bins->setCurrentItem(it); // wählt den Bin (wie DaVinci)
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
    // DaVinci löscht die Clips mit; hier wandern sie (und Unter-Bins) in den Eltern-Bin -> nichts geht verloren
    if (clips > 0
        && QMessageBox::question(this, T("Bin entfernen"),
                                 T("Bin „%1“ enthält %2 Clip(s). Sie werden in „%3“ verschoben.")
                                     .arg(name).arg(clips).arg(m_project->binName(parent)),
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
