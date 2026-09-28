#include "ui/MediaPool.h"

#include "app/Theme.h"
#include "core/Project.h"
#include "core/Timecode.h"
#include "engine/Engine.h"

#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
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
    importBtn->setText("Importieren…");
    connect(importBtn, &QToolButton::clicked, this, &MediaPool::importDialog);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 4, 0);
    header->addWidget(title, 1);
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
    rebuild();
}

void MediaPool::importDialog()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this, "Medien importieren", QString(),
        "Medien (*.mp4 *.mov *.mkv *.avi *.webm *.mts *.m4v *.mp3 *.wav *.flac *.ogg *.m4a *.aac "
        "*.png *.jpg *.jpeg *.webp *.bmp);;Alle Dateien (*)");
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
        QMessageBox::warning(this, "Import", "Nicht lesbar:\n" + failed.join('\n'));
}

void MediaPool::rebuild()
{
    m_list->clear();
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
    auto* titleItem = new QListWidgetItem(QIcon(titleThumb), "Text\nTitel");
    titleItem->setData(Qt::UserRole, QString(TitleItem));
    titleItem->setToolTip("Titel – in die Timeline ziehen");
    m_list->addItem(titleItem);

    for (const MediaInfo& m : m_project->media()) {
        QPixmap thumb;
        const bool offline = !QFileInfo::exists(m.path); // wie DaVinci "Media Offline"
        if (offline) thumb = placeholderThumb("Media Offline");
        else if (m.hasVideo) {
            const QImage img = m_engine->thumbnail(m.path, m.isImage ? 0 : m.length / 3, kThumb);
            if (!img.isNull()) thumb = QPixmap::fromImage(img);
        }
        if (thumb.isNull()) thumb = placeholderThumb(m.hasAudio ? "♪ Audio" : "?");

        auto* item = new QListWidgetItem(QIcon(thumb),
                                         m.name + "\n" + Timecode::format(m.length, m_project->fps()));
        item->setData(Qt::UserRole, m.path);
        item->setToolTip(m.path);
        m_list->addItem(item);
    }
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
