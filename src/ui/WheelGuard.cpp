// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/WheelGuard.h"

#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QChildEvent>
#include <QComboBox>
#include <QEvent>
#include <QScrollBar>
#include <QWidget>

namespace {

// Value widgets; scroll bars are sliders too, but the wheel is theirs (the scroll area hands it to them)
bool takesWheel(const QObject* o)
{
    return (qobject_cast<const QAbstractSlider*>(o) && !qobject_cast<const QScrollBar*>(o))
           || qobject_cast<const QAbstractSpinBox*>(o) || qobject_cast<const QComboBox*>(o);
}

class Guard : public QObject {
public:
    explicit Guard(QWidget* root) : QObject(root) { watch(root); }

    // The whole subtree: containers too, so that widgets added to them later are seen (QEvent::ChildAdded)
    void watch(QWidget* w)
    {
        w->installEventFilter(this); // installing twice keeps one entry
        if (takesWheel(w) && w->focusPolicy() == Qt::WheelFocus) w->setFocusPolicy(Qt::StrongFocus);
        for (QObject* child : w->children())
            if (child->isWidgetType()) watch(static_cast<QWidget*>(child));
    }

    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() == QEvent::ChildAdded) {
            QObject* child = static_cast<QChildEvent*>(e)->child();
            if (child->isWidgetType()) watch(static_cast<QWidget*>(child));
        } else if (e->type() == QEvent::Wheel && takesWheel(obj) && !static_cast<QWidget*>(obj)->hasFocus()) {
            // not for this widget: ignored, so QApplication hands it to the parents (the scroll area scrolls)
            e->ignore();
            return true;
        }
        return QObject::eventFilter(obj, e);
    }
};

} // namespace

void WheelGuard::install(QWidget* root)
{
    if (root) new Guard(root);
}
