// SPDX-License-Identifier: AGPL-3.0-only
// The shared touch layer (see Touch.h): device checks, kinetic scrolling of scroll areas, finger drag and drop.
#include "ui/touch/Touch.h"

#include "core/I18n.h"

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QPixmap>
#include <QPointer>
#include <QPointingDevice>
#include <QScroller>
#include <QScrollBar>
#include <QScrollerProperties>
#include <QStyleHints>
#include <QTimer>
#include <QToolTip>
#include <QTouchEvent>
#include <cmath>

namespace Touch {

bool isTouchDevice(const QPointingDevice* device)
{
    return device && device->type() == QInputDevice::DeviceType::TouchScreen;
}

bool isTouch(const QInputEvent* e)
{
    return e && e->device() && e->device()->type() == QInputDevice::DeviceType::TouchScreen;
}

void longPressAt(QWidget* w, const QPointF& pos, Qt::KeyboardModifiers mods)
{
    if (!w) return;
    const QPoint global = w->mapToGlobal(pos).toPoint();
    const QPointer<QWidget> alive(w);
    // The context menu (like a right click; reason Other: a widget that opens some menus on the right button's
    // release still opens them). Nobody took it (no menu here): what the mouse's tooltip says - a finger has no hover
    QContextMenuEvent ce(QContextMenuEvent::Other, pos.toPoint(), global, mods);
    QApplication::sendEvent(w, &ce);
    if (ce.isAccepted() || !alive || QApplication::activePopupWidget()) return;
    QHelpEvent he(QEvent::ToolTip, pos.toPoint(), global);
    QApplication::sendEvent(w, &he);
    if (QToolTip::isVisible()) return;
    QToolTip::showText(global, T("Hier gibt es kein Menü."), w);
}

int slopPx()
{
    // A finger is less precise than a mouse (startDragDistance is 10 px by default); Android's own touch slop is 8 dp
#ifdef Q_OS_ANDROID
    return 8;
#else
    return std::max(10, QGuiApplication::styleHints()->startDragDistance());
#endif
}

int longPressMs()
{
    return std::clamp(QGuiApplication::styleHints()->mousePressAndHoldInterval(), 300, 1000);
}

namespace {

constexpr char kFilterName[] = "schneidiKineticScroll";

QElapsedTimer& clock()
{
    static QElapsedTimer t = [] {
        QElapsedTimer c;
        c.start();
        return c;
    }();
    return t;
}

// A finger drag on this child works the child (a slider, a number or text field), not the scroll area
bool dragsItself(QWidget* w, QWidget* viewport)
{
    for (; w && w != viewport; w = w->parentWidget()) {
        if (w->property("touchDrags").toBool()) return true;
        if (qobject_cast<QAbstractSlider*>(w) || qobject_cast<QAbstractSpinBox*>(w) || qobject_cast<QLineEdit*>(w))
            return true;
    }
    return false;
}

void sendMouse(QWidget* to, QEvent::Type type, const QPointF& global, bool down, const QPointingDevice* device,
               Qt::KeyboardModifiers mods)
{
    if (!to) return;
    if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick) clickFocus(to); // like a click
    const QPointF local = to->mapFromGlobal(global);
    const QPointF scene = to->window() ? to->window()->mapFromGlobal(global) : local;
    const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent me(type, local, scene, global, button, down ? Qt::LeftButton : Qt::NoButton, mods,
                   device ? device : QPointingDevice::primaryPointingDevice());
    QApplication::sendEvent(to, &me);
}

void contextMenu(QWidget* to, const QPointF& global, Qt::KeyboardModifiers mods)
{
    if (to) longPressAt(to, to->mapFromGlobal(global), mods);
}

// One finger on a scroll area's viewport: scroll with a flick (QScroller fed by hand, so the press/tap/long press
// logic stays ours), tap = click, long press = context menu, or on a dragging list the item follows the finger
class KineticFilter : public QObject {
public:
    KineticFilter(QAbstractScrollArea* area) : QObject(area->viewport()), m_area(area), m_vp(area->viewport())
    {
        setObjectName(kFilterName);
        m_vp->setAttribute(Qt::WA_AcceptTouchEvents);
        m_vp->installEventFilter(this);
        m_hold = new QTimer(this);
        m_hold->setSingleShot(true);
        m_hold->setInterval(longPressMs());
        connect(m_hold, &QTimer::timeout, this, &KineticFilter::longPress);
        QScroller* s = QScroller::scroller(m_vp);
        QScrollerProperties p = s->scrollerProperties();
        // Desktop look: no rubber band past the ends; our own slop decides when the drag begins
        p.setScrollMetric(QScrollerProperties::HorizontalOvershootPolicy, QScrollerProperties::OvershootAlwaysOff);
        p.setScrollMetric(QScrollerProperties::VerticalOvershootPolicy, QScrollerProperties::OvershootAlwaysOff);
        p.setScrollMetric(QScrollerProperties::DragStartDistance, 0.001);
        // A slanted swipe follows its main direction (a list does not drift sideways while flicked)
        p.setScrollMetric(QScrollerProperties::AxisLockThreshold, 0.6);
        s->setScrollerProperties(p);
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (obj != m_vp) return false;
        if (e->type() != QEvent::TouchBegin && e->type() != QEvent::TouchUpdate && e->type() != QEvent::TouchEnd
            && e->type() != QEvent::TouchCancel)
            return false;
        auto* te = static_cast<QTouchEvent*>(e);
        if (!isTouchDevice(te->pointingDevice()) || te->points().isEmpty()) return false;
        return touch(te);
    }

private:
    enum class State { Idle, Pending, Scrolling, Held, Dragging, Done };

