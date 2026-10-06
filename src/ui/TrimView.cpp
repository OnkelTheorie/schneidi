#include "ui/TrimView.h"

#include "app/Theme.h"
#include "core/I18n.h"
#include "core/Timecode.h"

#include <QFontDatabase>
#include <QPainter>
#include <algorithm>

namespace {
constexpr int kGap = 10;
constexpr int kMargin = 8;

QString kindName(TimelineOps::TrimKind k)
{
    using TimelineOps::TrimKind;
    switch (k) {
    case TrimKind::Ripple: return T("Ripple");
    case TrimKind::Roll: return T("Roll");
    case TrimKind::Slip: return T("Slip");
    case TrimKind::Slide: return T("Slide");
    }
    return {};
}
} // namespace

TrimView::TrimView(QWidget* parent) : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void TrimView::setAspect(const QSize& frameSize)
{
    if (frameSize.isEmpty() || frameSize == m_aspect) return;
    m_aspect = frameSize;
    update();
}

void TrimView::assign(QVector<Slot>& list, const QVector<TrimFrames::Pane>& panes)
{
    QVector<Slot> next(panes.size());
    for (int i = 0; i < panes.size(); ++i) {
        next[i].pane = panes[i];
        // same clip on this side: keep the last image as a stand-in (dragging changes the frame every few ms)
        if (i < list.size() && list[i].pane.clipId == panes[i].clipId && panes[i].clipId != 0
            && list[i].pane.path == panes[i].path) {
            next[i].image = list[i].image;
            next[i].exact = list[i].exact && list[i].pane.fileFrame == panes[i].fileFrame;
        }
    }
    list = next;
}

void TrimView::setView(const TrimFrames::View& view)
{
    // Slip/slide swap which panes are large; images follow their pane, not the slot
    if (view.kind != m_view.kind) {
        m_main.clear();
        m_small.clear();
    }
    m_view = view;
    assign(m_main, view.main);
    assign(m_small, view.small);
    update();
}

bool TrimView::setImage(const QString& path, int frame, const QImage& image)
{
    bool used = false;
    for (QVector<Slot>* list : {&m_main, &m_small})
        for (Slot& s : *list)
            if (!s.pane.path.isEmpty() && s.pane.path == path && s.pane.fileFrame == frame) {
                s.image = image;
                s.exact = true;
                used = true;
            }
    if (used) update();
    return used;
}

bool TrimView::isExact(bool small, int index) const
{
    const QVector<Slot>& list = small ? m_small : m_main;
    return index >= 0 && index < list.size() && list[index].exact;
}

QVector<QRect> TrimView::mainRects() const
{
    QVector<QRect> main, small;
    layoutRects(&main, &small);
    return main;
}

QVector<QRect> TrimView::smallRects() const
{
    QVector<QRect> main, small;
    layoutRects(&main, &small);
    return small;
}

void TrimView::layoutRects(QVector<QRect>* main, QVector<QRect>* small) const
{
    const int label = fontMetrics().height() + 6; // name/timecode line under each pane
    const bool four = !m_view.small.isEmpty();
    // space: header line (kind) on top, delta line at the bottom
    const int availW = width() - 2 * kMargin - kGap;
    const int availH = height() - 2 * kMargin - 2 * label;
    const double aspect = double(m_aspect.height()) / m_aspect.width();
    double pw = availW / 2.0;
    double ph = pw * aspect;
    // four-up: small panes are half as large, above the large ones
    const double needH = four ? ph * 1.5 + 2 * label + kGap : ph + label;
    const double fixedH = four ? 2 * label + kGap : label;
    if (needH > availH) {
        ph = std::max(8.0, (availH - fixedH) / (four ? 1.5 : 1.0));
        pw = ph / aspect;
    }
    const double totalW = 2 * pw + kGap;
    const double totalH = four ? ph * 1.5 + 2 * label + kGap : ph + label;
    const double x0 = (width() - totalW) / 2;
    double y = kMargin + label + std::max(0.0, (availH - totalH) / 2);
    if (four) {
        const double sw = pw / 2, sh = ph / 2;
        for (int i = 0; i < 2; ++i)
            *small << QRectF(x0 + i * (pw + kGap) + (pw - sw) / 2, y, sw, sh).toRect();
        y += sh + label + kGap;
    }
    for (int i = 0; i < 2; ++i) *main << QRectF(x0 + i * (pw + kGap), y, pw, ph).toRect();
}

