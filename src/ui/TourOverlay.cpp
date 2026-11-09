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
#include <utility>

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
    m_title->setWordWrap(true);
    m_counter = new QLabel;
    m_counter->setObjectName("tour_counter");
    m_counter->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    m_counter->setAlignment(Qt::AlignRight | Qt::AlignTop);
    head->addWidget(m_title, 1);
    head->addWidget(m_counter);
    lay->addLayout(head);
    m_text = new QLabel;
    m_text->setObjectName("tour_text");
    m_text->setWordWrap(true);
    m_text->setTextFormat(Qt::PlainText);
    lay->addWidget(m_text);
    lay->addStretch(1); // a bubble kept at the tour's steady size: the room above the buttons
    m_row1 = new QHBoxLayout;
    m_row1->setSpacing(6);
    lay->addLayout(m_row1);
    m_row2 = new QHBoxLayout;
    m_row2->setSpacing(6);
    lay->addLayout(m_row2);
    m_hint = new QLabel;
    m_hint->setObjectName("tour_hint");
    m_hint->setWordWrap(true);
    m_hint->setStyleSheet(QString("color: %1; font-size: small;").arg(Theme::textDim.name()));
    lay->addWidget(m_hint);
    m_bubble->setFixedWidth(kMinWidth);

    window->installEventFilter(this); // size changes
    m_follow.setInterval(250);        // panels that move without the window changing size (splitters, toggles)
    connect(&m_follow, &QTimer::timeout, this, &TourOverlay::relayout);
}

void TourOverlay::setTargets(std::function<QVector<QRect>()> targets)
{
    m_targets = std::move(targets);
    if (isVisible()) relayout();
}

void TourOverlay::setKeepFree(std::function<QVector<QRect>()> keep, std::function<QVector<QRect>()> avoid)
{
    m_keep = std::move(keep);
    m_avoid = std::move(avoid);
}

namespace {
// Takes every item out of a row (the widgets stay where they are)
void emptyRow(QHBoxLayout* row)
{
    while (QLayoutItem* item = row->takeAt(0)) delete item;
}
// Every layout's cached hints, the nested ones too (a label's new text at a new width)
void invalidateAll(QLayout* layout)
{
    for (int i = 0; i < layout->count(); ++i)
        if (QLayout* sub = layout->itemAt(i)->layout()) invalidateAll(sub);
    layout->invalidate();
}
} // namespace

void TourOverlay::clearButtons()
{
    emptyRow(m_row1);
    emptyRow(m_row2);
    for (const Button& b : std::exchange(m_buttonList, {})) { // later: the button clicked may be the one asking for new buttons
        b.button->hide();
        b.button->setParent(nullptr);
        b.button->deleteLater();
    }
    m_escape = {};
    m_arranged = 0;
}

QPushButton* TourOverlay::addButton(const QString& name, const QString& text, bool primary, bool left)
{
    auto* b = new QPushButton(text, m_bubble);
    b->setObjectName(name);
    if (primary) {
        b->setDefault(true);
        b->setStyleSheet(QString("QPushButton { background: %1; color: %2; border: none; padding: 4px 12px; "
                                 "border-radius: 4px; }")
                             .arg(Theme::primary.name(), Theme::onPrimary.name()));
    }
    // Never squeezed below its text: the bubble grows (or takes a second row) instead
    b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_buttonList.push_back({b, left});
    b->show(); // the bubble may be showing already (the next step)
    return b;
}

void TourOverlay::arrangeButtons(bool twoRows)
{
    emptyRow(m_row1);
    emptyRow(m_row2);
    for (const Button& b : std::as_const(m_buttonList))
        if (b.left) m_row1->addWidget(b.button);
    m_row1->addStretch(1);
    QHBoxLayout* right = twoRows ? m_row2 : m_row1;
    if (twoRows) right->addStretch(1);
    for (const Button& b : std::as_const(m_buttonList))
        if (!b.left) right->addWidget(b.button);
}

