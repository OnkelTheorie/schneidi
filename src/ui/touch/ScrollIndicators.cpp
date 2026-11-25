// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/touch/ScrollIndicators.h"

#include "app/Theme.h"
#include "ui/touch/Touch.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTimer>
#include <QVariantAnimation>

#include <algorithm>

ScrollIndicator::ScrollIndicator(QScrollBar* bar, QWidget* host, std::function<QRect()> area)
    : QWidget(host), m_bar(bar), m_area(std::move(area))
{
    setObjectName(bar->orientation() == Qt::Vertical ? "scrollIndicatorV" : "scrollIndicatorH");
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    m_fade = new QVariantAnimation(this);
    m_fade->setDuration(220);
    connect(m_fade, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        m_opacity = v.toDouble();
        setAttribute(Qt::WA_TransparentForMouseEvents, m_opacity <= 0.0 || !grabbable());
        update();
    });
    m_idle = new QTimer(this);
    m_idle->setSingleShot(true);
    m_idle->setInterval(kHideMs);
    connect(m_idle, &QTimer::timeout, this, [this] {
        if (m_dragFrom >= 0) m_idle->start(); // (held: stays)
        else setShown(false);
    });
    connect(bar, &QScrollBar::valueChanged, this, &ScrollIndicator::poke);
    connect(bar, &QScrollBar::rangeChanged, this, [this] { update(); });
    host->installEventFilter(this);
    m_lastValue = bar->value();
    m_lastMax = bar->maximum();
    m_lastPage = bar->pageStep();
    place();
    QWidget::show();
    raise();
}

bool ScrollIndicator::grabbable()
{
    // A finger scrolls the content itself (kinetic): an indicator that took presses right after a pan ate the next
    // tap at the edge or jumped a page. Only a mouse (touch mode off: a narrow desktop window) drags it
    return !Touch::modeActive();
}

void ScrollIndicator::poke()
{
    if (!m_bar) return;
    const int v = m_bar->value(), max = m_bar->maximum(), page = m_bar->pageStep();
    const bool moved = v != m_lastValue || max != m_lastMax || page != m_lastPage;
    m_lastValue = v;
    m_lastMax = max;
    m_lastPage = page;
    if (!moved || max <= m_bar->minimum()) return; // (nothing to scroll: never shown)
    place(); // (where the content is now: a scroll area lays out its viewport after its own resize event)
    setShown(true);
    update();
}

void ScrollIndicator::setShown(bool on)
{
    if (on) {
        m_idle->start();
        setAttribute(Qt::WA_TransparentForMouseEvents, !grabbable());
        if (m_opacity >= 1.0 && m_fade->state() != QAbstractAnimation::Running) return;
        m_fade->stop();
        m_opacity = 1.0;
        setAttribute(Qt::WA_TransparentForMouseEvents, !grabbable());
        raise();
        update();
        return;
    }
    m_fade->stop();
    m_fade->setStartValue(m_opacity);
    m_fade->setEndValue(0.0);
    m_fade->start();
}

bool ScrollIndicator::eventFilter(QObject* watched, QEvent* event)
{
    // A filter sees the resize before the scroll area lays out its viewport: placed again once that is done
    if (watched == parent() && (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest)) {
        place();
        QMetaObject::invokeMethod(this, &ScrollIndicator::place, Qt::QueuedConnection);
    }
    return QWidget::eventFilter(watched, event);
}

void ScrollIndicator::place()
{
    auto* host = parentWidget();
    const QRect area = m_area ? m_area() : host->rect();
    if (m_bar->orientation() == Qt::Vertical)
        setGeometry(area.right() + 1 - kHit, area.top(), kHit, area.height());
    else
        setGeometry(area.left(), area.bottom() + 1 - kHit, area.width(), kHit);
    raise();
}

QRect ScrollIndicator::handleRect() const
{
    if (!m_bar) return {};
    const bool vertical = m_bar->orientation() == Qt::Vertical;
    const int length = vertical ? height() : width();
    const int range = std::max(0, m_bar->maximum() - m_bar->minimum());
    const int page = std::max(1, m_bar->pageStep());
    const int handle = std::clamp(int(double(length) * page / (range + page)), std::min(length, 28), length);
    const int pos = range > 0 ? int(double(length - handle) * (m_bar->value() - m_bar->minimum()) / range) : 0;
    const int thick = m_dragFrom >= 0 ? kThin + 2 : kThin;
    return vertical ? QRect(width() - thick - 2, pos, thick, handle) : QRect(pos, height() - thick - 2, handle, thick);
}

