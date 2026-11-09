// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/TourOverlay.h"

#include "app/Theme.h"
#include "core/I18n.h"

#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>

namespace {
constexpr int kPad = 6;      // the hole is this much larger than its target
constexpr int kFeather = 14; // soft edge: from clear to dim over this many pixels
constexpr int kRadius = 8;
constexpr int kDim = 150;    // alpha of the dim layer
} // namespace

TourOverlay::TourOverlay(QWidget* window) : QWidget(window)
{
    setObjectName("tour_overlay");
    hide();
    m_bubble = new QFrame(this);
    m_bubble->setObjectName("tour_bubble");
    m_bubble->setStyleSheet(QString("QFrame#tour_bubble { background: %1; border: 1px solid %2; border-radius: 8px; }"
                                    "QLabel { background: transparent; border: none; }")
                                .arg(Theme::panelHeader.name(), Theme::primary.name()));
    auto* lay = new QVBoxLayout(m_bubble);
    lay->setContentsMargins(14, 10, 14, 10);
    lay->setSpacing(8);
    auto* head = new QHBoxLayout;
    m_title = new QLabel;
    m_title->setObjectName("tour_title");
    m_title->setStyleSheet("font-weight: 600;");
    m_counter = new QLabel;
    m_counter->setObjectName("tour_counter");
    m_counter->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    head->addWidget(m_title, 1);
    head->addWidget(m_counter);
    lay->addLayout(head);
    m_text = new QLabel;
    m_text->setObjectName("tour_text");
    m_text->setWordWrap(true);
    m_text->setTextFormat(Qt::PlainText);
    lay->addWidget(m_text);
    m_buttons = new QHBoxLayout;
    m_buttons->setSpacing(6);
    lay->addLayout(m_buttons);
    m_hint = new QLabel;
    m_hint->setObjectName("tour_hint");
    m_hint->setWordWrap(true);
    m_hint->setStyleSheet(QString("color: %1; font-size: small;").arg(Theme::textDim.name()));
    lay->addWidget(m_hint);
    m_bubble->setFixedWidth(360);

    window->installEventFilter(this); // size changes
    m_follow.setInterval(250);        // panels that move without the window changing size (splitters, toggles)
    connect(&m_follow, &QTimer::timeout, this, &TourOverlay::relayout);
}

void TourOverlay::setTargets(std::function<QVector<QRect>()> targets)
{
    m_targets = std::move(targets);
    if (isVisible()) relayout();
}

