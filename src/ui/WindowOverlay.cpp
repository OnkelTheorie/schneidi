// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/WindowOverlay.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/GradeWindows.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/TimelineOps.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace {

constexpr double kHandle = 4.5; // half size of a handle (widget pixels)
constexpr double kHit = 8;      // grab distance
constexpr double kKnob = 22;    // distance of the rotate knob above the shape (widget pixels)

// Handles 0..7 clockwise from the top left (corners and edge middles) in window-local units (-1..1)
constexpr double kHandles[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}};
constexpr int kRotate = 8;

} // namespace

WindowOverlay::WindowOverlay(Editor* editor, std::function<QVector<int>()> targets, QObject* parent)
    : ViewerOverlay(parent), m_editor(editor), m_targets(std::move(targets))
{
}

void WindowOverlay::setTarget(int clipId, int window, int playhead)
{
    if (clipId == m_clipId && window == m_window && playhead == m_playhead) return;
    m_clipId = clipId;
    m_window = window;
    m_playhead = playhead;
    emit changed();
}

void WindowOverlay::setVisible(bool on)
{
    if (on == m_visible) return;
    m_visible = on;
    emit changed();
}

const Clip* WindowOverlay::clip() const
{
    if (!m_visible || !m_window || !m_clipId) return nullptr;
    const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), m_clipId);
    return c && GradeWindows::exists(*c, m_window) ? c : nullptr;
}

bool WindowOverlay::transforms(const QRectF& frame, QTransform* clipToWidget, QTransform* localToClip,
                               QSizeF* picture) const
{
    const Clip* c = clip();
    if (!c || frame.isEmpty()) return false;
    const ProjectFormat& fmt = m_editor->project()->format();
    const double W = fmt.width, H = fmt.height;
    const double t = std::clamp(m_playhead - c->start, 0, std::max(0, c->length() - 1));
    // The window's picture = the clip before transform, filling the frame (a source of another aspect ratio sits
    // inside with bars; the window then refers to the whole frame as well)
    const double iw = W, ih = H;
    QTransform ct;
    ct.translate(frame.left(), frame.top());
    ct.scale(frame.width() / W, frame.height() / H);
    if (Keys::hasTransform(*c)) {
        // Inspector transform like the TimelineBuilder: zoom around the center, position Y upward, rotation
        // counterclockwise
        ct.translate(W / 2 + Keys::valueAt(*c, AnimParam::PosX, t), H / 2 - Keys::valueAt(*c, AnimParam::PosY, t));
        ct.rotate(-Keys::valueAt(*c, AnimParam::Rotation, t));
        ct.scale(Keys::valueAt(*c, AnimParam::ZoomX, t), Keys::valueAt(*c, AnimParam::ZoomY, t));
        ct.translate(-iw / 2, -ih / 2);
    }
    const GradeWindows::Geometry g = GradeWindows::geometryAt(*c, m_window, t);
    QTransform lt;
    lt.translate(g.x / 100 * iw, g.y / 100 * ih);
    lt.rotate(-g.rotation); // counterclockwise on screen, like the mask
    if (clipToWidget) *clipToWidget = ct;
    if (localToClip) *localToClip = lt;
    if (picture) *picture = QSizeF(iw, ih);
    return true;
}

QPointF WindowOverlay::centerPos(const QRectF& frame) const
{
    QTransform ct, lt;
    if (!transforms(frame, &ct, &lt, nullptr)) return {};
    return ct.map(lt.map(QPointF(0, 0)));
}

QPointF WindowOverlay::handlePos(int handle, const QRectF& frame) const
{
    QTransform ct, lt;
    QSizeF pic;
    const Clip* c = clip();
    if (!c || !transforms(frame, &ct, &lt, &pic)) return {};
    const double t = std::clamp(m_playhead - c->start, 0, std::max(0, c->length() - 1));
    const GradeWindows::Geometry g = GradeWindows::geometryAt(*c, m_window, t);
    const double hw = g.width / 100 * pic.width() / 2, hh = g.height / 100 * pic.height() / 2;
    if (handle == kRotate) {
        // Above the top edge, a fixed distance on screen
        const QPointF top = ct.map(lt.map(QPointF(0, -hh)));
        const QPointF center = ct.map(lt.map(QPointF(0, 0)));
        QPointF dir = top - center;
        const double len = std::hypot(dir.x(), dir.y());
        if (len < 1e-6) {
            dir = ct.map(lt.map(QPointF(0, -1))) - center;
            const double l = std::hypot(dir.x(), dir.y());
            return center + (l > 1e-9 ? dir / l : QPointF(0, -1)) * kKnob;
        }
        return top + dir / len * kKnob;
    }
    if (handle < 0 || handle > 7) return {};
    return ct.map(lt.map(QPointF(kHandles[handle][0] * hw, kHandles[handle][1] * hh)));
}