void ScrollIndicator::paintEvent(QPaintEvent*)
{
    if (m_opacity <= 0.0 || !m_bar || m_bar->maximum() <= m_bar->minimum()) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setOpacity(m_opacity);
    p.setPen(Qt::NoPen);
    QColor c = Theme::text;
    c.setAlpha(150);
    p.setBrush(c);
    const QRectF r = handleRect();
    const double radius = std::min(r.width(), r.height()) / 2.0;
    p.drawRoundedRect(r, radius, radius);
}

void ScrollIndicator::mousePressEvent(QMouseEvent* e)
{
    if (!m_bar || m_opacity <= 0.0) return e->ignore();
    m_dragFrom = m_bar->orientation() == Qt::Vertical ? e->position().toPoint().y() : e->position().toPoint().x();
    m_dragValue = m_bar->value();
    // A press beside the handle jumps it there first (as a scroll bar's page click, but straight to the spot)
    const QRect h = handleRect();
    const bool onHandle = m_bar->orientation() == Qt::Vertical ? (m_dragFrom >= h.top() && m_dragFrom <= h.bottom())
                                                              : (m_dragFrom >= h.left() && m_dragFrom <= h.right());
    if (!onHandle) {
        const int length = m_bar->orientation() == Qt::Vertical ? height() : width();
        const int handle = m_bar->orientation() == Qt::Vertical ? h.height() : h.width();
        const int range = m_bar->maximum() - m_bar->minimum();
        if (length > handle)
            m_bar->setValue(m_bar->minimum() + int(double(m_dragFrom - handle / 2) * range / (length - handle)));
        m_dragValue = m_bar->value();
    }
    setShown(true);
    update();
    e->accept();
}

void ScrollIndicator::mouseMoveEvent(QMouseEvent* e)
{
    if (!m_bar || m_dragFrom < 0) return e->ignore();
    const bool vertical = m_bar->orientation() == Qt::Vertical;
    const int at = vertical ? e->position().toPoint().y() : e->position().toPoint().x();
    const int length = vertical ? height() : width();
    const QRect h = handleRect();
    const int free = length - (vertical ? h.height() : h.width());
    const int range = m_bar->maximum() - m_bar->minimum();
    if (free > 0) m_bar->setValue(m_dragValue + int(double(at - m_dragFrom) * range / free));
    setShown(true);
    e->accept();
}

void ScrollIndicator::mouseReleaseEvent(QMouseEvent* e)
{
    m_dragFrom = -1;
    setShown(true); // (fades after the usual time)
    update();
    e->accept();
}

// --- ScrollIndicators ---

ScrollIndicators::ScrollIndicators(QWidget* window) : QObject(window), m_window(window)
{
    for (QAbstractScrollArea* area : window->findChildren<QAbstractScrollArea*>()) adopt(area);
    qApp->installEventFilter(this);
}

ScrollIndicators::~ScrollIndicators()
{
    for (const Adopted& a : std::as_const(m_adopted)) {
        for (const QPointer<ScrollIndicator>& i : a.indicators) delete i.data();
        if (!a.area) continue;
        a.area->setHorizontalScrollBarPolicy(a.h);
        a.area->setVerticalScrollBarPolicy(a.v);
    }
}

void ScrollIndicators::adopt(QAbstractScrollArea* area)
{
    if (!area || area->property("ownScrollBars").toBool()) return;
    for (const Adopted& a : std::as_const(m_adopted))
        if (a.area == area) return;
    // A destroyed area leaves the list (QPointer is already null in destroyed(); the indicators were its children)
    connect(area, &QObject::destroyed, this, [this] { m_adopted.removeIf([](const Adopted& a) { return !a.area; }); });
    Adopted a{area, area->horizontalScrollBarPolicy(), area->verticalScrollBarPolicy(), {}};
    if (a.h == Qt::ScrollBarAlwaysOff && a.v == Qt::ScrollBarAlwaysOff) {
        m_adopted << a; // (nothing to do; remembered so it is not looked at again)
        return;
    }
    const auto viewport = [area] { return area->viewport()->geometry(); };
    if (a.v != Qt::ScrollBarAlwaysOff) {
        area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        a.indicators << new ScrollIndicator(area->verticalScrollBar(), area, viewport);
    }
    if (a.h != Qt::ScrollBarAlwaysOff) {
        area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        a.indicators << new ScrollIndicator(area->horizontalScrollBar(), area, viewport);
    }
    m_adopted << a;
}

bool ScrollIndicators::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Show) {
        if (auto* area = qobject_cast<QAbstractScrollArea*>(watched); area && area->window() == m_window) adopt(area);
    }
    return QObject::eventFilter(watched, event);
}