QSize TourOverlay::bubbleSize(int atLeast)
{
    m_bubble->ensurePolished();
    for (QWidget* w : m_bubble->findChildren<QWidget*>()) w->ensurePolished(); // the style sheet's fonts and paddings
    auto* lay = static_cast<QVBoxLayout*>(m_bubble->layout());
    const QMargins m = lay->contentsMargins();
    // What the bubble adds around its layout: its frame (the style sheet's border) and the layout's margins
    const int frame = m_bubble->width() - m_bubble->contentsRect().width();
    const int extra = frame + m.left() + m.right();
    int leftW = 0, rightW = 0, leftN = 0, rightN = 0;
    for (const Button& b : std::as_const(m_buttonList)) {
        const int w = b.button->sizeHint().width();
        (b.left ? leftW : rightW) += w;
        ++(b.left ? leftN : rightN);
    }
    const int sp = m_row1->spacing();
    leftW += sp * std::max(0, leftN - 1);
    rightW += sp * std::max(0, rightN - 1);
    const int gapBetween = leftN && rightN ? 3 * sp : 0; // the stretch between the two groups, at least
    const int oneRow = leftW + gapBetween + rightW;
    // Never wider than the window allows (a small window); the buttons win over kMaxWidth though: they never clip
    const int windowMax = std::max(kMinWidth, width() - 2 * 14);
    const int maxW = std::min(kMaxWidth, windowMax);
    const int base = 360; // a comfortable line length for the text
    int w;
    bool twoRows = false;
    if (oneRow + extra <= maxW) {
        w = std::max(base, oneRow + extra);
    } else {
        twoRows = leftN > 0 && rightN > 0;
        w = std::max(base, (twoRows ? std::max(leftW, rightW) : oneRow) + extra);
        w = std::min(w, windowMax);
    }
    w = std::clamp(std::max(w, atLeast), kMinWidth, std::max(kMinWidth, windowMax));
    if (m_arranged != (twoRows ? 2 : 1)) { // only when it changed (this runs a few times a second)
        arrangeButtons(twoRows);
        m_arranged = twoRows ? 2 : 1;
    }
    m_bubble->setFixedWidth(w);
    invalidateAll(lay);
    lay->activate();
    // The height at that width from the layout (word-wrapped labels report their height for a width)
    int h = m_bubble->hasHeightForWidth() ? m_bubble->heightForWidth(w) : -1;
    if (h <= 0) h = m_bubble->sizeHint().height();
    h = std::max(h, lay->totalMinimumSize().height());
    return {w, h};
}