    QScroller* scroller() const { return QScroller::scroller(m_vp); }
    // The finger in the viewport, along the axes the area scrolls: a bar that only scrolls sideways (the phone's tool
    // bar) never sees the finger's up and down (on the A51 a slanted swipe made it jump)
    QPointF local(const QPointF& global) const
    {
        QPointF p = m_vp->mapFromGlobal(global);
        const QPointF start = m_vp->mapFromGlobal(m_press);
        if (m_area && m_area->verticalScrollBar()->maximum() <= 0) p.setY(start.y());
        if (m_area && m_area->horizontalScrollBar()->maximum() <= 0) p.setX(start.x());
        return p;
    }
    QWidget* targetAt(const QPointF& global) const
    {
        QWidget* c = m_vp->childAt(local(global).toPoint());
        return c ? c : m_vp.data();
    }

    bool touch(QTouchEvent* e)
    {
        const QEventPoint& pt = e->points().first();
        const QPointF global = pt.globalPosition();
        const QPointer<KineticFilter> alive(this);
        if (e->type() == QEvent::TouchBegin) {
            QWidget* child = m_vp->childAt(pt.position().toPoint());
            if (dragsItself(child, m_vp)) { // the child gets the finger as a mouse (Qt makes one of the touch)
                e->ignore();
                return true;
            }
            e->accept();
            // A list that scrolls by whole rows jumps under a finger: by pixels from the first touch on (mouse users
            // never get here, so their wheel steps stay as they are)
            if (auto* view = qobject_cast<QAbstractItemView*>(m_area.data())) {
                view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
                view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
            }
            m_device = e->pointingDevice();
            m_mods = e->modifiers();
            m_press = m_last = global;
            m_state = State::Pending;
            // A finger on a gliding list only stops it (no tap on what happens to pass under it)
            m_stoppedFling = scroller()->state() == QScroller::Scrolling;
            m_hold->start();
            scroller()->handleInput(QScroller::InputPress, local(global), clock().elapsed());
            return true;
        }
        e->accept();
        if (e->type() == QEvent::TouchCancel) {
            m_hold->stop();
            if (m_state == State::Dragging) TouchDrag::finish(m_last, false);
            scroller()->stop();
            m_state = State::Idle;
            return true;
        }
        const bool end = e->type() == QEvent::TouchEnd;
        m_last = global;
        const bool moved = QLineF(m_press, global).length() >= slopPx();
        switch (m_state) {
        case State::Pending:
        case State::Scrolling:
            if (m_state == State::Pending && moved) {
                m_hold->stop();
                m_lastTap.invalidate();
                m_state = State::Scrolling;
            }
            scroller()->handleInput(end ? QScroller::InputRelease : QScroller::InputMove, local(global),
                                    clock().elapsed());
            if (end && m_state == State::Pending && !m_stoppedFling) tap();
            if (end && alive) m_state = State::Idle;
            return true;
        case State::Held:
            if (end) {
                m_state = State::Idle;
                contextMenu(targetAt(m_press), m_press, m_mods);
            } else if (moved) {
                startDrag();
            }
            return true;
        case State::Dragging:
            if (end) {
                m_state = State::Idle;
                TouchDrag::finish(global, true);
            } else {
                TouchDrag::move(global);
            }
            return true;
        case State::Done:
        case State::Idle:
            if (end) m_state = State::Idle;
            return true;
        }
        return true;
    }

