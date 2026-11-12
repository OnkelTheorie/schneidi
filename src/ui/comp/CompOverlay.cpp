// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/comp/CompOverlay.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/CompTrack.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/TimelineOps.h"
#include "engine/RegionPick.h"
#include "engine/Tracker.h"
#include "engine/picture/CompNodes.h"
#include "ui/TiltTransform.h"
#include "ui/comp/CompShared.h"

#include <QMouseEvent>
#include <QSettings>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace PC = Picture::Comp;

namespace {

constexpr double kHandle = 4.5; // half size (widget pixels)
constexpr double kHit = 9;

} // namespace

CompOverlay::CompOverlay(Editor* editor, QObject* parent)
    : ViewerOverlay(parent), m_editor(editor),
      m_tolerance(std::clamp(QSettings().value("comp/pickTolerance", RegionPick::kDefaultTolerance).toDouble(), 0.0, 1.0))
{
}

void CompOverlay::setTarget(int clipId, int node, int playhead)
{
    if (clipId == m_clipId && node == m_node && playhead == m_playhead) return;
    if (clipId != m_clipId || node != m_node) setPicking(false); // picking belongs to the mask it started on
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
    QTransform ct, view;
    view.translate(frame.left(), frame.top());
    view.scale(frame.width() / W, frame.height() / H);
    if (Keys::hasTransform(*c)) { // the clip's own transform after the graph, as the picture engine applies it
        QTransform flat;
        flat.translate(W / 2 + Keys::valueAt(*c, AnimParam::PosX, t), H / 2 - Keys::valueAt(*c, AnimParam::PosY, t));
        flat.rotate(-Keys::valueAt(*c, AnimParam::Rotation, t));
        flat.scale(Keys::valueAt(*c, AnimParam::ZoomX, t), Keys::valueAt(*c, AnimParam::ZoomY, t));
        flat.translate(-W / 2, -H / 2);
        ct = TiltTransform::apply(flat, *c, t, W, H, W, H);
    }
    *out = ct * view;
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
        // 0..1 of the clip's own picture (without bars), as trackNode reads them
        const QRectF r = Tracker::pictureRect(m_editor->project()->timelineFormat(), *c);
        const auto own = [&](const QString& kx, const QString& ky) {
            return r.topLeft() + QPointF(num(kx) * r.width(), num(ky) * r.height());
        };
        const QString mode = PC::textValue(*k, values, QStringLiteral("mode"));
        if (mode == QLatin1String(::Comp::kQuadMode)) { // the four start corners, clockwise from the top left
            for (int i = 0; i < 4; ++i) {
                const QString kx = QString::fromLatin1(::Comp::kQuadStartKeys[2 * i]);
                const QString ky = QString::fromLatin1(::Comp::kQuadStartKeys[2 * i + 1]);
                out << Handle{Handle::Point, kx, ky, own(kx, ky), QString::number(i + 1), true};
            }
            return out;
        }
        out << Handle{Handle::Point, "ax", "ay", own("ax", "ay"), QStringLiteral("A"), true};
        if (mode == QLatin1String("two"))
            out << Handle{Handle::Point, "bx", "by", own("bx", "by"), QStringLiteral("B"), true};
    } else if (k->id == QLatin1String(PC::kCornerPinType)) {
        // Its own corners by hand; following a tracker they come from there (the corners shown as an outline only)
        if (::Comp::followId(num(QStringLiteral("follow"))) > 0) return out;
        for (int i = 0; i < 4; ++i) {
            const QString kx = QString::fromLatin1(::Comp::kCornerKeys[2 * i]);
            const QString ky = QString::fromLatin1(::Comp::kCornerKeys[2 * i + 1]);
            out << Handle{Handle::Point, kx, ky, at(kx, ky), QString::number(i + 1)};
        }
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
    // Four corners (tracker mode quad, corner pin): their quad; a tracker's corners found at the playhead dashed
    QPainterPath found;
    if ((k->id == QLatin1String(::Comp::kTrackerType) || k->id == QLatin1String(PC::kCornerPinType)) && hs.size() == 4) {
        QPolygonF poly;
        for (const Handle& h : hs) poly << ct.map(h.at);
        poly << poly.first();
        outline.addPolygon(poly);
        if (k->id == QLatin1String(::Comp::kTrackerType)) {
            const AnimParam c0 = Keys::effectParam(::Comp::kEffectId, CompGraph::paramKey(m_node, ::Comp::kCornerKeys[0]));
            if (Keys::animated(*c, c0)) {
                const QRectF r = Tracker::pictureRect(m_editor->project()->timelineFormat(), *c);
                QPolygonF now;
                for (int i = 0; i < 4; ++i) {
                    const double x = CompUi::number(*c, m_node, *k, QString::fromLatin1(::Comp::kCornerKeys[2 * i]), t);
                    const double y = CompUi::number(*c, m_node, *k, QString::fromLatin1(::Comp::kCornerKeys[2 * i + 1]), t);
                    now << ct.map(r.topLeft() + QPointF(x * r.width(), y * r.height()));
                }
                now << now.first();
                found.addPolygon(now);
            }
        }
    }
    if (!found.isEmpty()) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 140), 3));
        p.drawPath(found);
        QPen dash(Theme::primary, 1.6, Qt::DashLine);
        p.setPen(dash);
        p.drawPath(found);
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
    if (m_picking && clip()) return Qt::CrossCursor;
    const int i = handleAt(pos, frame);
    if (i < 0) return Qt::ArrowCursor;
    return handles()[i].kind == Handle::Size ? Qt::SizeFDiagCursor : Qt::SizeAllCursor;
}

