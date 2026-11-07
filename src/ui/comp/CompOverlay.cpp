// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/comp/CompOverlay.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/TimelineOps.h"
#include "engine/picture/CompNodes.h"
#include "ui/comp/CompShared.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace PC = Picture::Comp;

namespace {

constexpr double kHandle = 4.5; // half size (widget pixels)
constexpr double kHit = 9;

} // namespace

CompOverlay::CompOverlay(Editor* editor, QObject* parent) : ViewerOverlay(parent), m_editor(editor) {}

void CompOverlay::setTarget(int clipId, int node, int playhead)
{
    if (clipId == m_clipId && node == m_node && playhead == m_playhead) return;
    m_clipId = clipId;
    m_node = node;
    m_playhead = playhead;
    emit changed();
}

void CompOverlay::setVisible(bool on)
{
    if (on == m_visible) return;
    m_visible = on;
    emit changed();
}

const Clip* CompOverlay::clip() const
{
    if (!m_visible || !m_clipId || !m_node) return nullptr;
    return TimelineOps::findClip(m_editor->project()->timeline(), m_clipId);
}

QSizeF CompOverlay::pictureSize() const
{
    const ProjectFormat f = m_editor->project()->timelineFormat();
    return QSizeF(f.width, f.height);
}

bool CompOverlay::clipToWidget(const QRectF& frame, QTransform* out) const
{
    const Clip* c = clip();
    if (!c || frame.isEmpty()) return false;
    const QSizeF pic = pictureSize();
    const double W = pic.width(), H = pic.height();
    const double t = CompUi::clipFrame(*c, m_playhead);
    QTransform ct;
    ct.translate(frame.left(), frame.top());
    ct.scale(frame.width() / W, frame.height() / H);
    if (Keys::hasTransform(*c)) { // the clip's own transform after the graph, as the picture engine applies it
        ct.translate(W / 2 + Keys::valueAt(*c, AnimParam::PosX, t), H / 2 - Keys::valueAt(*c, AnimParam::PosY, t));
        ct.rotate(-Keys::valueAt(*c, AnimParam::Rotation, t));
        ct.scale(Keys::valueAt(*c, AnimParam::ZoomX, t), Keys::valueAt(*c, AnimParam::ZoomY, t));
        ct.translate(-W / 2, -H / 2);
    }
    *out = ct;
    return true;
}

QVector<CompOverlay::Handle> CompOverlay::handles() const
{
    QVector<Handle> out;
    const Clip* c = clip();
    CompGraph g;
    if (!c || !CompUi::graphOf(*c, &g)) return out;
    const CompNode* n = g.node(m_node);
    const PC::NodeType* k = n ? PC::find(n->type) : nullptr;
    if (!k) return out;
    const int t = CompUi::clipFrame(*c, m_playhead);
    const QSizeF pic = pictureSize();
    const auto num = [&](const QString& key) { return CompUi::number(*c, m_node, *k, key, t); };
    const auto at = [&](const QString& kx, const QString& ky) { return QPointF(num(kx) * pic.width(), num(ky) * pic.height()); };
    const QVariantMap values = CompUi::nodeValues(*c, m_node);
    if (k->id == QLatin1String("mask")) {
        const QString shape = PC::textValue(*k, values, QStringLiteral("shape"));
        if (shape == QLatin1String("rect") || shape == QLatin1String("ellipse")) {
            const QPointF center = at("x", "y");
            out << Handle{Handle::Point, "x", "y", center, {}};
            // Corner: the half sizes turned with the shape (sizes relative to the picture width)
            QTransform rot;
            rot.rotate(num("rotate"));
            const QPointF half(num("w") * pic.width() / 2, num("h") * pic.width() / 2);
            out << Handle{Handle::Size, "w", "h", center + rot.map(half), {}};
        } else {
            const bool gradient = shape == QLatin1String("linear") || shape == QLatin1String("radial");
            const int count = gradient ? 2 : std::clamp(int(num("points")), 0, 32);
            for (int i = 0; i < count; ++i) {
                const QString p = QStringLiteral("p%1").arg(i);
                out << Handle{Handle::Point, p + 'x', p + 'y', at(p + 'x', p + 'y'), QString::number(i + 1)};
            }
        }
    } else if (k->id == QLatin1String("transform")) {
        out << Handle{Handle::Point, "x", "y", at("x", "y"), {}};
    } else if (k->id == QLatin1String("tracker")) {
        out << Handle{Handle::Point, "ax", "ay", at("ax", "ay"), QStringLiteral("A")};
        if (PC::textValue(*k, values, QStringLiteral("mode")) == QLatin1String("two"))
            out << Handle{Handle::Point, "bx", "by", at("bx", "by"), QStringLiteral("B")};
    }
    return out;
}

QPointF CompOverlay::handlePos(int i, const QRectF& frame) const
{
    QTransform ct;
    const QVector<Handle> hs = handles();
    if (i < 0 || i >= hs.size() || !clipToWidget(frame, &ct)) return {};
    return ct.map(hs[i].at);
}

int CompOverlay::handleAt(const QPointF& pos, const QRectF& frame) const
{
    QTransform ct;
    if (!clipToWidget(frame, &ct)) return -1;
    const QVector<Handle> hs = handles();
    for (int i = int(hs.size()) - 1; i >= 0; --i) { // the size corner above the middle
        const QPointF h = ct.map(hs[i].at);
        if (std::hypot(h.x() - pos.x(), h.y() - pos.y()) <= kHit) return i;
    }
    return -1;
}