    void tap()
    {
        m_hold->stop();
        QWidget* to = targetAt(m_press);
        const QPointer<KineticFilter> alive(this);
        const bool second = m_lastTap.isValid()
                            && m_lastTap.elapsed() < QGuiApplication::styleHints()->mouseDoubleClickInterval()
                            && QLineF(m_lastTapPos, m_press).length() < 3 * slopPx();
        if (second) {
            m_lastTap.invalidate();
            sendMouse(to, QEvent::MouseButtonDblClick, m_press, true, m_device, m_mods);
        } else {
            m_lastTap.start();
            m_lastTapPos = m_press;
            sendMouse(to, QEvent::MouseButtonPress, m_press, true, m_device, m_mods);
        }
        if (alive) sendMouse(to, QEvent::MouseButtonRelease, m_press, false, m_device, m_mods);
    }

    void longPress()
    {
        if (m_state != State::Pending) return;
        scroller()->stop();
        m_lastTap.invalidate();
        auto* view = qobject_cast<QAbstractItemView*>(m_area.data());
        if (view && view->dragEnabled() && view->indexAt(local(m_press).toPoint()).isValid()) {
            // Select the item under the finger; moving now drags it, lifting opens its menu
            const QPointer<KineticFilter> alive(this);
            sendMouse(m_vp, QEvent::MouseButtonPress, m_press, true, m_device, m_mods);
            if (alive) sendMouse(m_vp, QEvent::MouseButtonRelease, m_press, false, m_device, m_mods);
            if (alive) m_state = State::Held;
            return;
        }
        m_state = State::Done;
        contextMenu(targetAt(m_press), m_press, m_mods);
    }

    void startDrag()
    {
        auto* view = qobject_cast<QAbstractItemView*>(m_area.data());
        m_state = State::Done;
        if (!view || !view->model() || !view->selectionModel()) return;
        QModelIndexList indexes = view->selectionModel()->selectedIndexes();
        if (indexes.isEmpty()) return;
        QMimeData* data = view->model()->mimeData(indexes);
        if (!data) return;
        QPixmap pixmap;
        const QIcon icon = indexes.first().data(Qt::DecorationRole).value<QIcon>();
        if (!icon.isNull()) pixmap = icon.pixmap(view->iconSize().isValid() ? view->iconSize() : QSize(64, 36));
        m_state = State::Dragging;
        TouchDrag::start(view, data, pixmap, m_last);
    }

    QPointer<QAbstractScrollArea> m_area;
    QPointer<QWidget> m_vp;
    QTimer* m_hold = nullptr;
    State m_state = State::Idle;
    QPointer<const QPointingDevice> m_device;
    Qt::KeyboardModifiers m_mods;
    QPointF m_press, m_last, m_lastTapPos;
    QElapsedTimer m_lastTap;
    bool m_stoppedFling = false; // this touch began on a gliding list
};

class PolishFilter : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() == QEvent::Polish && obj->isWidgetType())
            if (auto* area = qobject_cast<QAbstractScrollArea*>(obj); area && !area->property("touchNoKinetic").toBool())
                enableKineticScroll(area);
        return false;
    }
};

} // namespace

void enableKineticScroll(QAbstractScrollArea* area)
{
    if (!area || !area->viewport() || hasKineticScroll(area)) return;
    new KineticFilter(area);
}

bool hasKineticScroll(const QAbstractScrollArea* area)
{
    return area && area->viewport() && area->viewport()->findChild<QObject*>(kFilterName, Qt::FindDirectChildrenOnly);
}

