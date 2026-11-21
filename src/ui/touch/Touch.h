// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Touch input (touchscreen laptops, tablets; later the Android build of the same widgets). One shared layer instead of
// touch code in every widget:
//  - Touch::isTouch(): an input event comes from a finger (a QTouchEvent of a touchscreen or a mouse event made from
//    one). Hit tests use it for larger grab zones (Touch::grabPx), mouse and pen keep their exact sizes
//  - TouchGestures: for widgets that draw themselves (timeline, viewer). One finger becomes the widget's own mouse
//    events (carrying the touch device, so Touch::isTouch() sees them) once it moves past the slop or lifts (tap);
//    a second finger before that is a pinch (zoom + pan, one callback); a double tap and a long press have their own
//    callbacks (long press default: the widget's context menu)
//  - Touch::installKineticScrolling(): every scroll area (lists, panels, dialogs, settings) scrolls with a finger and
//    flicks (QScroller driven by hand); a tap clicks, a long press opens the context menu, and on a list that drags
//    (Media Pool, effects) a long press then moving drags the item (TouchDrag)
//  - TouchDrag: drag and drop with a finger, without QDrag (its platform loop wants a mouse and does not run on
//    Android): sends the drag enter/move/drop events to the widget under the finger itself
// Only touchscreens are handled: touchpads (macOS sends their touches to widgets that take touch events) and pens go
// the usual way.

#include <QPointF>
#include <algorithm>

class QApplication;
class QAbstractScrollArea;
class QEvent;
class QInputEvent;
class QMimeData;
class QPixmap;
class QPointingDevice;
class QWidget;

namespace Touch {

// Half width of a grab zone (px each side of an edge) for a finger; mouse zones stay as they are
constexpr int kGrabPx = 14;
inline int grabPx(bool touch, int mousePx) { return touch ? std::max(mousePx, kGrabPx) : mousePx; }

// The event comes from a touchscreen (also a mouse event synthesized from one, by Qt or by TouchGestures)
bool isTouch(const QInputEvent* e);
bool isTouchDevice(const QPointingDevice* device);
// A finger moving less than this (px) is still a tap / a long press
int slopPx();
// Held this long without moving = long press (ms)
int longPressMs();

// Kinetic finger scrolling for one scroll area (idempotent) and whether it has it
void enableKineticScroll(QAbstractScrollArea* area);
bool hasKineticScroll(const QAbstractScrollArea* area);
// Every scroll area the application creates gets it (an application event filter on Polish). A widget sets the
// property "touchNoKinetic" = true to keep it off; a child widget inside a scroll area sets "touchDrags" = true when
// a finger drag on it changes it (sliders, number fields and text fields are known already) - the finger then works
// the child instead of scrolling
void installKineticScrolling(QApplication* app);

} // namespace Touch

// Drag and drop with a finger (see above). One at a time
namespace TouchDrag {
// Starts a drag of `data` (owned from here) from `source` at the finger's global position
void start(QWidget* source, QMimeData* data, const QPixmap& pixmap, const QPointF& globalPos);
bool active();
void move(const QPointF& globalPos);
// Lifted: drop on the widget that accepted (drop = false: cancelled, a leave event only). Returns true if dropped
bool finish(const QPointF& globalPos, bool drop = true);
} // namespace TouchDrag