void TourOverlay::clearButtons()
{
    while (QLayoutItem* item = m_buttons->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

QPushButton* TourOverlay::addButton(const QString& text, bool primary)
{
    auto* b = new QPushButton(text);
    if (primary) {
        b->setDefault(true);
        b->setStyleSheet(QString("QPushButton { background: %1; color: %2; border: none; padding: 4px 12px; "
                                 "border-radius: 4px; }")
                             .arg(Theme::primary.name(), Theme::onPrimary.name()));
    }
    m_buttons->addWidget(b);
    return b;
}

void TourOverlay::showStep(const QString& text, int index, int count, bool waiting, bool sample)
{
    m_question = false;
    m_title->hide();
    m_counter->setText(QString("%1/%2").arg(index + 1).arg(count));
    m_counter->show();
    m_text->setText(text);
    m_hint->hide();
    clearButtons();
    auto* endB = addButton(T("Rundgang beenden"), false);
    endB->setObjectName("tour_end");
    endB->setFlat(true);
    connect(endB, &QPushButton::clicked, this, &TourOverlay::endClicked);
    m_buttons->addStretch(1);
    if (sample) {
        auto* s = addButton(T("Beispielclips laden"), false);
        s->setObjectName("tour_sample");
        connect(s, &QPushButton::clicked, this, &TourOverlay::sampleClicked);
    }
    if (index > 0) {
        auto* back = addButton(T("Zurück"), false);
        back->setObjectName("tour_back");
        connect(back, &QPushButton::clicked, this, &TourOverlay::backClicked);
    }
    const QString nextText = waiting ? T("Überspringen") : index + 1 == count ? T("Fertig") : T("Weiter");
    auto* next = addButton(nextText, !waiting);
    next->setObjectName("tour_next");
    connect(next, &QPushButton::clicked, this, &TourOverlay::nextClicked);
    setGeometry(parentWidget()->rect());
    show();
    raise();
    relayout();
    QTimer::singleShot(0, this, &TourOverlay::relayout); // again once the labels have their final font and width
    next->setFocus();
    m_follow.start();
    qApp->installEventFilter(this); // Esc
}

void TourOverlay::showQuestion(const QString& title, const QString& text, bool firstStart)
{
    m_question = true;
    m_title->setText(title);
    m_title->show();
    m_counter->hide();
    m_text->setText(text);
    m_hint->setText(T("Du findest alle Rundgänge jederzeit unter Hilfe › Rundgänge."));
    m_hint->show();
    clearButtons();
    if (firstStart) {
        auto* never = addButton(T("Keine Rundgänge anbieten"), false);
        never->setObjectName("tour_never");
        never->setFlat(true);
        connect(never, &QPushButton::clicked, this, &TourOverlay::never);
    }
    m_buttons->addStretch(1);
    auto* later = addButton(T("Später"), false);
    later->setObjectName("tour_later");
    connect(later, &QPushButton::clicked, this, &TourOverlay::later);
    auto* show = addButton(T("Zeig's mir"), true);
    show->setObjectName("tour_show");
    connect(show, &QPushButton::clicked, this, &TourOverlay::showMe);
    setGeometry(parentWidget()->rect());
    this->show();
    raise();
    relayout();
    QTimer::singleShot(0, this, &TourOverlay::relayout);
    show->setFocus();
    m_follow.stop();
    qApp->installEventFilter(this);
}

void TourOverlay::end()
{
    m_follow.stop();
    qApp->removeEventFilter(this);
    hide();
}

QRect TourOverlay::bubbleGeometry() const { return m_bubble->geometry(); }
QWidget* TourOverlay::bubble() const { return m_bubble; }

QRect TourOverlay::placeBubble(const QRect& area, const QRect& target, const QSize& size, int gap)
{
    const QRect inner = area.adjusted(gap, gap, -gap, -gap);
    if (!target.isValid()) {
        QRect r(QPoint(0, 0), size);
        r.moveCenter(area.center());
        return r;
    }
    const auto clampX = [&](int x) { return std::clamp(x, inner.left(), std::max(inner.left(), inner.right() - size.width() + 1)); };
    const auto clampY = [&](int y) { return std::clamp(y, inner.top(), std::max(inner.top(), inner.bottom() - size.height() + 1)); };
    const int cx = clampX(target.center().x() - size.width() / 2);
    const int cy = clampY(target.center().y() - size.height() / 2);
    const QRect places[] = {
        QRect(QPoint(cx, target.bottom() + gap), size),                     // below
        QRect(QPoint(cx, target.top() - gap - size.height()), size),        // above
        QRect(QPoint(target.right() + gap, cy), size),                      // right
        QRect(QPoint(target.left() - gap - size.width(), cy), size),        // left
    };
    for (const QRect& r : places)
        if (inner.contains(r)) return r;
    // A target filling (nearly) the whole window: inside it, at its lower part, still in the window
    QRect r(QPoint(clampX(cx), clampY(target.bottom() - gap - size.height())), size);
    return r;
}

void TourOverlay::relayout()
{
    if (!parentWidget()) return;
    if (geometry() != parentWidget()->rect()) setGeometry(parentWidget()->rect());
    QVector<QRect> holes;
    if (!m_question && m_targets)
        for (const QRect& t : m_targets())
            if (t.isValid()) holes << t.adjusted(-kPad, -kPad, kPad, kPad).intersected(rect());
    // The height at the bubble's fixed width, summed by hand: the layout's own hint does not follow wrapped labels
    const int w = m_bubble->width();
    auto* lay = static_cast<QVBoxLayout*>(m_bubble->layout());
    const QMargins m = lay->contentsMargins();
    const int inner = w - m.left() - m.right() - 2 * m_bubble->frameWidth() - 2; // the stylesheet border too
    int h = m.top() + m.bottom() + 2 * m_bubble->frameWidth();
    int rows = 0;
    for (int i = 0; i < lay->count(); ++i) {
        QLayoutItem* item = lay->itemAt(i);
        int rowH = 0;
        if (auto* l = qobject_cast<QLabel*>(item->widget())) {
            if (l->isHidden()) continue;
            l->ensurePolished(); // the style sheet's font, before the first paint
            rowH = l->heightForWidth(inner);
            l->setFixedHeight(rowH);
        } else {
            rowH = item->sizeHint().height();
            if (i == 0) // the title/counter row: the layout's hint misses a label shown while the bubble was hidden
                for (QLabel* l : {m_title, m_counter})
                    if (!l->isHidden()) rowH = std::max(rowH, l->sizeHint().height());
        }
        h += rowH;
        ++rows;
    }
    h += lay->spacing() * std::max(0, rows - 1);
    const QSize size(w, h);
    QRect box;
    for (const QRect& h : holes) box = box.united(h);
    const QRect bubble = placeBubble(rect(), box, size);
    if (holes != m_holes || bubble != m_bubble->geometry()) {
        m_holes = holes;
        m_bubble->setGeometry(bubble);
        // Clicks inside a hole go to the widget below
        QRegion mask(rect());
        for (const QRect& h : holes) mask -= QRegion(h, QRegion::Rectangle);
        setMask(mask);
        update();
    }
}

void TourOverlay::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath dim;
    dim.addRect(rect());
    for (const QRect& h : m_holes) {
        QPainterPath hole;
        hole.addRoundedRect(QRectF(h).adjusted(-kFeather, -kFeather, kFeather, kFeather), kRadius + kFeather,
                            kRadius + kFeather);
        dim = dim.subtracted(hole);
    }
    p.fillPath(dim, QColor(0, 0, 0, kDim));
    // Soft edge: rings from clear (at the hole) to the dim layer's darkness (kFeather outside)
    for (const QRect& h : m_holes) {
        for (int i = 0; i < kFeather; ++i) {
            const double grow = i + 0.5;
            p.setPen(QPen(QColor(0, 0, 0, kDim * (i + 1) / (kFeather + 1)), 1.0));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(h).adjusted(-grow, -grow, grow, grow), kRadius + grow, kRadius + grow);
        }
        p.setPen(QPen(Theme::alpha(Theme::primary, 200), 2.0));
        p.drawRoundedRect(QRectF(h).adjusted(-1, -1, 1, 1), kRadius, kRadius);
    }
}

bool TourOverlay::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == parentWidget() && e->type() == QEvent::Resize && isVisible()) relayout();
    if (e->type() == QEvent::KeyPress && isVisible() && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape) {
        // Only while our window is the active one (a dialog of its own keeps its Esc)
        const QWidget* active = QApplication::activeWindow();
        if (!active || active == window()) {
            if (m_question) emit later();
            else emit endClicked();
            return true;
        }
    }
    return QWidget::eventFilter(obj, e);
}
