// SPDX-License-Identifier: AGPL-3.0-only
// Phone basics of the touch layer (see Touch.h): click focus for the layer's own mouse events, the on-screen
// keyboard, the back key, safe areas. Nothing here is Android-only code; main.cpp decides what runs where.
#include "ui/touch/Touch.h"
#include <QLayout>
#include <QScreen>

#include <algorithm>
#include <QAbstractItemView>
#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QInputMethod>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QScrollArea>
#include <QTimer>
#include <QWidget>
#include <QWindow>

namespace Touch {

namespace {

int g_panelRequests = 0;

// A finger's tap on a text field asks for the keyboard
class InputPanelFilter : public QObject {
public:
    explicit InputPanelFilter(QObject* parent) : QObject(parent)
    {
        setObjectName("schneidiTouchInputPanel");
        // The keyboard came up (or moved): the window got smaller under it, the field must stay in view
        connect(QGuiApplication::inputMethod(), &QInputMethod::keyboardRectangleChanged, this,
                [] { QTimer::singleShot(0, [] { keepFocusVisible(); }); });
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        // The finger lifted on the focused text field (it just got the focus from this tap or had it already: the
        // keyboard may have been put away): the keyboard comes. The press gave the focus like a click before
        // Also when the tap itself put the focus into a text field elsewhere (a double tap on a track name opens its
        // name field, found in the Android emulator): the focus came within this tap, not by mouse or keyboard
        switch (e->type()) {
        case QEvent::FocusIn:
            if (obj->isWidgetType()) {
                m_focused = static_cast<QWidget*>(obj);
                m_focusedAt.start();
            }
            return false;
        case QEvent::KeyPress: m_focused = nullptr; return false;
        case QEvent::MouseButtonPress:
            if (!isTouch(static_cast<QMouseEvent*>(e))) m_focused = nullptr;
            return false;
        case QEvent::MouseButtonRelease: break;
        default: return false;
        }
        if (!isTouch(static_cast<QMouseEvent*>(e))) return false;
        auto* w = qobject_cast<QWidget*>(obj);
        QWidget* f = QApplication::focusWidget(); // (a number field's text part has the field as its focus proxy)
        const bool focusedByTap = f && m_focused == f && m_focusedAt.isValid() && m_focusedAt.elapsed() < kTapFocusMs;
        m_focused = nullptr;
        if (!w || !f || (!focusedByTap && w != f && !f->isAncestorOf(w)) || !f->testAttribute(Qt::WA_InputMethodEnabled)
            || !f->inputMethodQuery(Qt::ImEnabled).toBool() || f->inputMethodQuery(Qt::ImReadOnly).toBool())
            return false;
        ++g_panelRequests;
        QTimer::singleShot(0, f, [] { QGuiApplication::inputMethod()->show(); }); // after the widget's own release
        return false;
    }

private:
    static constexpr int kTapFocusMs = 700; // a tap or double tap, not a field focused long before
    QPointer<QWidget> m_focused; // the widget that last got the focus, until a key or a mouse button says otherwise
    QElapsedTimer m_focusedAt;
};

class BackKeyFilter : public QObject {
public:
    BackKeyFilter(QObject* parent, std::function<bool()> stepBack) : QObject(parent), m_stepBack(std::move(stepBack))
    {
        setObjectName("schneidiTouchBackKey");
    }

protected:
    bool eventFilter(QObject*, QEvent* e) override
    {
        if (e->type() != QEvent::KeyPress && e->type() != QEvent::KeyRelease) return false;
        auto* ke = static_cast<QKeyEvent*>(e);
        if (ke->key() != Qt::Key_Back) return false;
        ke->accept(); // taken: an unaccepted back key ends the app on Android
        if (e->type() == QEvent::KeyPress && !ke->isAutoRepeat()) back(m_stepBack);
        return true;
    }

private:
    std::function<bool()> m_stepBack;
};

} // namespace

void clickFocus(QWidget* w)
{
    for (; w; w = w->parentWidget()) {
        if (w->isEnabled() && (w->focusPolicy() & Qt::ClickFocus) == Qt::ClickFocus) {
            if (!w->hasFocus()) w->setFocus(Qt::MouseFocusReason);
            return;
        }
        if (w->isWindow()) return;
    }
}

void installInputPanel(QApplication* app)
{
    if (!app || app->findChild<QObject*>("schneidiTouchInputPanel", Qt::FindDirectChildrenOnly)) return;
    app->installEventFilter(new InputPanelFilter(app));
}

void keepFocusVisible()
{
    QWidget* focus = QApplication::focusWidget();
    if (!focus) return;
    for (QWidget* p = focus->parentWidget(); p; p = p->parentWidget()) {
        if (auto* view = qobject_cast<QAbstractItemView*>(p)) { // an editor open in a list or table
            if (view->currentIndex().isValid()) view->scrollTo(view->currentIndex());
        } else if (auto* area = qobject_cast<QScrollArea*>(p); area && area->widget()
                   && area->widget()->isAncestorOf(focus)) {
            area->ensureWidgetVisible(focus, 16, 24);
        }
        if (p->isWindow()) break;
    }
}

int inputPanelRequests() { return g_panelRequests; }

void installBackKey(QApplication* app, std::function<bool()> stepBack)
{
    if (!app || app->findChild<QObject*>("schneidiTouchBackKey", Qt::FindDirectChildrenOnly)) return;
    app->installEventFilter(new BackKeyFilter(app, std::move(stepBack)));
}

bool back(const std::function<bool()>& stepBack)
{
    if (QWidget* popup = QApplication::activePopupWidget()) { // a menu, a combo box's list, a popup panel
        popup->close();
        return true;
    }
    QWidget* front = QApplication::activeModalWidget();
    if (!front) front = qobject_cast<QDialog*>(QApplication::activeWindow()); // a dialog that is not modal
    if (front) {
        if (qobject_cast<QDialog*>(front)) { // like Esc: QDialog rejects, a message box takes its Esc answer
            QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(front, &esc);
        } else {
            front->close();
        }
        return true;
    }
    return stepBack && stepBack();
}

QMargins safeAreaMargins(const QWidget* w)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    if (w && w->window() && w->window()->windowHandle()) return w->window()->windowHandle()->safeAreaMargins();
#else
    Q_UNUSED(w);
#endif
    return {};
}

namespace {
class DialogFit : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* obj, QEvent* event) override
    {
        if (event->type() == QEvent::Show)
            if (auto* dialog = qobject_cast<QDialog*>(obj)) {
                QPointer<QDialog> d(dialog);
                QTimer::singleShot(0, d, [d] { // after the show's own layout
                    if (!d || !d->layout()) return;
                    const int wanted = std::max(d->layout()->totalMinimumSize().width(), d->sizeHint().width());
                    QScreen* screen = d->screen();
                    const int room = screen ? screen->availableGeometry().width() : wanted;
                    const int w = std::min(wanted, room);
                    if (d->width() >= w) return;
                    // Grown about its middle (it was centered at its first width), inside the screen
                    const QRect avail = screen ? screen->availableGeometry() : QRect();
                    int x = d->x() - (w - d->width()) / 2;
                    if (avail.isValid()) x = std::clamp(x, avail.left(), avail.right() + 1 - w);
                    d->setGeometry(x, d->y(), w, d->height());
                });
            }
        return QObject::eventFilter(obj, event);
    }
};
} // namespace

void installDialogFit(QApplication* app) { app->installEventFilter(new DialogFit(app)); }

} // namespace Touch
