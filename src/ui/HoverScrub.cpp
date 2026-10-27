#include "ui/HoverScrub.h"

#include "engine/ScrubFrames.h"

#include <QEvent>
#include <QListWidget>
#include <QMouseEvent>

#include <algorithm>

HoverScrub::HoverScrub(QListWidget* list, const QSize& thumbSize, FileOf fileOf, Show show)
    : QObject(list), m_list(list), m_frames(new ScrubFrames(thumbSize, this)), m_fileOf(std::move(fileOf)),
      m_show(std::move(show))
{
    m_list->viewport()->setMouseTracking(true);
    m_list->viewport()->installEventFilter(this);
    connect(m_frames, &ScrubFrames::ready, this, [this](const QString& path, int) {
        if (path == m_path) refresh();
    });
}

void HoverScrub::setEnabled(bool on)
{
    if (!on) leave();
    m_enabled = on;
}

void HoverScrub::forget()
{
    m_item = nullptr;
    m_path.clear();
    m_slot = -1;
}

bool HoverScrub::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == m_list->viewport()) {
        if (e->type() == QEvent::MouseMove) {
            const auto* me = static_cast<QMouseEvent*>(e);
            if (me->buttons() == Qt::NoButton) hover(me->position().toPoint());
            else leave(); // dragging: the thumbnail as it is
        } else if (e->type() == QEvent::Leave || e->type() == QEvent::MouseButtonPress || e->type() == QEvent::Wheel) {
            leave();
        }
    }
    return QObject::eventFilter(obj, e);
}

void HoverScrub::hover(const QPoint& pos)
{
    QListWidgetItem* it = m_enabled && m_list->viewMode() == QListView::IconMode ? m_list->itemAt(pos) : nullptr;
    if (it != m_item) { // another entry: is there anything to scrub (asked once per entry)
        leave();
        const QString path = it ? m_fileOf(it) : QString();
        if (path.isEmpty()) return;
        m_item = it;
        m_path = path;
    }
    if (!m_item) return;
    // Across the thumbnail (the cell is a little wider than the picture)
    const QRect cell = m_list->visualItemRect(m_item);
    const int inset = std::max(0, (cell.width() - m_list->iconSize().width()) / 2);
    const int width = std::max(1, cell.width() - 2 * inset);
    m_fraction = std::clamp(double(pos.x() - cell.left() - inset) / width, 0.0, 1.0);
    m_slot = ScrubFrames::slotAt(m_fraction);
    refresh();
}

void HoverScrub::leave()
{
    QListWidgetItem* it = m_item;
    forget();
    if (it) m_show(it, {}, -1);
}

void HoverScrub::refresh()
{
    if (!m_item) return;
    if (m_frames->failed(m_path)) { // nothing to decode in it (no video stream, unreadable)
        leave();
        return;
    }
    m_show(m_item, m_frames->picture(m_path, m_slot), m_fraction);
}
