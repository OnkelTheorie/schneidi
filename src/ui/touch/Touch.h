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

#include <QList>
#include <QMargins>
#include <QPointF>
#include <Qt>
#include <algorithm>
#include <functional>

class QApplication;
class QAbstractScrollArea;
class QEvent;
class QInputEvent;
class QMimeData;
class QPixmap;
class QPointingDevice;
class QWidget;

namespace Touch {

// ---------- Touch mode (TouchMode.cpp) ----------
// Settings › General › Touch mode: larger controls for fingers (buttons, fields, scroll bars, menu entries, list rows,
// timeline tracks, fade handles) and finger-sized grab zones for every pointer. Auto (the default): on where the
// system has a touchscreen and no mouse or touchpad (a phone, a tablet without keyboard); a touch laptop stays off
// (its touchpad counts) unless the user turns it on. Decided once at the start (takes effect after a restart, like the
// interface size): main.cpp calls initMode() before Theme::apply
enum class Mode { Auto, On, Off };
inline constexpr const char* kModeKey = "ui/touchMode"; // "on", "off"; missing = auto
Mode savedMode();
void setSavedMode(Mode mode);
// Auto's answer for these input devices (QInputDevice::DeviceType values)
bool autoOn(const QList<int>& deviceTypes);
// The saved mode against the devices there are now
bool decideMode();
void initMode();                    // modeActive() = decideMode()
bool modeActive();                  // touch mode is on in this run
void setModeActive(bool on);        // tests, initMode

// Half width of a grab zone (px each side of an edge) for a finger (or any pointer in touch mode); mouse zones stay
// as they are
constexpr int kGrabPx = 14;
inline int grabPx(bool touch, int mousePx) { return touch || modeActive() ? std::max(mousePx, kGrabPx) : mousePx; }
// Of a small handle (a fade grip, a keyframe icon): how far its grab zone reaches past its picture
inline int handleSlackPx(bool touch, int mousePx) { return touch || modeActive() ? std::max(mousePx, 8) : mousePx; }

// The event comes from a touchscreen (also a mouse event synthesized from one, by Qt or by TouchGestures)
bool isTouch(const QInputEvent* e);
bool isTouchDevice(const QPointingDevice* device);
// A long press at `pos` (widget coordinates): the context menu as a right click would open it; where nobody opens one,
// the tooltip the mouse would show there (a finger has no hover), else a short hint that there is no menu
void longPressAt(QWidget* w, const QPointF& pos, Qt::KeyboardModifiers mods = {});
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

// ---------- Phone basics (TouchPhone.cpp) ----------
// The focus a mouse click would give: `w` or the first parent with a click focus policy (Qt gives it only for
// spontaneous presses; the touch layer's own mouse events are not). TouchGestures and the scroll areas' taps use it
void clickFocus(QWidget* w);
// On-screen keyboard: a finger's tap on a text field (focused by the tap or already) asks for the input panel at once
// (Qt's widgets ask only on a second tap into a field that has the focus already); when the keyboard comes up or the
// window shrinks for it, the focused field scrolls into view inside its scroll area. Scrolling with a finger never
// moves the focus. A mouse or a keyboard (Tab) never opens the panel
void installInputPanel(QApplication* app);
// The focused widget into view inside the scroll areas around it (what the keyboard's arrival does)
void keepFocusVisible();
// How often the layer asked for the keyboard (tests: the offscreen platform has no input panel to look at)
int inputPanelRequests();

// Back key (Android's back button or gesture, Qt::Key_Back): one step back per press, never quitting by itself -
// the open popup or menu closes, else the dialog in front (like Esc: a message box takes its Esc answer), else
// `stepBack` (the main window: leaves the full-screen viewer, the large viewer, another page; with nothing left it
// asks whether to quit). The key is always taken (Android ends the app on a back key nobody accepted).
// main.cpp installs it on Android only: on a desktop a keyboard's "browser back" key would do it too
void installBackKey(QApplication* app, std::function<bool()> stepBack);
// A dialog shown narrower than its contents (on Android a dialog's first size came from fonts that were not yet the
// final ones: a check box's text was cut at the edge) grows to its contents' width once shown, at most the screen's.
// main.cpp installs it on Android only
void installDialogFit(QApplication* app);
// One step of the back key (also for tests); true = something closed or stepped back
bool back(const std::function<bool()>& stepBack);

// Safe area of the widget's window (notch, rounded corners, system bars drawn over the app): Qt 6.9 and newer report
// it and lay widgets out inside it themselves (WA_ContentsMarginsRespectsSafeArea, on for windows); older Qt: none
QMargins safeAreaMargins(const QWidget* w);

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