void CompOverlay::paint(QPainter& p, const QRectF& frame)
{
    QTransform ct;
    const QVector<Handle> hs = handles();
    if (hs.isEmpty() || !clipToWidget(frame, &ct)) return;
    const Clip* c = clip();
    CompGraph g;
    CompUi::graphOf(*c, &g);
    const PC::NodeType* k = PC::find(g.node(m_node)->type);
    const int t = CompUi::clipFrame(*c, m_playhead);
    const QSizeF pic = pictureSize();
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    // The shape's outline (dark under light: visible on any picture)
    QPainterPath outline;
    if (k->id == QLatin1String("mask")) {
        const QString shape = PC::textValue(*k, CompUi::nodeValues(*c, m_node), QStringLiteral("shape"));
        if (shape == QLatin1String("rect") || shape == QLatin1String("ellipse")) {
            const double hw = CompUi::number(*c, m_node, *k, "w", t) * pic.width() / 2;
            const double hh = CompUi::number(*c, m_node, *k, "h", t) * pic.width() / 2;
            QPainterPath local;
            const QRectF r(-hw, -hh, 2 * hw, 2 * hh);
            if (shape == QLatin1String("ellipse")) local.addEllipse(r);
            else local.addRect(r);
            QTransform lt;
            lt.translate(hs[0].at.x(), hs[0].at.y());
            lt.rotate(CompUi::number(*c, m_node, *k, "rotate", t));
            outline = (lt * ct).map(local);
        } else if (hs.size() >= 2) {
            QPolygonF poly;
            for (const Handle& h : hs) poly << ct.map(h.at);
            if (shape == QLatin1String("polygon") || shape == QLatin1String("bezier")) poly << poly.first();
            outline.addPolygon(poly);
        }
    }
    if (!outline.isEmpty()) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 140), 3));
        p.drawPath(outline);
        p.setPen(QPen(QColor(255, 255, 255, 230), 1.4));
        p.drawPath(outline);
    }
    for (int i = 0; i < hs.size(); ++i) {
        const QPointF h = ct.map(hs[i].at);
        p.setPen(QPen(QColor(0, 0, 0, 160), 1));
        if (hs[i].kind == Handle::Size) {
            p.setBrush(QColor(255, 255, 255));
            p.drawRect(QRectF(h.x() - kHandle, h.y() - kHandle, 2 * kHandle, 2 * kHandle));
        } else {
            p.setBrush(i == m_drag ? Theme::secondary : Theme::primary);
            p.drawEllipse(h, kHandle + 1, kHandle + 1);
            p.setPen(QPen(QColor(255, 255, 255, 200), 1));
            p.drawLine(h - QPointF(9, 0), h - QPointF(kHandle + 2, 0));
            p.drawLine(h + QPointF(kHandle + 2, 0), h + QPointF(9, 0));
            p.drawLine(h - QPointF(0, 9), h - QPointF(0, kHandle + 2));
            p.drawLine(h + QPointF(0, kHandle + 2), h + QPointF(0, 9));
        }
        if (!hs[i].label.isEmpty()) {
            p.setPen(QColor(255, 255, 255, 230));
            p.drawText(h + QPointF(8, -8), hs[i].label);
        }
    }
    p.restore();
}

Qt::CursorShape CompOverlay::cursorAt(const QPointF& pos, const QRectF& frame) const
{
    const int i = handleAt(pos, frame);
    if (i < 0) return Qt::ArrowCursor;
    return handles()[i].kind == Handle::Size ? Qt::SizeFDiagCursor : Qt::SizeAllCursor;
}

bool CompOverlay::mousePress(QMouseEvent* e, const QRectF& frame)
{
    if (e->button() != Qt::LeftButton) return false;
    m_drag = handleAt(e->position(), frame);
    if (m_drag >= 0) emit changed();
    return m_drag >= 0;
}

bool CompOverlay::mouseMove(QMouseEvent* e, const QRectF& frame)
{
    if (m_drag < 0) return false;
    QTransform ct;
    const QVector<Handle> hs = handles();
    if (m_drag >= hs.size() || !clipToWidget(frame, &ct)) return true;
    const QPointF pos = ct.inverted().map(e->position()); // clip picture pixels
    const QSizeF pic = pictureSize();
    const Handle& h = hs[m_drag];
    QVariantMap values;
    if (h.kind == Handle::Size) {
        // Around the shape's middle, in the shape's own (turned) axes
        const Clip* c = clip();
        CompGraph g;
        CompUi::graphOf(*c, &g);
        const PC::NodeType* k = PC::find(g.node(m_node)->type);
        QTransform lt;
        lt.translate(hs[0].at.x(), hs[0].at.y());
        lt.rotate(CompUi::number(*c, m_node, *k, "rotate", CompUi::clipFrame(*c, m_playhead)));
        const QPointF l = lt.inverted().map(pos);
        values.insert(h.kx, std::abs(l.x()) * 2 / pic.width());
        values.insert(h.ky, std::abs(l.y()) * 2 / pic.width());
    } else {
        values.insert(h.kx, pos.x() / pic.width());
        values.insert(h.ky, pos.y() / pic.height());
    }
    m_editor->setCompNodeValuesAt(m_clipId, m_node, values, m_playhead,
                                  QStringLiteral("comp:overlay:%1:%2").arg(m_clipId).arg(m_node));
    return true;
}

bool CompOverlay::mouseRelease(QMouseEvent* e, const QRectF&)
{
    if (m_drag < 0 || e->button() != Qt::LeftButton) return false;
    m_drag = -1;
    m_editor->project()->closeMerge();
    emit changed();
    return true;
}