void installKineticScrolling(QApplication* app)
{
    constexpr char kName[] = "schneidiTouchPolish";
    if (!app || app->findChild<QObject*>(kName, Qt::FindDirectChildrenOnly)) return;
    auto* filter = new PolishFilter(app);
    filter->setObjectName(kName);
    app->installEventFilter(filter);
}

} // namespace Touch

// ---------- Finger drag and drop ----------

namespace TouchDrag {
namespace {

struct Drag {
    QPointer<QWidget> source;
    std::unique_ptr<QMimeData> data;
    QPointer<QWidget> target; // accepted the enter event
    QPointer<QLabel> image;   // the dragged item's picture next to the finger
    Qt::DropAction action = Qt::CopyAction;
};
std::unique_ptr<Drag> g_drag;

constexpr Qt::DropActions kActions = Qt::CopyAction | Qt::MoveAction | Qt::LinkAction;

void leave()
{
    if (!g_drag->target) return;
    QDragLeaveEvent le;
    QApplication::sendEvent(g_drag->target, &le);
    g_drag->target = nullptr;
}

QWidget* dropWidgetAt(const QPointF& global)
{
    QWidget* w = QApplication::widgetAt(global.toPoint());
    while (w && !w->acceptDrops()) w = w->isWindow() ? nullptr : w->parentWidget();
    return w;
}

} // namespace

void start(QWidget* source, QMimeData* data, const QPixmap& pixmap, const QPointF& globalPos)
{
    if (g_drag) finish(globalPos, false);
    g_drag = std::make_unique<Drag>();
    g_drag->source = source;
    g_drag->data.reset(data);
    if (!pixmap.isNull()) {
        auto* label = new QLabel(nullptr, Qt::ToolTip | Qt::FramelessWindowHint);
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        label->setAttribute(Qt::WA_ShowWithoutActivating);
        label->setPixmap(pixmap);
        label->resize(pixmap.deviceIndependentSize().toSize());
        g_drag->image = label;
    }
    move(globalPos);
}

bool active() { return bool(g_drag); }

void move(const QPointF& globalPos)
{
    if (!g_drag) return;
    if (QLabel* l = g_drag->image) { // above the finger, never under it (it would be the widget found there)
        l->move(globalPos.toPoint() - QPoint(l->width() / 2, l->height() + 24));
        if (!l->isVisible()) l->show();
    }
    QWidget* w = dropWidgetAt(globalPos);
    if (w != g_drag->target) {
        leave();
        // The first widget up the chain that takes the drag (like Qt: one that ignores the enter gets nothing more)
        for (; w; w = w->isWindow() ? nullptr : w->parentWidget()) {
            if (!w->acceptDrops()) continue;
            QDragEnterEvent de(w->mapFromGlobal(globalPos.toPoint()), kActions, g_drag->data.get(), Qt::LeftButton,
                               QGuiApplication::keyboardModifiers());
            QApplication::sendEvent(w, &de);
            if (de.isAccepted()) {
                g_drag->target = w;
                break;
            }
        }
    }
    if (!g_drag || !g_drag->target) return;
    QDragMoveEvent me(g_drag->target->mapFromGlobal(globalPos.toPoint()), kActions, g_drag->data.get(), Qt::LeftButton,
                      QGuiApplication::keyboardModifiers());
    QApplication::sendEvent(g_drag->target, &me);
    if (g_drag && !me.isAccepted()) leave();
    else if (g_drag) g_drag->action = me.dropAction();
}

bool finish(const QPointF& globalPos, bool drop)
{
    if (!g_drag) return false;
    std::unique_ptr<Drag> d = std::move(g_drag); // a drop handler may start the next one
    if (d->image) d->image->deleteLater();
    if (!d->target) return false;
    if (!drop) {
        QDragLeaveEvent le;
        QApplication::sendEvent(d->target, &le);
        return false;
    }
    QDropEvent de(d->target->mapFromGlobal(globalPos), kActions, d->data.get(), Qt::LeftButton,
                  QGuiApplication::keyboardModifiers());
    de.setDropAction(d->action);
    QApplication::sendEvent(d->target, &de);
    return de.isAccepted();
}

} // namespace TouchDrag