void TrimView::drawSlot(QPainter& p, const QRect& r, const Slot& s, bool large) const
{
    const TrimFrames::Pane& pane = s.pane;
    p.fillRect(r, pane.clipId ? Theme::thumbBg : Qt::black);
    if (!s.image.isNull()) {
        QSize sz = s.image.size().scaled(r.size(), Qt::KeepAspectRatio);
        const QRect ir(r.x() + (r.width() - sz.width()) / 2, r.y() + (r.height() - sz.height()) / 2, sz.width(), sz.height());
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(ir, s.image);
    } else {
        p.setPen(pane.clipId ? Theme::textDim : Theme::textFaint);
        p.drawText(r, Qt::AlignCenter, pane.clipId ? pane.name : T("Kein Clip"));
    }
    p.setPen(Theme::border);
    p.setBrush(Qt::NoBrush);
    p.drawRect(r.adjusted(0, 0, -1, -1));

    // In/Out badge in the corner (like the DaVinci trim view)
    QFont f = font();
    f.setBold(true);
    if (!large) f.setPointSizeF(f.pointSizeF() * 0.85);
    p.setFont(f);
    const QString badge = pane.out ? QStringLiteral("OUT") : QStringLiteral("IN");
    const QRect b = p.fontMetrics().boundingRect(badge).adjusted(-5, -2, 5, 2);
    const QRect br(r.left() + 6, r.top() + 6, b.width(), b.height());
    p.fillRect(br, Theme::alpha(Qt::black, 160));
    p.setPen(Theme::secondary);
    p.drawText(br, Qt::AlignCenter, badge);
    p.setFont(font());

    if (!pane.clipId) return;
    // name left, source timecode right, under the pane
    const int label = fontMetrics().height() + 6;
    const QRect line(r.left(), r.bottom() + 1, r.width(), label);
    const QString tc = Timecode::format(pane.sourceFrame, m_fps);
    const int tcW = fontMetrics().horizontalAdvance(tc);
    p.setPen(Theme::text);
    p.drawText(line.adjusted(0, 0, -tcW - 8, 0), Qt::AlignLeft | Qt::AlignVCenter,
               fontMetrics().elidedText(pane.name, Qt::ElideRight, std::max(0, line.width() - tcW - 8)));
    p.setPen(Theme::textDim);
    p.drawText(line, Qt::AlignRight | Qt::AlignVCenter, tc);
}

void TrimView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), Theme::viewerBg);
    if (m_view.isNull()) return;
    QVector<QRect> main, small;
    layoutRects(&main, &small);
    for (int i = 0; i < m_small.size() && i < small.size(); ++i) drawSlot(p, small[i], m_small[i], false);
    for (int i = 0; i < m_main.size() && i < main.size(); ++i) drawSlot(p, main[i], m_main[i], true);

    const int label = fontMetrics().height() + 6;
    const int top = (small.isEmpty() ? main : small).value(0).top();
    p.setPen(Theme::textDim);
    p.drawText(QRect(0, top - label, width(), label), Qt::AlignHCenter | Qt::AlignVCenter, kindName(m_view.kind));
    // trim amount below, signed like the readout at the edit in the timeline
    const int bottom = main.value(0).bottom() + label;
    const QString delta = (m_view.delta >= 0 ? "+" : "") + Timecode::format(m_view.delta, m_fps);
    p.setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    p.setPen(m_view.delta ? Theme::primary : Theme::textDim);
    p.drawText(QRect(0, bottom, width(), label), Qt::AlignHCenter | Qt::AlignVCenter, delta);
}