bool CompOverlay::mousePress(QMouseEvent* e, const QRectF& frame)
{
    if (e->button() != Qt::LeftButton) return false;
    if (m_picking) {
        QTransform ct;
        const Clip* c = clip();
        if (!c || !clipToWidget(frame, &ct)) return false;
        // 0..1 of the clip's own picture (without bars), as RegionPick and the tracker read it
        const QPointF pos = ct.inverted().map(e->position());
        const QRectF r = Tracker::pictureRect(m_editor->project()->timelineFormat(), *c);
        const QPointF p((pos.x() - r.left()) / std::max(1.0, r.width()), (pos.y() - r.top()) / std::max(1.0, r.height()));
        if (p.x() < 0 || p.y() < 0 || p.x() > 1 || p.y() > 1) return true; // on the bars: nothing to pick
        pickAt(p, e->modifiers());
        return true;
    }
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
    } else if (h.ownPicture) {
        const QRectF r = Tracker::pictureRect(m_editor->project()->timelineFormat(), *clip());
        values.insert(h.kx, (pos.x() - r.left()) / std::max(1.0, r.width()));
        values.insert(h.ky, (pos.y() - r.top()) / std::max(1.0, r.height()));
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

void CompOverlay::setPicking(bool on)
{
    if (on == m_picking) return;
    m_picking = on;
    emit pickingChanged(on);
    emit changed();
}

namespace {

// What a pick says to the user (RegionPick's texts are English, for the CLI)
QString pickText(const RegionPick::Result& r)
{
    if (!r.ok()) return T("Hier ist keine Fläche zu finden: Toleranz erhöhen oder auf eine größere, gleichmäßige Fläche klicken");
    if (r.area > 0.6) return T("Die Fläche ist über fast das ganze Bild gelaufen: Toleranz senken oder näher an ihrer Mitte klicken");
    if (!r.warning.isEmpty()) return T("Sehr kleine Fläche: Toleranz erhöhen");
    return T("Fläche gewählt: %1 Punkte").arg(r.polygon.size());
}

} // namespace

int CompOverlay::pickAt(QPointF point, Qt::KeyboardModifiers modifiers)
{
    const Clip* c = clip();
    CompGraph g;
    if (!c || !CompUi::graphOf(*c, &g) || !g.node(m_node) || g.node(m_node)->type != QLatin1String("mask")) return 0;
    const ProjectFormat format = m_editor->project()->timelineFormat();
    RegionPick::Options o;
    o.tolerance = m_tolerance;
    const QImage picture = RegionPick::clipPicture(format, *c, CompUi::clipFrame(*c, m_playhead), o.maxEdge);
    const RegionPick::Result r = RegionPick::pick(picture, point, o);
    m_pickStatus = pickText(r);
    int id = 0;
    if (r.ok()) {
        const RegionPick::Combine mode = modifiers & Qt::ShiftModifier ? RegionPick::Combine::Add
                                         : modifiers & (Qt::AltModifier | Qt::ControlModifier) ? RegionPick::Combine::Subtract
                                                                                               : RegionPick::Combine::Replace;
        QPointF placed(-1, -1);
        QString err;
        id = RegionPick::applyPick(*m_editor, format, m_clipId, m_node, mode, r.polygon, m_playhead, &picture, &placed, &err);
        if (!id) m_pickStatus = T("Nicht übernommen: %1").arg(err);
        else if (placed.x() >= 0) m_pickStatus += T(" · Tracker-Punkt an die Kante gesetzt");
        if (id) m_last = LastPick{m_clipId, id, m_playhead, point, picture};
    }
    emit picked(m_pickStatus);
    emit changed();
    return id;
}

void CompOverlay::setPickTolerance(double tolerance)
{
    tolerance = std::clamp(tolerance, 0.0, 1.0);
    if (std::abs(tolerance - m_tolerance) < 1e-9) return;
    m_tolerance = tolerance;
    QSettings().setValue("comp/pickTolerance", tolerance);
    repick();
}

int CompOverlay::repick()
{
    if (!m_last.clipId || m_last.clipId != m_clipId || m_last.picture.isNull()) return 0;
    const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), m_last.clipId);
    CompGraph g;
    if (!c || !CompUi::graphOf(*c, &g) || !g.node(m_last.node) || g.node(m_last.node)->type != QLatin1String("mask")) {
        m_last = {};
        return 0;
    }
    RegionPick::Options o;
    o.tolerance = m_tolerance;
    const RegionPick::Result r = RegionPick::pick(m_last.picture, m_last.point, o);
    m_pickStatus = pickText(r);
    int id = 0;
    if (r.ok()) {
        QString err;
        id = RegionPick::applyPick(*m_editor, m_editor->project()->timelineFormat(), m_last.clipId, m_last.node,
                                   RegionPick::Combine::Replace, r.polygon, m_last.playhead, nullptr, nullptr, &err);
        if (!id) m_pickStatus = T("Nicht übernommen: %1").arg(err);
    }
    emit picked(m_pickStatus);
    emit changed();
    return id;
}
