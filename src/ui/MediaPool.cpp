#include "ui/MediaPool.h"

#include "app/Theme.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Timecode.h"
#include "engine/Engine.h"
#include "engine/ProxyManager.h"

#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QMessageBox>
#include <QPainter>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr QSize kThumb{144, 81};

class MediaList : public QListWidget {
public:
    using QListWidget::QListWidget;

protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        auto* data = new QMimeData;
        if (!items.isEmpty()) data->setData(MediaPool::MimeType, items.first()->data(Qt::UserRole).toString().toUtf8());
        return data;
    }
    QStringList mimeTypes() const override { return {MediaPool::MimeType}; }
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

} // namespace

MediaPool::MediaPool(Project* project, Engine* engine, QWidget* parent)
    : QWidget(parent), m_project(project), m_engine(engine)
{
    setObjectName("Panel");
    setAcceptDrops(true);

    auto* title = new QLabel("Media Pool");
    title->setObjectName("PanelTitle");
    auto* importBtn = new QToolButton;
    importBtn->setText(T("Importieren…"));
    connect(importBtn, &QToolButton::clicked, this, &MediaPool::importDialog);

    // Proxy-Erzeugung läuft: Datei + Prozent und Abbrechen im Kopf (wie DaVinci-Fortschrittsanzeige)
    m_proxyStatus = new QLabel;
    m_proxyStatus->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    m_proxyStatus->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_proxyStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); // Pool nicht breiter drücken
    m_proxyCancel = new QToolButton;
    m_proxyCancel->setText(T("Abbrechen"));
    m_proxyCancel->setToolTip(T("Proxy-Erzeugung abbrechen"));
    connect(m_proxyCancel, &QToolButton::clicked, m_engine->proxies(), &ProxyManager::cancelAll);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 4, 0);
    header->addWidget(title);
    header->addWidget(m_proxyStatus, 1);
    header->addWidget(m_proxyCancel);
    header->addWidget(importBtn);

    m_list = new MediaList;
    m_list->setViewMode(QListView::IconMode);
    m_list->setIconSize(kThumb);
    m_list->setGridSize(QSize(kThumb.width() + 16, kThumb.height() + 48));
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    m_list->setDragEnabled(true);
    m_list->setDragDropMode(QAbstractItemView::DragOnly);
    m_list->setWordWrap(true);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection); // mehrere Clips für Proxy-Befehle
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListWidget::customContextMenuRequested, this, &MediaPool::showContextMenu);
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
        const QString path = it->data(Qt::UserRole).toString();
        if (path != TitleItem) emit sourceRequested(path);
    });

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addLayout(header);
    lay->addWidget(m_list, 1);

    connect(m_project, &Project::mediaChanged, this, &MediaPool::rebuild);
    ProxyManager* proxies = m_engine->proxies();
    connect(proxies, &ProxyManager::progress, this, [this](const QString& path, int) { updateItem(path); });
    connect(proxies, &ProxyManager::proxyChanged, this, &MediaPool::updateItem);
    connect(proxies, &ProxyManager::queueChanged, this, &MediaPool::updateProxyStatus);
    connect(proxies, &ProxyManager::failed, this, [this](const QString& path, const QString& msg) {
        QMessageBox::warning(this, T("Proxy-Medien"), QFileInfo(path).fileName() + "\n" + msg);
    });
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
        const MediaInfo info = m_engine->probe(path);
        if (info.length <= 0 || (!info.hasVideo && !info.hasAudio)) {
            failed << info.name;
            continue;
        }
        m_project->addMedia(info);
    }
    if (!failed.isEmpty())
        QMessageBox::warning(this, "Import", T("Nicht lesbar:") + "\n" + failed.join('\n'));
}

void MediaPool::rebuild()
{
    m_list->clear();
    m_items.clear();
    m_thumbs.clear();
    // Titel-Generator vorne (in DaVinci unter Effects > Titles > "Text"); ziehen = 5-s-Titel
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

    for (const MediaInfo& m : m_project->media()) {
        QPixmap thumb;
        const bool offline = !QFileInfo::exists(m.path); // wie DaVinci "Media Offline"
        if (offline) thumb = placeholderThumb("Media Offline");
        else if (m.hasVideo) {
            const QImage img = m_engine->thumbnail(m.path, m.isImage ? 0 : m.length / 3, kThumb);
            if (!img.isNull()) thumb = QPixmap::fromImage(img);
        }
        if (thumb.isNull()) thumb = placeholderThumb(m.hasAudio ? T("♪ Audio") : "?");

        auto* item = new QListWidgetItem(m.name + "\n" + Timecode::format(m.length, m_project->fps()));
        item->setData(Qt::UserRole, m.path);
        m_list->addItem(item);
        m_items.insert(m.path, item);
        m_thumbs.insert(m.path, thumb);
        updateItem(m.path);
    }
}

void MediaPool::updateItem(const QString& path)
{
    QListWidgetItem* item = m_items.value(path);
    if (!item) return;
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

void MediaPool::showContextMenu(const QPoint& pos)
{
    // Rechtsklick auf einen nicht ausgewählten Clip wählt nur diesen (wie DaVinci)
    if (QListWidgetItem* it = m_list->itemAt(pos); it && !it->isSelected()) {
        m_list->clearSelection();
        it->setSelected(true);
    }
    const QStringList paths = selectedMedia();
    if (paths.isEmpty()) return;
    ProxyManager* proxies = m_engine->proxies();
    QStringList canGenerate, withProxy, pending;
    for (const QString& path : paths) {
        const MediaInfo* m = m_project->mediaInfo(path);
        if (proxies->isPending(path)) pending << path;
        else if (proxies->hasProxy(path)) withProxy << path;
        else if (m && m->hasVideo && !m->isImage && QFileInfo::exists(path)) canGenerate << path;
    }
    QMenu menu(this);
    menu.addAction(T("Proxy-Medien erzeugen"), this, [proxies, canGenerate] { proxies->generate(canGenerate); })
        ->setEnabled(!canGenerate.isEmpty());
    menu.addAction(T("Proxy-Medien löschen"), this, [proxies, withProxy] { proxies->remove(withProxy); })
        ->setEnabled(!withProxy.isEmpty());
    if (!pending.isEmpty())
        menu.addAction(T("Proxy-Erzeugung abbrechen"), this, [proxies, pending] { proxies->cancel(pending); });
    menu.exec(m_list->viewport()->mapToGlobal(pos));
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