void TourOverlay::showStep(const QString& text, int index, int count, bool waiting, const QString& offer)
{
    if (m_question || index == 0) m_steady = {}; // a new tour
    m_question = false;
    m_title->hide();
    m_counter->setText(QString("%1/%2").arg(index + 1).arg(count));
    m_counter->show();
    m_text->setText(text);
    m_hint->hide();
    clearButtons();
    auto* endB = addButton("tour_end", T("Rundgang beenden"), false, true);
    endB->setFlat(true);
    connect(endB, &QPushButton::clicked, this, &TourOverlay::endClicked);
    if (!offer.isEmpty()) {
        auto* o = addButton("tour_offer", offer, false, false);
        connect(o, &QPushButton::clicked, this, &TourOverlay::offerClicked);
    }
    if (index > 0) {
        auto* back = addButton("tour_back", T("Zurück"), false, false);
        connect(back, &QPushButton::clicked, this, &TourOverlay::backClicked);
    }
    const QString nextText = waiting ? T("Überspringen") : index + 1 == count ? T("Fertig") : T("Weiter");
    auto* next = addButton("tour_next", nextText, !waiting, false);
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

void TourOverlay::showQuestion(const QString& title, const QString& text, const QString& hint,
                               const QVector<Choice>& choices)
{
    m_question = true;
    m_steady = {};
    m_title->setText(title);
    m_title->setVisible(!title.isEmpty());
    m_counter->hide();
    m_text->setText(text);
    m_hint->setText(hint);
    m_hint->setVisible(!hint.isEmpty());
    clearButtons();
    QPushButton* focus = nullptr;
    for (const Choice& c : choices) {
        auto* b = addButton(c.name, c.text, c.primary, c.flat);
        b->setFlat(c.flat);
        connect(b, &QPushButton::clicked, this, [run = c.run] {
            if (run) run();
        });
        if (c.primary || !focus) focus = b;
        if (c.escape) m_escape = c.run;
    }
    setGeometry(parentWidget()->rect());
    this->show();
    raise();
    relayout();
    QTimer::singleShot(0, this, &TourOverlay::relayout);
    if (focus) focus->setFocus();
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

QRect TourOverlay::placeBubble(const QRect& area, const QVector<QRect>& holes, const QSize& size, int gap,
                               const QVector<QRect>& keep, const QVector<QRect>& avoid, const QRect& previous)
{
    const QRect inner = area.adjusted(gap, gap, -gap, -gap);
    QRect all;
    for (const QRect& h : holes) all = all.united(h);
    if (!all.isValid()) {
        QRect r(QPoint(0, 0), size);
        r.moveCenter(area.center());
        return r;
    }
    const auto clampX = [&](int x) { return std::clamp(x, inner.left(), std::max(inner.left(), inner.right() - size.width() + 1)); };
    const auto clampY = [&](int y) { return std::clamp(y, inner.top(), std::max(inner.top(), inner.bottom() - size.height() + 1)); };
    const auto clear = [&](const QRect& r, const QVector<QRect>& rects) {
        for (const QRect& h : rects)
            if (h.intersects(r)) return false;
        return true;
    };
    const auto free = [&](const QRect& r, bool strict) {
        return inner.contains(r) && clear(r, holes) && clear(r, keep) && (!strict || clear(r, avoid));
    };
    const QRect main = holes.value(0).isValid() ? holes.value(0) : all;
    // The place of the step before, if it still fits beside this step's main hole
    if (previous.isValid()) {
        const QRect r(previous.topLeft(), size);
        const QRect near = main.adjusted(-gap - 120, -gap - 120, gap + 120, gap + 120);
        if (free(r, true) && near.intersects(r)) return r;
    }
    // Beside the main hole first (far apart targets: the bubble belongs to the first, not between them), then beside
    // all of them together; free of the places to avoid if it can be, else only off the holes and the kept controls
    for (const bool strict : {true, false})
        for (const QRect& target : {main, all}) {
            if (!target.isValid()) continue;
            const int cx = clampX(target.center().x() - size.width() / 2);
            const int cy = clampY(target.center().y() - size.height() / 2);
            QVector<QRect> places;
            if (previous.isValid()) { // the previous line first: below/above at its x, right/left at its y
                const int px = clampX(previous.left()), py = clampY(previous.top());
                const QRect side[] = {QRect(QPoint(px, target.bottom() + gap), size),
                                      QRect(QPoint(px, target.top() - gap - size.height()), size),
                                      QRect(QPoint(target.right() + gap, py), size),
                                      QRect(QPoint(target.left() - gap - size.width(), py), size)};
                // Its side first: where it sat relative to this target
                const int first = previous.top() > target.bottom() ? 0
                                  : previous.bottom() < target.top() ? 1
                                  : previous.left() > target.right() ? 2
                                  : previous.right() < target.left() ? 3 : 0;
                places << side[first];
                for (int i = 0; i < 4; ++i)
                    if (i != first) places << side[i];
            }
            places << QRect(QPoint(cx, target.bottom() + gap), size)              // below
                   << QRect(QPoint(cx, target.top() - gap - size.height()), size) // above
                   << QRect(QPoint(target.right() + gap, cy), size)               // right
                   << QRect(QPoint(target.left() - gap - size.width(), cy), size); // left
            for (const QRect& r : std::as_const(places))
                if (free(r, strict)) return r;
        }
    // A target filling (nearly) the whole window: inside it, at its lower part, still in the window
    return QRect(QPoint(clampX(main.center().x() - size.width() / 2), clampY(main.bottom() - gap - size.height())), size);
}

void TourOverlay::relayout()
{
    if (!parentWidget()) return;
    if (geometry() != parentWidget()->rect()) setGeometry(parentWidget()->rect());
    QVector<QRect> holes;
    if (!m_question && m_targets)
        for (const QRect& t : m_targets())
            if (t.isValid()) holes << t.adjusted(-kPad, -kPad, kPad, kPad).intersected(rect());
    QVector<QRect> keep, avoid;
    if (!m_question && m_keep)
        for (const QRect& k : m_keep()) keep << k.adjusted(-kPad, -kPad, kPad, kPad);
    if (!m_question && m_avoid) avoid = m_avoid();
    // The steady size: never smaller than the tour's bubbles before (its height; its width when it fits the window)
    QSize size = bubbleSize();
    if (!m_question) {
        if (size.width() < m_steady.width() && m_steady.width() <= width() - 28) {
            size = bubbleSize(m_steady.width());
        }
        size.setHeight(std::max(size.height(), m_steady.height()));
        m_steady = m_steady.expandedTo(size);
    }
    const QRect previous = m_bubble->isVisible() && !m_question ? m_bubble->geometry() : QRect();
    const QRect bubble = placeBubble(rect(), holes, size, 14, keep, avoid, previous);
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
            if (!m_question) emit endClicked();
            else if (const auto escape = m_escape) escape(); // a copy: the answer may ask the next question
            return true;
        }
    }
    return QWidget::eventFilter(obj, e);
}