void WindowOverlay::paint(QPainter& p, const QRectF& frame)
{
    QTransform ct, lt;
    QSizeF pic;
    const Clip* c = clip();
    if (!c || !transforms(frame, &ct, &lt, &pic)) return;
    const double t = std::clamp(m_playhead - c->start, 0, std::max(0, c->length() - 1));
    const GradeWindows::Geometry g = GradeWindows::geometryAt(*c, m_window, t);
    const double hw = std::max(0.5, g.width / 100 * pic.width() / 2), hh = std::max(0.5, g.height / 100 * pic.height() / 2);
    const double soft = g.softness / 100 * pic.height();
    const auto shape = [&](double grow) {
        QPainterPath path;
        const QRectF r(-hw - grow, -hh - grow, 2 * (hw + grow), 2 * (hh + grow));
        if (g.shape == GradeWindows::Shape::Ellipse) path.addEllipse(r);
        else if (grow > 0) path.addRoundedRect(r, grow, grow);
        else path.addRect(r);
        return (lt * ct).map(path);
    };
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setClipRect(frame.adjusted(-kKnob - 10, -kKnob - 10, kKnob + 10, kKnob + 10));
    const QPainterPath inner = shape(0);
    // Dark under the light line: visible on bright and dark pictures
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 140), 3));
    p.drawPath(inner);
    p.setPen(QPen(QColor(255, 255, 255, 230), 1.4));
    p.drawPath(inner);
    if (soft > 0.5) {
        QPen dash(QColor(255, 255, 255, 170), 1, Qt::DashLine);
        p.setPen(dash);
        p.drawPath(shape(soft));
    }
    // Rotate knob with its line, then the size handles
    const QPointF top = ct.map(lt.map(QPointF(0, -hh)));
    const QPointF knob = handlePos(kRotate, frame);
    p.setPen(QPen(QColor(255, 255, 255, 200), 1));
    p.drawLine(top, knob);
    p.setBrush(Theme::primary);
    p.setPen(QPen(QColor(0, 0, 0, 160), 1));
    p.drawEllipse(knob, kHandle, kHandle);
    p.setBrush(QColor(255, 255, 255));
    for (int i = 0; i < 8; ++i) {
        const QPointF h = handlePos(i, frame);
        p.drawRect(QRectF(h.x() - kHandle, h.y() - kHandle, 2 * kHandle, 2 * kHandle));
    }
    // Center cross and number
    const QPointF center = ct.map(lt.map(QPointF(0, 0)));
    p.setPen(QPen(QColor(255, 255, 255, 200), 1));
    p.drawLine(center - QPointF(5, 0), center + QPointF(5, 0));
    p.drawLine(center - QPointF(0, 5), center + QPointF(0, 5));
    p.setPen(QColor(255, 255, 255, 220));
    p.drawText(center + QPointF(6, -6), QString::number(m_window));
    p.restore();
}

