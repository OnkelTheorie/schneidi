// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/MediaPool.h"
#include "ui/SplitterMemory.h"

#include "app/Theme.h"
#include "core/CompGraph.h"
#include "core/I18n.h"
#include "core/IndexSearch.h"
#include "core/MediaAspect.h"
#include "core/Project.h"
#include "core/ProxyChoice.h"
#include "core/Timecode.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "engine/GradedThumbs.h"
#include "engine/MediaIndex.h"
#include "engine/MediaProbe.h"
#include "engine/ProbeCache.h"
#include "engine/ProxyManager.h"
#include "ui/FileManager.h"
#include "ui/HoverScrub.h"
#include "ui/MediaStorage.h"
#include "ui/ProxyProgress.h"
#include "ui/ElidedLabel.h"
#include "ui/PoolFilterButton.h"
#include "ui/TimelineFormatDialog.h"
#include "core/ProjectFormat.h"
#include "engine/Background.h"

#include <QAction>
#include <QActionGroup>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDrag>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QProgressDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QProgressBar>
#include <QPainterPath>
#include <QPushButton>
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
#include <cmath>
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
            if (!MediaPool::isTitleItem(path)) paths << path; // sequences too: can be moved into bins
        }
        if (!paths.isEmpty()) data->setData(MediaPool::ListMimeType, paths.join('\n').toUtf8());
        return data;
    }
    QStringList mimeTypes() const override { return {MediaPool::MimeType, MediaPool::ListMimeType}; }
    // Never QAbstractItemView::startDrag: a drop that ends as a Move (Shift held over a target that takes the proposed
    // action, a bin) would make it delete the dragged rows itself - the clip gone from the list and MediaPool's
    // path -> item map left with a freed item. The pool rebuilds itself after a move into a bin.
    void startDrag(Qt::DropActions actions) override
    {
        const QList<QListWidgetItem*> items = selectedItems();
        if (items.isEmpty()) return;
        auto* drag = new QDrag(this);
        drag->setMimeData(mimeData(items));
        const QIcon icon = items.first()->icon();
        if (!icon.isNull()) drag->setPixmap(icon.pixmap(iconSize().isValid() ? iconSize() : kListIcon));
        drag->exec(actions, Qt::CopyAction);
    }
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
// aspect > 0: the picture shown at that shape (MediaInfo::pictureAspect, a stretched 4:3 recording)
QPixmap scrubThumb(const QImage& picture, const QPixmap& normal, double fraction, double aspect)
{
    QPixmap pm(kThumb);
    if (picture.isNull()) {
        if (normal.isNull()) pm.fill(Theme::thumbBg);
        else pm = normal;
    } else if (aspect > 0) {
        pm.fill(Qt::black);
        QPainter p(&pm);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const QSizeF size = MediaAspect::fit(aspect, QSizeF(kThumb));
        p.drawImage(QRectF(QPointF((kThumb.width() - size.width()) / 2, (kThumb.height() - size.height()) / 2), size), picture);
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

// Bin icon with a small accent dot at the bottom right: a watch folder brings its new files into this bin
QIcon watchedIcon(const QIcon& folder)
{
    constexpr int kSize = 32; // drawn at twice the list's size: sharp on HiDPI, scaled down otherwise
    QPixmap pm = folder.pixmap(kSize, kSize);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF dot(kSize * 0.52, kSize * 0.52, kSize * 0.46, kSize * 0.46);
    p.setPen(QPen(QColor(0x18, 0x18, 0x1b), 2.5));
    p.setBrush(Theme::primary);
    p.drawEllipse(dot);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x18, 0x18, 0x1b));
    p.drawEllipse(dot.center(), dot.width() * 0.17, dot.width() * 0.17);
    return QIcon(pm);
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

// Entry of a timeline/compound/multicam clip: the type icon, or the timeline's picture (TimelinePosters: first frame
// with its grade) with the type icon as a small badge at the top left
QPixmap sequenceThumb(const Sequence& q, const QImage& picture)
{
    const QColor bg = (q.compound ? Theme::compoundClip : Theme::videoClip).darker(170);
    QPixmap thumb(kThumb);
    thumb.fill(bg);
    QPainter p(&thumb);
    p.setRenderHint(QPainter::Antialiasing);
    if (!picture.isNull()) {
        p.drawImage(QRect(QPoint(0, 0), kThumb), picture);
        constexpr double kBadge = 0.3;
        const QPointF at(4, 4);
        p.setPen(QPen(QColor(0, 0, 0, 140), 1));
        p.setBrush(bg);
        p.drawRoundedRect(QRectF(at, QSizeF(kThumb) * kBadge), 3, 3);
        p.translate(at);
        p.scale(kBadge, kBadge);
    }
    // Icon: timeline = track bars, compound clip = stacked layers, multicam clip = grid of angles
    const QColor fg(0xe8, 0xe8, 0xec);
    if (q.multicam) {
        for (int i = 0; i < 4; ++i) {
            const QRectF r(34 + (i % 2) * 31, 14 + (i / 2) * 27, 29, 25);
            p.setPen(QPen(i == 0 ? Theme::multicamActive : fg, i == 0 ? 2.0 : 1.0));
            p.setBrush(Theme::compoundClip.darker(110 + 20 * i));
            p.drawRect(r);
        }
    } else if (q.compound) {
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
    return thumb;
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

    auto* title = new QLabel(T("Medienpool"));
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
    m_proxyCancel->setObjectName("pool_proxy_cancel");
    m_proxyCancel->setText("✕"); // small: the header is narrow, the text beside it says what runs
    m_proxyCancel->setAutoRaise(true);
    m_proxyCancel->setToolTip(T("Proxy-Erzeugung abbrechen"));
    connect(m_proxyCancel, &QToolButton::clicked, m_engine->proxies(), &ProxyManager::cancelAll);
    // The batch's overall progress as a thin line under the header (never widens the pool)
    m_proxyBar = new QProgressBar;
    m_proxyBar->setObjectName("pool_proxy_bar");
    m_proxyBar->setRange(0, 1000);
    m_proxyBar->setTextVisible(false);
    m_proxyBar->setFixedHeight(2);
    m_proxyBar->setMinimumWidth(0);
    m_proxyBar->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_proxyBar->setStyleSheet(QString("QProgressBar { background: transparent; border: none; }"
                                      "QProgressBar::chunk { background: %1; }")
                                  .arg(Theme::primary.name()));
    m_proxyBar->hide();

    // Search: magnifier shows the field, Esc resp. another click clears and closes it
    m_search = new QLineEdit;
    m_search->setPlaceholderText(T("Im Material suchen…"));
    m_search->setObjectName("pool_search");
    m_search->setClearButtonEnabled(true);
    m_search->setVisible(false);
    title->ensurePolished(); // the field is at least as wide as the title it replaces: the Viewer beside stays put
    m_search->setMinimumWidth(std::max(60, title->sizeHint().width()));
    connect(m_search, &QLineEdit::textChanged, this, [this] {
        rebuildClips();
        m_indexTimer->start(); // the material search after a short pause in typing
    });
    m_searchBtn = new QToolButton;
    m_searchBtn->setText("🔍︎"); // U+FE0E: as text, not as a colored emoji (Windows)
    m_searchBtn->setToolTip(T("Clips und Material durchsuchen"));
    m_searchBtn->setAutoRaise(true);
    m_searchBtn->setCheckable(true);
    // The open field takes the title's place: the header stays as narrow as before (the pool does not widen)
    connect(m_searchBtn, &QToolButton::toggled, this, [this, title](bool on) {
        title->setVisible(!on);
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

    m_binTitle = new ElidedLabel; // long search/filter texts end in "…" (all in the tooltip), never widen the pool
    m_binTitle->setObjectName("pool_bin_title");
    m_binTitle->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    m_binTitle->setContentsMargins(6, 2, 6, 2);
    m_binTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    // Only the cross, like on a filter chip: the narrow pool keeps its room for what is filtered
    m_filterReset = new QToolButton;
    m_filterReset->setObjectName("pool_filter_reset");
    m_filterReset->setText("✕");
    m_filterReset->setToolTip(T("Filter zurücksetzen"));
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
    // Search or filter without a match: says so instead of an empty list (mouse goes through to the list)
    m_emptyHint = new QLabel;
    m_emptyHint->setObjectName("pool_empty_hint");
    m_emptyHint->setAlignment(Qt::AlignCenter);
    m_emptyHint->setWordWrap(true);
    m_emptyHint->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    m_emptyHint->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_emptyHint->hide();
    auto* hintLay = new QVBoxLayout(m_list->viewport());
    hintLay->addWidget(m_emptyHint);
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
        const QString path = it->data(Qt::UserRole).toString();
        if (const int seq = sequenceOfItem(path)) emit sequenceOpenRequested(seq);
        else if (!isTitleItem(path)) emit sourceRequested(path);
    });

    auto* clipArea = new QWidget;
    auto* clipLay = new QVBoxLayout(clipArea);
    clipLay->setContentsMargins(0, 0, 0, 0);
    clipLay->setSpacing(0);
    clipLay->addLayout(titleRow);

    // Material search: the shots of the material index that match the search text, with picture and time
    m_indexPanel = new QWidget;
    m_indexPanel->setObjectName("pool_index_panel");
    auto* indexLay = new QVBoxLayout(m_indexPanel);
    indexLay->setContentsMargins(0, 0, 0, 0);
    indexLay->setSpacing(0);
    m_indexTitle = new QLabel;
    m_indexTitle->setObjectName("pool_index_title");
    m_indexTitle->setWordWrap(true);
    m_indexTitle->setContentsMargins(6, 2, 6, 2);
    m_indexTitle->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    m_indexTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_indexHits = new QListWidget;
    m_indexHits->setObjectName("pool_index_hits");
    m_indexHits->setIconSize(kListIcon);
    m_indexHits->setUniformItemSizes(true);
    m_indexHits->setContextMenuPolicy(Qt::CustomContextMenu);
    m_indexHits->setToolTip(T("Doppelklick: in der Quellvorschau an dieser Stelle zeigen"));
    connect(m_indexHits, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
        emit sourceRequestedAt(it->data(Qt::UserRole).toString(), it->data(Qt::UserRole + 1).toInt());
    });
    connect(m_indexHits, &QListWidget::customContextMenuRequested, this, &MediaPool::showHitMenu);
    indexLay->addWidget(m_indexTitle);
    indexLay->addWidget(m_indexHits, 1);
    m_indexPanel->hide();
    m_indexTimer = new QTimer(this);
    m_indexTimer->setSingleShot(true);
    m_indexTimer->setInterval(200);
    connect(m_indexTimer, &QTimer::timeout, this, &MediaPool::updateIndexHits);
    m_indexPool = new QThreadPool(this);
    m_indexPool->setMaxThreadCount(1); // one file at a time: each decodes on several cores already

    auto* clipSplit = new QSplitter(Qt::Vertical);
    clipSplit->addWidget(m_list);
    clipSplit->addWidget(m_indexPanel);
    clipSplit->setStretchFactor(0, 3);
    clipSplit->setStretchFactor(1, 2);
    clipSplit->setChildrenCollapsible(false);
    SplitterMemory::remember(clipSplit, "mediaPoolIndex", {300, 200}); // the user's drag comes back on the next start
    clipLay->addWidget(clipSplit, 1);

    auto* split = new QSplitter(Qt::Horizontal);
    split->addWidget(m_bins);
    split->addWidget(clipArea);
    split->setStretchFactor(1, 1);
    split->setChildrenCollapsible(false);
    SplitterMemory::remember(split, "mediaPoolBins", {110, 400}); // the user's drag comes back on the next start

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addLayout(header);
    lay->addWidget(m_proxyBar);
    lay->addWidget(split, 1);

    // Keys (bindable, MainWindow registers them): they act only in their widget, Delete in the bin list never
    // reaches the timeline
    m_removeBinKey = new QAction(T("Ausgewählten Ordner entfernen"), this);
    m_removeBinKey->setShortcuts({QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)});
    connect(m_removeBinKey, &QAction::triggered, this, [this] {
        if (QTreeWidgetItem* it = m_bins->currentItem()) removeBin(it->data(0, Qt::UserRole).toInt());
    });
    m_closeSearchKey = new QAction(T("Suche schließen"), this);
    m_closeSearchKey->setShortcut(QKeySequence(Qt::Key_Escape));
    connect(m_closeSearchKey, &QAction::triggered, this, [this] { m_searchBtn->setChecked(false); }); // clears and closes
    m_removeClipsKey = new QAction(T("Aus dem Medienpool entfernen"), this);
    m_removeClipsKey->setShortcuts({QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)});
    connect(m_removeClipsKey, &QAction::triggered, this, &MediaPool::removeSelected);

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
            const MediaInfo* m = m_project->mediaInfo(path);
            if (fraction < 0) m_scrubPicture.remove(path);
            else m_scrubPicture.insert(path, scrubThumb(picture, m_thumbs.value(path), fraction, m ? m->pictureAspect : 0));
            updateItem(path);
        });

    m_thumbPool = new QThreadPool(this);
    m_thumbPool->setMaxThreadCount(2);
    m_thumbPool->setThreadPriority(QThread::LowPriority);
    connect(m_project, &Project::mediaChanged, this, &MediaPool::rebuild);
    // Timelines show their first picture with its grade (sources stay neutral); before the formatChanged below
    m_posters = new TimelinePosters(m_project, kThumb, this);
    connect(m_posters, &TimelinePosters::changed, this, [this](int id) {
        const Sequence* q = m_project->sequence(id);
        for (int i = 0; q && !q->multicam && i < m_list->count(); ++i)
            if (sequenceOfItem(m_list->item(i)->data(Qt::UserRole).toString()) == id)
                m_list->item(i)->setIcon(QIcon(sequenceThumb(*q, m_posters->poster(id))));
    });
    connect(m_project, &Project::formatChanged, this, [this] { // thumbnails anew (frames counted in the new rate)
        ++m_thumbGeneration;
        m_thumbs.clear();
        m_thumbPending.clear();
        m_hitThumbs.clear();
        rebuildClips();
        if (!m_search->text().trimmed().isEmpty()) updateIndexHits(); // the hits' frames are of the old rate
    });
    // Another project, other media: the search shows their hits (the old ones led nowhere)
    const auto hitsAnew = [this] {
        if (!m_search->text().trimmed().isEmpty()) updateIndexHits();
    };
    connect(m_project, &Project::loaded, this, hitsAnew);
    connect(m_project, &Project::mediaChanged, this, hitsAnew);
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
    connect(m_project, &Project::watchFoldersChanged, this, [this] { // the marks of watched bins
        QTimer::singleShot(0, this, [this] { rebuildBins(); });
    });
    ProxyManager* proxies = m_engine->proxies();
    connect(proxies, &ProxyManager::progress, this, [this](const QString& path, int) { updateItem(path); });
    connect(proxies, &ProxyManager::proxyChanged, this, &MediaPool::updateItem);
    connect(proxies, &ProxyManager::qualityChanged, this, [this] { // the proxy marks follow the new quality's files
        for (const QString& path : m_items.keys()) updateItem(path);
    });
    connect(proxies, &ProxyManager::queueChanged, this, &MediaPool::updateProxyStatus);
    connect(ProxyProgress::of(proxies), &ProxyProgress::changed, this, &MediaPool::updateProxyStatus);
    // Failures are not asked about here: the main window reports them without blocking (status line, background jobs)
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