int WindowOverlay::partAt(const QPointF& pos, const QRectF& frame) const
{
    QTransform ct, lt;
    QSizeF pic;
    const Clip* c = clip();
    if (!c || !transforms(frame, &ct, &lt, &pic)) return None;
    const auto near = [&](const QPointF& h) { return std::hypot(h.x() - pos.x(), h.y() - pos.y()) <= kHit; };
    if (near(handlePos(kRotate, frame))) return kRotate;
    for (int i = 0; i < 8; ++i)
        if (near(handlePos(i, frame))) return i;
    bool ok = false;
    const QTransform inv = (lt * ct).inverted(&ok);
    if (!ok) return None;
    const QPointF l = inv.map(pos);
    const double t = std::clamp(m_playhead - c->start, 0, std::max(0, c->length() - 1));
    const GradeWindows::Geometry g = GradeWindows::geometryAt(*c, m_window, t);
    const double hw = std::max(0.5, g.width / 100 * pic.width() / 2), hh = std::max(0.5, g.height / 100 * pic.height() / 2);
    const bool inside = g.shape == GradeWindows::Shape::Ellipse
                            ? (l.x() * l.x()) / (hw * hw) + (l.y() * l.y()) / (hh * hh) <= 1
                            : std::abs(l.x()) <= hw && std::abs(l.y()) <= hh;
    return inside ? Inside : None;
}

Qt::CursorShape WindowOverlay::cursorAt(const QPointF& pos, const QRectF& frame) const
{
    const int part = partAt(pos, frame);
    if (part == None) return Qt::ArrowCursor;
    if (part == Inside) return Qt::SizeAllCursor;
    if (part == kRotate) return Qt::PointingHandCursor;
    return Qt::CrossCursor;
}

void WindowOverlay::set(const QVector<QPair<QString, double>>& values)
{
    QVector<QPair<AnimParam, double>> list;
    for (const auto& [key, v] : values) {
        AnimParam p = GradeWindows::param(m_window, key);
        double lo = 0, hi = 0;
        Keys::range(p, &lo, &hi);
        list.append({p, lo < hi ? std::clamp(v, lo, hi) : v});
    }
    m_editor->setGradeValues(m_targets(), list, m_playhead, T("Fenster"), QString("window:overlay:%1").arg(m_window));
}

bool WindowOverlay::mousePress(QMouseEvent* e, const QRectF& frame)
{
    if (e->button() != Qt::LeftButton) return false;
    const int part = partAt(e->position(), frame);
    if (part == None) return false;
    QTransform ct;
    if (!transforms(frame, &ct, nullptr, nullptr)) return false;
    const Clip* c = clip();
    const double t = std::clamp(m_playhead - c->start, 0, std::max(0, c->length() - 1));
    const GradeWindows::Geometry g = GradeWindows::geometryAt(*c, m_window, t);
    m_part = part;
    m_pressClip = ct.inverted().map(e->position());
    m_startX = g.x;
    m_startY = g.y;
    return true;
}

bool WindowOverlay::mouseMove(QMouseEvent* e, const QRectF& frame)
{
    if (m_part == None) return false;
    QTransform ct, lt;
    QSizeF pic;
    if (!transforms(frame, &ct, &lt, &pic)) return true;
    const QPointF pos = ct.inverted().map(e->position()); // clip picture pixels
    if (m_part == Inside) {
        const QPointF d = pos - m_pressClip;
        set({{"x", m_startX + d.x() / pic.width() * 100}, {"y", m_startY + d.y() / pic.height() * 100}});
    } else if (m_part == kRotate) {
        const QPointF center = lt.map(QPointF(0, 0));
        const QPointF v = pos - center;
        // Angle counterclockwise on screen; the knob sits at the top (90 degrees) when the rotation is 0
        double angle = std::atan2(-v.y(), v.x()) * 180 / M_PI - 90;
        if (e->modifiers() & Qt::ShiftModifier) angle = std::round(angle / 15) * 15; // steps like DaVinci with Shift
        while (angle > 180) angle -= 360;
        while (angle < -180) angle += 360;
        set({{"rotation", angle}});
    } else {
        // Size around the center: the handle follows the mouse in window-local coordinates
        const QPointF l = lt.inverted().map(pos);
        QVector<QPair<QString, double>> values;
        if (kHandles[m_part][0] != 0) values.append({"width", std::abs(l.x()) * 2 / pic.width() * 100});
        if (kHandles[m_part][1] != 0) values.append({"height", std::abs(l.y()) * 2 / pic.height() * 100});
        set(values);
    }
    return true;
}

bool WindowOverlay::mouseRelease(QMouseEvent* e, const QRectF&)
{
    if (m_part == None || e->button() != Qt::LeftButton) return false;
    m_part = None;
    m_editor->project()->closeMerge();
    return true;
}