QVector<MediaPool::Unreadable> MediaPool::addFiles(const QStringList& paths, int bin, bool again, int* added)
{
    if (added) *added = 0;
    // Probing opens every file (~1 ms from the header, other containers up to ~0.5 s, a long stream analysis up to a
    // minute): side by side in threads, the window repainting meanwhile (a progress dialog with Cancel when it takes
    // longer; until then no input reaches the window). Added in the given order. A file the drop preview probed
    // already (ProbeCache) is not probed again, unless `again` (Retry).
    const int n = int(paths.size());
    QVector<ProbeResult> results(n);
    QVector<int> todo;
    for (int i = 0; i < n; ++i) {
        std::optional<ProbeResult> known = m_probeCache && !again ? m_probeCache->find(paths[i]) : std::nullopt;
        if (known && (known->usable() || !known->why.isEmpty())) results[i] = *std::move(known); // with its reason
        else todo << i;
    }
    if (!todo.isEmpty()) {
        const FrameRate rate = m_engine->format().rate;
        auto shared = std::make_shared<std::atomic<bool>>(false); // canceled: files not started yet are left out
        std::atomic<int> done{0};
        QVector<bool> skipped(n, false);
        QThreadPool pool;
        pool.setMaxThreadCount(std::clamp(Background::threads(QThread::idealThreadCount() / 2), 1, int(todo.size())));
        ProbeResult* out = results.data(); // detached once here, the threads only write their own element
        bool* skip = skipped.data();
        for (int i : todo)
            pool.start([out, skip, shared, &done, &paths, rate, i] {
                if (shared->load()) skip[i] = true;
                else out[i] = ProbeCache::probe(paths[i], rate);
                ++done;
            });
        const int total = int(todo.size());
        QEventLoop loop;
        QTimer tick;
        tick.setInterval(20);
        QElapsedTimer since;
        since.start();
        std::unique_ptr<QProgressDialog> progress;
        constexpr int kQuietMs = 400; // shorter: no dialog flashes up
        connect(&tick, &QTimer::timeout, &loop, [&] {
            if (done.load() >= total || (!progress && since.elapsed() >= kQuietMs)) loop.quit();
            if (progress) progress->setValue(done.load());
        });
        tick.start();
        // Live requests and watch folders wait meanwhile (MainWindow::updateLiveWaiting, FolderWatch's busy check)
        ++m_probing;
        if (m_probing == 1) emit probingChanged(true);
        if (done.load() < total) loop.exec(QEventLoop::ExcludeUserInputEvents);
        if (done.load() < total) {
            progress = std::make_unique<QProgressDialog>(T("Medien werden gelesen …"), T("Abbrechen"), 0, total, this);
            progress->setWindowModality(Qt::ApplicationModal);
            progress->setMinimumDuration(0);
            progress->setAutoClose(false);
            progress->setAutoReset(false);
            connect(progress.get(), &QProgressDialog::canceled, &loop, [this, shared, &progress] {
                shared->store(true);
                m_importCanceled = true; // a folder import stops with its next folder
                progress->setLabelText(T("Wird abgebrochen …")); // the files being read now finish first
            });
            progress->show();
            loop.exec();
        }
        pool.waitForDone();
        if (--m_probing == 0) emit probingChanged(false);
        for (int i = 0; i < n; ++i)
            if (skipped[i]) results[i] = {}; // left out: neither imported nor reported
        if (m_probeCache)
            for (int i : todo)
                if (!skipped[i]) m_probeCache->insert(results[i]);
    }
    // The bin went meanwhile (nothing should change the project during the wait; to be safe): the top level
    if (bin != 0 && !m_project->bin(bin)) bin = 0;
    QVector<Unreadable> failed;
    for (ProbeResult& r : results) {
        if (r.info.path.isEmpty()) continue; // canceled
        if (!r.usable()) {
            failed << Unreadable{r.info.path, bin, r.why, r.busy};
            continue;
        }
        MediaInfo info = r.info;
        info.bin = bin;
        m_project->addMedia(info);
        if (added) ++*added;
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
    m_importCanceled = false;
    reportUnreadable(addFiles(files, m_currentBin)); // import lands in the selected bin
}

void MediaPool::reportUnreadable(QVector<Unreadable> failed)
{
    using MediaProbe::Busy;
    const auto label = [](const Unreadable& u) {
        return u.busy == Busy::Locked    ? T("von einem anderen Programm gesperrt")
               : u.busy == Busy::Written ? T("gerade erst geschrieben")
                                         : QString();
    };
    while (!failed.isEmpty()) {
        QStringList lines, reasons;
        bool locked = false, written = false;
        for (const Unreadable& u : failed) {
            const QString name = QFileInfo(u.path).fileName(), why = label(u);
            lines << (why.isEmpty() ? name : QString("%1 (%2)").arg(name, why));
            reasons << QDir::toNativeSeparators(u.path) + ": " + (u.why.isEmpty() ? QString("?") : u.why);
            locked = locked || u.busy == Busy::Locked;
            written = written || u.busy == Busy::Written;
        }
        const bool one = failed.size() == 1;
        QString text = one ? T("schneidi konnte in %1 kein Bild und keinen Ton finden.").arg(QFileInfo(failed[0].path).fileName())
                           : T("schneidi konnte in %1 Dateien kein Bild und keinen Ton finden:").arg(failed.size()) + "\n"
                                 + lines.join('\n');
        // The real reason first (a damaged file stays damaged however fresh it is), then what a moment may change
        QString info;
        if (one && failed[0].busy == Busy::Locked) {
            info = T("Ein anderes Programm hält die Datei gerade (Aufnahme, Kopie oder Virenprüfung?). Erneut versuchen?");
        } else {
            if (one && !failed[0].why.isEmpty()) info = T("Grund: %1").arg(failed[0].why) + "\n\n";
            if (locked || written)
                info += one ? T("Die Datei wurde gerade erst geschrieben – läuft die Aufnahme, der Download oder die Kopie "
                                "noch, klappt es vielleicht gleich. Erneut versuchen?")
                            : T("Manche Dateien werden vielleicht noch geschrieben oder sind gesperrt. Erneut versuchen?");
            else
                info += one ? T("Die Datei ist beschädigt oder enthält nichts, was schneidi abspielen kann.")
                            : T("Die Dateien sind beschädigt oder enthalten nichts, was schneidi abspielen kann. Die "
                                "Details nennen die Gründe.");
        }
        QMessageBox box(QMessageBox::Warning, T("Import"), text, QMessageBox::NoButton, this);
        box.setInformativeText(info);
        box.setDetailedText(reasons.join('\n'));
        QPushButton* retry = box.addButton(T("Erneut versuchen"), QMessageBox::AcceptRole);
        QPushButton* close = box.addButton(QMessageBox::Close);
        box.setDefaultButton(locked || written ? retry : close);
        box.exec();
        if (box.clickedButton() != retry) return;
        // Each into the bin it was meant for, as files of that bin together
        QVector<Unreadable> still;
        QMap<int, QStringList> byBin;
        for (const Unreadable& u : failed) byBin[u.bin] << u.path;
        for (auto it = byBin.cbegin(); it != byBin.cend(); ++it) // a bin left empty went: the selected one
            still << addFiles(it.value(), it.key() == 0 || m_project->bin(it.key()) ? it.key() : m_currentBin, true);
        failed = std::move(still);
    }
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
    QVector<Unreadable> failed;
    QUndoStack* undo = m_project->undoStack();
    undo->beginMacro(T("Ordner importieren"));
    m_importCanceled = false;
    importFolderInto(dir, m_currentBin, 0, &failed);
    undo->endMacro();
    reportUnreadable(failed);
}

// Only creates the bin if there is media in the folder (or below); returns whether anything was imported
bool MediaPool::importFolderInto(const QString& dir, int parentBin, int depth, QVector<Unreadable>* failed)
{
    if (depth > 8 || m_importCanceled) return false; // protection against very deep trees; Cancel stops the rest
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
        int added = 0;
        *failed << addFiles(files, bin, false, &added);
        any = added > 0; // canceled files are neither added nor unreadable: an empty bin goes
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
    return {{m_removeBinKey, "pool_remove_bin", QKeySequence(Qt::Key_Delete), m_bins, QKeySequence(Qt::Key_Backspace)},
            {m_closeSearchKey, "pool_close_search", QKeySequence(Qt::Key_Escape), m_search},
            {m_removeClipsKey, "pool_remove_clips", QKeySequence(Qt::Key_Delete), m_list, QKeySequence(Qt::Key_Backspace)}};
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
    m_indexCancel = true; // a file being measured stops at its next picture
    m_indexPool->clear();
    m_indexPool->waitForDone();
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
    const MediaInfo* info = m_project->mediaInfo(path);
    const double aspect = info ? info->pictureAspect : 0; // a stretched 4:3 recording at 4:3
    m_thumbPool->start([this, format, path, frame, generation, aspect] {
        Background::lowerThisThread(Background::Level::Low); // own pool: its threads do nothing else
        Background::yieldToForeground();
        const QImage img = Engine::thumbnail(format, path, frame, kThumb, true, aspect); // in the clip's own shape
        QMetaObject::invokeMethod(this, [this, path, img, generation, aspect] { thumbReady(path, img, generation, aspect); },
                                  Qt::QueuedConnection);
    });
}

void MediaPool::thumbReady(const QString& path, const QImage& img, int generation, double aspect)
{
    if (generation != m_thumbGeneration) return; // format changed meanwhile: requested anew
    m_thumbPending.remove(path);
    const MediaInfo* info = m_project->mediaInfo(path);
    if (!info) return; // removed meanwhile
    m_thumbs.insert(path, img.isNull() ? placeholderThumb(info->hasAudio ? T("♪ Audio") : "?") : QPixmap::fromImage(img));
    m_thumbAspect.insert(path, aspect);
    updateItem(path);
    if (info->pictureAspect != aspect) requestThumb(path, info->isImage ? 0 : info->length / 3); // shape changed meanwhile
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
    // Bins a watch folder fills: marked, the folders in the tooltip
    QHash<int, QStringList> watched;
    for (const WatchFolder& w : m_project->watchFolders())
        watched[w.bin != 0 && m_project->bin(w.bin) ? w.bin : 0] << QDir::toNativeSeparators(w.path);
    const QIcon watchedFolder = watched.isEmpty() ? QIcon() : watchedIcon(folder);
    const auto mark = [&](QTreeWidgetItem* it, int id) {
        if (!watched.contains(id)) return;
        it->setIcon(0, watchedFolder);
        it->setToolTip(0, T("Überwacht – neue Dateien aus:") + "\n" + watched.value(id).join('\n'));
    };
    QTreeWidgetItem* current = nullptr;
    QTreeWidgetItem* edit = nullptr;
    std::function<void(QTreeWidgetItem*, int)> addChildren = [&](QTreeWidgetItem* parent, int parentId) {
        for (int id : m_project->childBins(parentId)) {
            auto* it = new QTreeWidgetItem(parent, {m_project->binName(id)});
            it->setData(0, Qt::UserRole, id);
            it->setIcon(0, folder);
            it->setFlags(it->flags() | Qt::ItemIsEditable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
            mark(it, id);
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
    mark(master, 0);
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
    m_binTitle->setFullText(title);
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
        // Shape (0.22): circle, rectangle, arrow … as a clip of its own
        QPixmap shapeThumb(kThumb);
        shapeThumb.fill(Theme::titleClip.darker(135));
        {
            QPainter p(&shapeThumb);
            p.setRenderHint(QPainter::Antialiasing);
            const double side = std::min(shapeThumb.width(), shapeThumb.height()) * 0.42;
            const QPointF c(shapeThumb.width() / 2.0, shapeThumb.height() / 2.0);
            p.setPen(QPen(QColor(0xf0, 0xf0, 0xf0), 3));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(c + QPointF(-side * 0.35, 0), side / 2, side / 2);
            p.setBrush(QColor(255, 214, 0));
            p.setPen(Qt::NoPen);
            p.drawRect(QRectF(c.x() + side * 0.05, c.y() - side * 0.4, side * 0.8, side * 0.8));
        }
        auto* shapeItem = new QListWidgetItem(QIcon(shapeThumb), T("Form\nKreis, Pfeil …"));
        shapeItem->setData(Qt::UserRole, QString(ShapeItem));
        shapeItem->setToolTip(T("Form (Kreis, Rechteck, Pfeil, Stern …) – in die Timeline ziehen"));
        m_list->addItem(shapeItem);
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
        const QString item = sequenceItem(q->id);
        // Multicam clips keep their grid of angles (which angle would be "the" picture?)
        const QImage picture = q->multicam ? QImage() : m_posters->poster(q->id);
        auto* it = new QListWidgetItem(QIcon(sequenceThumb(*q, picture)), QString());
        it->setData(Qt::UserRole, item); // tool tip: updateSequenceItems (with the resolution)
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
                m_thumbs.insert(m->path, placeholderThumb(T("Datei fehlt")));
                m_offline.insert(m->path);
            } else if (m->hasVideo) {
                m_thumbs.insert(m->path, placeholderThumb(QString())); // until the worker delivers
                requestThumb(m->path, m->isImage ? 0 : m->length / 3);
            } else {
                m_thumbs.insert(m->path, placeholderThumb(m->hasAudio ? T("♪ Audio") : "?"));
            }
        } else if (m->hasVideo && !offline && m_thumbAspect.value(m->path) != m->pictureAspect) {
            requestThumb(m->path, m->isImage ? 0 : m->length / 3); // shown at another shape: the old one until then
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
    m_emptyHint->setText(filter.active() ? T("Nichts passt zum Filter") : T("Nichts passt zur Suche"));
    m_emptyHint->setVisible(narrowed && m_list->count() == 0);
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
    if (m_fresh.contains(path)) { // new from a watch folder (markFresh)
        p.setPen(QPen(Theme::primary, 3));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(pm.rect()).adjusted(1.5, 1.5, -1.5, -1.5));
        tip += "\n" + T("Neu aus einem überwachten Ordner");
    }
    p.end();
    item->setIcon(QIcon(pm));
    item->setToolTip(tip);
}

void MediaPool::markFresh(const QStringList& paths)
{
    for (const QString& path : paths) {
        m_fresh.insert(path);
        updateItem(path);
    }
    QTimer::singleShot(10000, this, [this, paths] {
        for (const QString& path : paths)
            if (m_fresh.remove(path)) updateItem(path);
    });
}

void MediaPool::selectMedia(const QStringList& paths)
{
    if (paths.isEmpty()) return;
    if (const MediaInfo* m = m_project->mediaInfo(paths.first()); m && m->bin != m_currentBin) setCurrentBin(m->bin);
    m_list->clearSelection();
    QListWidgetItem* first = nullptr;
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem* it = m_list->item(i);
        if (!paths.contains(it->data(Qt::UserRole).toString())) continue;
        it->setSelected(true);
        if (!first) first = it;
    }
    if (!first) return;
    m_list->setCurrentItem(first, QItemSelectionModel::Current);
    m_list->scrollToItem(first);
}

void MediaPool::updateProxyStatus()
{
    const ProxyManager* proxies = m_engine->proxies();
    const int n = proxies->pendingCount();
    m_proxyStatus->setVisible(n > 0);
    m_proxyCancel->setVisible(n > 0);
    m_proxyBar->setVisible(n > 0);
    if (n == 0) return;
    const ProxyProgress* batch = ProxyProgress::of(m_engine->proxies());
    m_proxyBar->setValue(int(std::lround(batch->fraction() * 1000)));
    QString text = T("Proxy: %1 – %2 %").arg(QFileInfo(proxies->current()).fileName())
                       .arg(proxies->progressOf(proxies->current()));
    if (n > 1) text += " " + T("(+%1 wartend)").arg(n - 1);
    m_proxyStatus->setText(text);
    m_proxyStatus->setToolTip(text);
}

QRect MediaPool::itemArea(const QString& path) const
{
    QListWidgetItem* it = m_items.value(path);
    if (!it || it->isHidden() || !m_list->isVisible()) return {};
    const QRect r = m_list->visualItemRect(it).intersected(m_list->viewport()->rect());
    if (r.isEmpty()) return {};
    return QRect(m_list->viewport()->mapTo(this, r.topLeft()), r.size());
}

QStringList MediaPool::selectedMedia() const
{
    QStringList paths;
    for (QListWidgetItem* it : m_list->selectedItems()) {
        const QString path = it->data(Qt::UserRole).toString();
        if (!isTitleItem(path) && !sequenceOfItem(path)) paths << path;
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
        // What it is, and its resolution: its own (a 9:16 short) or the project's
        const ProjectFormat own = m_project->sequenceFormat(id);
        const QString size = resolutionLabel(own.width, own.height);
        const QString tip =
            (q->multicam ? T("Multicam-Clip – in die Timeline ziehen legt ihn ab, Doppelklick öffnet die Winkel")
             : q->compound ? T("Verbundclip – Doppelklick öffnet ihn, in die Timeline ziehen legt ihn ab")
                           : T("Timeline – Doppelklick öffnet sie, in eine andere Timeline ziehen verschachtelt sie"))
            + "\n" + (q->hasOwnSize() ? T("Eigene Auflösung: %1").arg(size) : T("Auflösung: %1 (Projekteinstellungen)").arg(size));
        if (it->toolTip() != tip) it->setToolTip(tip);
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
    const QString name = QInputDialog::getText(this, q->compound ? T("Verbundclip umbenennen") : T("Timeline umbenennen"),
                                               T("Name:"), QLineEdit::Normal, q->name, &ok);
    if (ok) m_project->renameSequence(id, name);
}

void MediaPool::editSequenceFormat(int id)
{
    const Sequence* q = m_project->sequence(id);
    if (!q) return;
    TimelineFormatDialog dlg(q->name, q->hasOwnSize() ? QSize(q->width, q->height) : QSize(),
                             m_project->format().size(), this);
    if (dlg.exec() != QDialog::Accepted) return;
    // The project's own size means "follow the project" (as in the CLI), not a fixed size that writes file version 5
    const QSize size = dlg.size();
    m_project->setSequenceSize(id, size == m_project->format().size() ? QSize() : size);
}

void MediaPool::removeSelected()
{
    const QStringList paths = selectedMedia();
    const QVector<int> sequences = selectedSequences();
    if (paths.isEmpty() && sequences.isEmpty()) return;
    // Timelines that go anyway do not count as using the media
    QVector<QPair<int, int>> usage;
    for (const QPair<int, int>& u : m_project->mediaUsage(paths))
        if (!sequences.contains(u.first) || !m_project->canRemoveSequence(u.first)) usage << u;
    if (!usage.isEmpty()) {
        // As a clip's media (the clip goes too) or only referenced inside a clip: a compositing node's file or a
        // title texture (the reference is cleared, the clip stays)
        QSet<QString> used;
        bool referenced = false;
        for (const Sequence& s : m_project->sequences())
            if (std::any_of(usage.cbegin(), usage.cend(), [&](const QPair<int, int>& u) { return u.first == s.id; }))
                for (const auto* tracks : {&s.timeline.video, &s.timeline.audio})
                    for (const Track& t : *tracks)
                        for (const Clip& c : t.clips) {
                            if (c.kind == ClipKind::Media && paths.contains(c.mediaPath)) used.insert(c.mediaPath);
                            QStringList refs;
                            if (c.isTitle() && !c.title.look.texture.isEmpty()) refs << c.title.look.texture;
                            for (const EffectInstance& e : c.effects)
                                if (e.effectId == QLatin1String(Comp::kEffectId)) refs += Comp::mediaPaths(e);
                            for (const QString& r : refs)
                                if (paths.contains(r)) {
                                    used.insert(r);
                                    referenced = true;
                                }
                        }
        QString text = used.size() == 1 ? T("„%1“ wird noch benutzt:").arg(QFileInfo(*used.cbegin()).fileName())
                                        : T("%1 der ausgewählten Clips werden noch benutzt:").arg(used.size());
        for (const auto& [sequence, clips] : usage)
            text += "\n• " + (clips == 1 ? T("%1: 1 Clip").arg(m_project->sequenceName(sequence))
                                         : T("%1: %2 Clips").arg(m_project->sequenceName(sequence)).arg(clips));
        QMessageBox box(QMessageBox::Question, T("Aus dem Medienpool entfernen"), text, QMessageBox::NoButton, this);
        QString info = T("„Mit ihren Clips entfernen“ nimmt diese Clips auch aus den Timelines (ein "
                         "Rückgängig-Schritt holt alles zurück).");
        if (referenced)
            info += ' ' + T("Wird eine Datei nur im Compositing oder als Titel-Textur benutzt, bleibt der Clip; nur dieser "
                            "Verweis wird geleert.");
        box.setInformativeText(info);
        QPushButton* withClips = box.addButton(T("Mit ihren Clips entfernen"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() != withClips) return;
    }
    QUndoStack* undo = m_project->undoStack();
    const bool several = !sequences.isEmpty() && (sequences.size() > 1 || !paths.isEmpty());
    if (several) undo->beginMacro(T("Aus dem Medienpool entfernen"));
    // A compound clip inside a timeline that goes too can go after that timeline
    for (bool progress = true; progress;) {
        progress = false;
        for (int id : sequences)
            if (m_project->sequence(id) && m_project->canRemoveSequence(id)) progress = m_project->removeSequence(id) || progress;
    }
    if (!paths.isEmpty()) m_project->removeMedia(paths, true);
    if (several) undo->endMacro();
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
        menu.addAction(T("Timeline-Auflösung…"), this, [this, id] { editSequenceFormat(id); });
        if (const Sequence* q = m_project->sequence(id); q && !q->compound)
            menu.addAction(T("Als Hochformat-Timeline kopieren (9:16)"), this, [this, id] { emit reframedCopyRequested(id); });
        QAction* del = menu.addAction(T("Löschen"), this, [this, id] { m_project->removeSequence(id); });
        del->setEnabled(m_project->canRemoveSequence(id));
        if (!del->isEnabled())
            del->setToolTip(m_project->sequenceUsed(id) ? T("Wird noch als Clip in einer Timeline benutzt")
                                                        : T("Die letzte Timeline bleibt"));
        menu.setToolTipsVisible(true);
        menu.addSeparator();
    }
    if (paths.size() == 1) { // onto the timeline without dragging (the keys F9/F12/Shift+F12 do the same)
        const QString path = paths.first();
        menu.addAction(T("Am Abspielkopf einfügen") + "\tF9", this, [this, path] { emit editRequested(path, 0); });
        menu.addAction(T("Am Abspielkopf auf freie Spur legen") + "\tF12", this, [this, path] { emit editRequested(path, 1); });
        menu.addAction(T("Ans Ende der Timeline anhängen") + "\tShift+F12", this,
                       [this, path] { emit editRequested(path, 2); });
    }
    // Picture or sound only (a recording whose sound the edit does not need): at the playhead, overwriting
    bool picture = false, sound = false;
    for (const QString& p : paths)
        if (const MediaInfo* m = m_project->mediaInfo(p)) {
            picture |= m->hasVideo;
            sound |= m->hasAudio;
        }
    if (picture && sound) {
        menu.addAction(T("Nur Bild am Abspielkopf platzieren"), this,
                       [this, paths] { emit partsRequested(paths, PlaceParts::Picture); })
            ->setToolTip(T("Ohne Ton – auch: beim Ziehen in die Timeline Shift halten"));
        menu.addAction(T("Nur Ton am Abspielkopf platzieren"), this,
                       [this, paths] { emit partsRequested(paths, PlaceParts::Sound); })
            ->setToolTip(T("Ohne Bild – auch: beim Ziehen in die Timeline Strg halten"));
        menu.setToolTipsVisible(true);
    }
    if (m_leaveOutSound) menu.addAction(m_leaveOutSound);
    if (!paths.isEmpty()) menu.addSeparator();
    menu.addAction(T("Neue Timeline"), this, &MediaPool::newTimeline);
    // A short next to the edit: its own 9:16 resolution, the project and the other timelines stay
    menu.addAction(T("Neue Hochformat-Timeline (9:16)"), this,
                   [this] { m_project->addTimeline({}, m_currentBin, QSize(1080, 1920)); });
    menu.addAction(T("Neuer Ordner"), this, &MediaPool::newBin);
    if (!paths.isEmpty()) {
        menu.addAction(T("Neuer Ordner mit ausgewählten Clips"), this, [this, paths] {
            QUndoStack* undo = m_project->undoStack();
            undo->beginMacro(T("Neuer Ordner mit ausgewählten Clips"));
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
        // Stretched 4:3 game footage: these files (their tiles and new clips) and their clips in the open timeline at
        // their true shape
        QStringList videos;
        bool allStretched = true;
        for (const QString& path : paths)
            if (const MediaInfo* m = m_project->mediaInfo(path); m && m->hasVideo && !m->isImage) {
                videos << path;
                allStretched = allStretched && m->pictureAspect > 0;
            }
        QAction* stretched = menu.addAction(T("Gestrecktes 4:3 entzerren"));
        stretched->setCheckable(true);
        stretched->setChecked(!videos.isEmpty() && allStretched);
        stretched->setEnabled(!videos.isEmpty());
        connect(stretched, &QAction::triggered, this, [this, videos](bool on) { emit stretchedRequested(videos, on); });
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
        // Greyed out with the reason in its text (no video, all there, being made)
        menu.addAction(ProxyChoice::label(T("Proxy-Medien erzeugen"), canGenerate.size() + withProxy.size() + pending.size(),
                                          canGenerate.size(), pending.size()),
                       this, [proxies, canGenerate] { proxies->generate(canGenerate); })
            ->setEnabled(!canGenerate.isEmpty());
        menu.addAction(T("Proxy-Medien löschen"), this, [proxies, withProxy] { proxies->remove(withProxy); })
            ->setEnabled(!withProxy.isEmpty());
        if (!pending.isEmpty())
            menu.addAction(T("Proxy-Erzeugung abbrechen"), this, [proxies, pending] { proxies->cancel(pending); });
        menu.addAction(T("Proxy-Medien aufräumen…"), this, &MediaPool::proxyCleanupRequested);
        menu.addSeparator();
        QStringList indexable;
        for (const QString& path : paths)
            if (const MediaInfo* m = m_project->mediaInfo(path);
                m && (m->hasVideo || m->hasAudio) && !m_indexing.contains(path) && QFileInfo::exists(path))
                indexable << path;
        menu.addAction(T("Material-Index erstellen"), this, [this, indexable] { buildIndex(indexable); })
            ->setEnabled(!indexable.isEmpty());
        menu.addSeparator();
        QString file = paths.first(); // the one right-clicked, else the first selected
        if (QListWidgetItem* it = m_list->itemAt(pos); it && paths.contains(it->data(Qt::UserRole).toString()))
            file = it->data(Qt::UserRole).toString();
        const bool exists = QFileInfo::exists(file);
        menu.addAction(T("Mit Standardprogramm öffnen"), this, [file] { FileManager::open(file); })->setEnabled(exists);
        menu.addAction(T("Im Dateimanager zeigen"), this, [file] { FileManager::reveal(file); })->setEnabled(exists);
        menu.addSeparator();
        QAction* remove = menu.addAction(T("Aus dem Medienpool entfernen"), this, &MediaPool::removeSelected);
        remove->setShortcuts(m_removeClipsKey->shortcuts()); // shows the key (acts only while the menu is open)
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
    menu.addAction(T("Neuer Ordner"), this, &MediaPool::newBin);
    if (id != 0) {
        menu.addAction(T("Ordner umbenennen"), this, [this, it] { m_bins->editItem(it); });
        menu.addAction(T("Ordner entfernen"), this, [this, id] { removeBin(id); });
    }
    menu.addSeparator();
    menu.addAction(T("Ordner überwachen…"), this, [this, id] { emit watchFolderRequested(id); });
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
        && QMessageBox::question(this, T("Ordner entfernen"),
                                 T("Ordner „%1“ enthält %2 Clip(s). Sie werden in „%3“ verschoben.")
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

// ---------- Material search (engine/MediaIndex) ----------

namespace {

QString clockOf(double seconds)
{
    const int s = std::max(0, int(seconds));
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

QString eventName(const QString& kind)
{
    static const QHash<QString, const char*> names{
        {"laughter", N_("Lachen")}, {"applause", N_("Applaus")}, {"cheering", N_("Jubel")}, {"shot", N_("Schuss")},
        {"scream", N_("Schrei")},   {"music", N_("Musik")},      {"speech", N_("Sprechen")}, {"silence", N_("Stille")}};
    const auto it = names.constFind(kind);
    return it == names.constEnd() ? kind : T(*it);
}

QString qualityName(const QString& q)
{
    static const QHash<QString, const char*> names{
        {"calm", N_("ruhig")},       {"motion", N_("viel Bewegung")}, {"dark", N_("dunkel")},
        {"overexposed", N_("überbelichtet")}, {"blurry", N_("unscharf")}, {"sharp", N_("scharf")},
        {"pan", N_("Schwenk")},      {"face likely", N_("Gesicht wahrscheinlich")}, {"speech", N_("Sprechen")}};
    const auto it = names.constFind(q);
    return it == names.constEnd() ? q : T(*it);
}

QString reasonText(const IndexSearch::Reason& r)
{
    if (r.field == "tag") return T("Schlagwort „%1“").arg(r.term);
    if (r.field == "desc") return T("Beschreibung „%1“").arg(r.term);
    if (r.field == "speech") return T("Gesagt „%1“").arg(r.term);
    if (r.field == "event") return T("Ton: %1").arg(eventName(r.term));
    return T("Bild: %1").arg(qualityName(r.term));
}

} // namespace

void MediaPool::buildIndex(const QStringList& paths)
{
    for (const QString& path : paths) {
        if (m_indexing.contains(path)) continue;
        m_indexing.insert(path);
        m_indexPool->start([this, path] {
            Background::lowerThisThread(Background::Level::Lowest); // own pool: its thread does nothing else
            Background::yieldToForeground();
            MediaIndex::Options o;
            o.progress = [this](double) { return !m_indexCancel.load(); };
            MediaIndex::build(path, o);
            QMetaObject::invokeMethod(
                this,
                [this, path] {
                    m_indexing.remove(path);
                    if (!m_search->text().trimmed().isEmpty()) updateIndexHits();
                    emit indexBuilt(path);
                },
                Qt::QueuedConnection);
        });
    }
    if (!m_search->text().trimmed().isEmpty()) updateIndexHits();
}

void MediaPool::updateIndexHits()
{
    const QString query = m_search->text().trimmed();
    m_indexHits->clear();
    if (query.isEmpty()) {
        m_indexPanel->hide();
        return;
    }
    m_indexPanel->show();
    QVector<IndexSearch::Shot> shots;
    int indexed = 0;
    for (const MediaInfo& m : m_project->media()) {
        std::optional<MediaIndex::Entry> e = MediaIndex::cached(m.path);
        if (!e) continue;
        e->file = m.path; // the project's spelling of the path (the index cleans it: "./", the drive letter's case)
        ++indexed;
        for (const MediaTranscript& t : m.transcripts) MediaIndex::addSpeech(*e, t.words);
        shots += MediaIndex::searchable(*e);
    }
    const QVector<IndexSearch::Hit> hits = IndexSearch::search(query, shots, 50);
    QString title = !indexed         ? T("Noch kein Material-Index: Rechtsklick auf Clips › Material-Index erstellen")
                    : hits.isEmpty() ? T("Im Material: nichts gefunden")
                                     : T("Im Material: %1 Treffer").arg(hits.size());
    if (!m_indexing.isEmpty()) title += "  " + T("(Index wird erstellt: %1)").arg(m_indexing.size());
    m_indexTitle->setText(title);

    const ProjectFormat format = m_project->format();
    const double fps = format.rate.fps();
    for (const IndexSearch::Hit& h : hits) {
        const MediaInfo* m = m_project->mediaInfo(h.file);
        if (!m) continue;
        const int frame = std::clamp(int(std::lround(h.from * fps)), 0, std::max(0, m->length - 1));
        QStringList why;
        for (const IndexSearch::Reason& r : h.reasons) why << reasonText(r);
        auto* it = new QListWidgetItem(
            QString("%1  %2–%3\n%4").arg(m->name, clockOf(h.from), clockOf(h.to), why.join(", ")), m_indexHits);
        it->setData(Qt::UserRole, h.file);
        it->setData(Qt::UserRole + 1, frame);
        it->setData(Qt::UserRole + 2, h.n);
        it->setToolTip(T("Einstellung %1 von %2").arg(h.n).arg(m->name));
        const QString key = h.file + '#' + QString::number(frame);
        it->setData(Qt::UserRole + 3, key);
        if (const auto pm = m_hitThumbs.constFind(key); pm != m_hitThumbs.constEnd()) {
            it->setIcon(*pm);
        } else if (m->hasVideo && !m->isImage && QFileInfo::exists(h.file)) {
            if (m_thumbs.contains(h.file)) it->setIcon(m_thumbs.value(h.file)); // until its own picture is there
            const double aspect = m->pictureAspect;
            const QString path = h.file;
            m_hitThumbs.insert(key, m_thumbs.value(h.file)); // asked once
            const int generation = m_thumbGeneration;
            m_thumbPool->start([this, format, path, frame, key, aspect, generation] {
                Background::lowerThisThread(Background::Level::Low);
                const QImage img = Engine::thumbnail(format, path, frame, kThumb, true, aspect);
                QMetaObject::invokeMethod(
                    this,
                    [this, key, img, generation] {
                        if (img.isNull() || generation != m_thumbGeneration) return; // a picture of the old rate
                        const QPixmap pm = QPixmap::fromImage(img);
                        m_hitThumbs.insert(key, pm);
                        for (int i = 0; i < m_indexHits->count(); ++i)
                            if (m_indexHits->item(i)->data(Qt::UserRole + 3).toString() == key)
                                m_indexHits->item(i)->setIcon(pm);
                    },
                    Qt::QueuedConnection);
            });
        } else {
            it->setIcon(m_thumbs.value(h.file));
        }
    }
}

void MediaPool::showHitMenu(const QPoint& pos)
{
    QListWidgetItem* it = m_indexHits->itemAt(pos);
    if (!it) return;
    const QString path = it->data(Qt::UserRole).toString();
    const int frame = it->data(Qt::UserRole + 1).toInt(), shot = it->data(Qt::UserRole + 2).toInt();
    QMenu menu(this);
    menu.addAction(T("In der Quellvorschau zeigen"), this, [this, path, frame] { emit sourceRequestedAt(path, frame); });
    menu.addAction(T("Beschreibung bearbeiten…"), this, [this, path, shot] { editHitNote(path, shot); });
    menu.exec(m_indexHits->viewport()->mapToGlobal(pos));
}

void MediaPool::editHitNote(const QString& path, int shot)
{
    std::optional<MediaIndex::Entry> e = MediaIndex::cached(path);
    if (!e) return;
    const MediaIndex::Shot* s = nullptr;
    for (const MediaIndex::Shot& x : e->shots)
        if (x.n == shot) s = &x;
    if (!s) {
        QMessageBox::warning(this, T("Beschreibung bearbeiten"),
                             T("Die Einstellung %1 gibt es in %2 nicht mehr (die Datei wurde neu untersucht).")
                                 .arg(shot)
                                 .arg(QFileInfo(path).fileName()));
        return;
    }
    QDialog dlg(this);
    dlg.setObjectName("index_note_dialog");
    dlg.setWindowTitle(T("Beschreibung bearbeiten"));
    auto* form = new QFormLayout(&dlg);
    auto* desc = new QLineEdit(s->desc);
    desc->setObjectName("index_note_desc");
    desc->setMinimumWidth(320);
    auto* tags = new QLineEdit(s->tags.join(", "));
    tags->setObjectName("index_note_tags");
    tags->setPlaceholderText(T("durch Kommas getrennt"));
    form->addRow(new QLabel(T("Einstellung %1 von %2").arg(shot).arg(QFileInfo(path).fileName())));
    form->addRow(T("Beschreibung"), desc);
    form->addRow(T("Schlagworte"), tags);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted) return;
    QStringList list;
    for (const QString& t : tags->text().split(',', Qt::SkipEmptyParts)) list << t.trimmed();
    QString error;
    // The shot is there (looked up above): what fails is writing the note next to the index (English for the CLI)
    if (!MediaIndex::annotate(*e, shot, desc->text(), list, &error))
        QMessageBox::warning(this, T("Beschreibung bearbeiten"),
                             T("Die Beschreibung ließ sich nicht speichern: in den Ordner %1 kann schneidi nicht "
                               "schreiben.")
                                 .arg(QDir::toNativeSeparators(MediaIndex::notesDir())));
    updateIndexHits();
}
