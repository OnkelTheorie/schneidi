// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/ShapeOverlay.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace {

constexpr double kHandle = 4.5; // half size of a handle (widget pixels)
constexpr double kHit = 8;      // grab distance
constexpr double kKnob = 22;    // distance of the rotate knob above the box (widget pixels)
constexpr double kHandles[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}};
constexpr int kRotate = 8;

} // namespace

ShapeOverlay::ShapeOverlay(Editor* editor, Selection* selection, std::function<int()> playhead, QObject* parent)
    : ViewerOverlay(parent), m_editor(editor), m_selection(selection), m_playhead(std::move(playhead))
{
    connect(selection, &Selection::changed, this, &ViewerOverlay::changed);
}

const Clip* ShapeOverlay::clip() const
{
    if (!m_selection || m_selection->ids().size() != 1) return nullptr;
    const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), *m_selection->ids().begin());
    if (!c || !c->isTitle() || !c->title.isShape() || c->title.shape.followClip > 0) return nullptr;
    const int f = m_playhead ? m_playhead() : c->start;
    return f >= c->start && f < c->end() ? c : nullptr;
}

int ShapeOverlay::localFrame(const Clip& c) const
{
    return std::clamp((m_playhead ? m_playhead() : c.start) - c.start, 0, std::max(0, c.length() - 1));
}

bool ShapeOverlay::transforms(const QRectF& frame, QTransform* toWidget, QTransform* local, double* hw, double* hh) const
{
    const Clip* c = clip();
    if (!c || frame.isEmpty()) return false;
    const ProjectFormat fmt = m_editor->project()->timelineFormat();
    const double W = fmt.width, H = fmt.height;
    const int t = localFrame(*c);
    QTransform view, flat;
    view.translate(frame.left(), frame.top());
    view.scale(frame.width() / W, frame.height() / H);
    if (Keys::hasTransform(*c)) { // the clip's own transform as the picture engine applies it to the title
        flat.translate(W / 2 + Keys::valueAt(*c, AnimParam::PosX, t), H / 2 - Keys::valueAt(*c, AnimParam::PosY, t));
        flat.rotate(-Keys::valueAt(*c, AnimParam::Rotation, t));
        flat.scale(Keys::valueAt(*c, AnimParam::ZoomX, t), Keys::valueAt(*c, AnimParam::ZoomY, t));
        flat.translate(-W / 2, -H / 2);
    }
    QTransform l;
    l.translate(W / 2 + Keys::valueAt(*c, AnimParam::TitlePosX, t), H / 2 - Keys::valueAt(*c, AnimParam::TitlePosY, t));
    l.rotate(Keys::valueAt(*c, AnimParam::ShapeRotation, t)); // clockwise on screen (y down)
    if (toWidget) *toWidget = flat * view;
    if (local) *local = l;
    if (hw) *hw = std::max(0.5, Keys::valueAt(*c, AnimParam::ShapeWidth, t) / 2);
    if (hh) *hh = std::max(0.5, Keys::valueAt(*c, AnimParam::ShapeHeight, t) / 2);
    return true;
}

QPointF ShapeOverlay::centerPos(const QRectF& frame) const
{
    QTransform w, l;
    if (!transforms(frame, &w, &l, nullptr, nullptr)) return {};
    return w.map(l.map(QPointF(0, 0)));
}

QPointF ShapeOverlay::handlePos(int handle, const QRectF& frame) const
{
    QTransform w, l;
    double hw = 0, hh = 0;
    if (!transforms(frame, &w, &l, &hw, &hh)) return {};
    if (handle == kRotate) {
        const QPointF top = w.map(l.map(QPointF(0, -hh))), center = w.map(l.map(QPointF(0, 0)));
        QPointF dir = top - center;
        double len = std::hypot(dir.x(), dir.y());
        if (len < 1e-6) {
            dir = w.map(l.map(QPointF(0, -1))) - center;
            len = std::hypot(dir.x(), dir.y());
        }
        return top + (len > 1e-9 ? dir / len : QPointF(0, -1)) * kKnob;
    }
    if (handle < 0 || handle > 7) return {};
    return w.map(l.map(QPointF(kHandles[handle][0] * hw, kHandles[handle][1] * hh)));
}

void ShapeOverlay::paint(QPainter& p, const QRectF& frame)
{
    QTransform w, l;
    double hw = 0, hh = 0;
    if (!transforms(frame, &w, &l, &hw, &hh)) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setClipRect(frame.adjusted(-kKnob - 10, -kKnob - 10, kKnob + 10, kKnob + 10));
    QPainterPath box;
    box.addRect(QRectF(-hw, -hh, 2 * hw, 2 * hh));
    const QPainterPath shown = (l * w).map(box);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 140), 3)); // dark under the light line: visible on any picture
    p.drawPath(shown);
    p.setPen(QPen(QColor(255, 255, 255, 230), 1.2, Qt::DashLine));
    p.drawPath(shown);
    const QPointF top = w.map(l.map(QPointF(0, -hh))), knob = handlePos(kRotate, frame);
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
    p.restore();
}

int ShapeOverlay::partAt(const QPointF& pos, const QRectF& frame) const
{
    QTransform w, l;
    double hw = 0, hh = 0;
    if (!transforms(frame, &w, &l, &hw, &hh)) return None;
    const auto near = [&](const QPointF& h) { return std::hypot(h.x() - pos.x(), h.y() - pos.y()) <= kHit; };
    if (near(handlePos(kRotate, frame))) return kRotate;
    for (int i = 0; i < 8; ++i)
        if (near(handlePos(i, frame))) return i;
    bool ok = false;
    const QTransform inv = (l * w).inverted(&ok);
    if (!ok) return None;
    const QPointF q = inv.map(pos);
    return std::abs(q.x()) <= hw && std::abs(q.y()) <= hh ? Inside : None;
}

Qt::CursorShape ShapeOverlay::cursorAt(const QPointF& pos, const QRectF& frame) const
{
    const int part = partAt(pos, frame);
    if (part == None) return Qt::ArrowCursor;
    if (part == Inside) return Qt::SizeAllCursor;
    if (part == kRotate) return Qt::PointingHandCursor;
    return Qt::CrossCursor;
}

void ShapeOverlay::set(const QVector<QPair<int, double>>& values)
{
    const Clip* c = clip();
    if (!c) return;
    const int t = localFrame(*c);
    m_editor->modifyClips({c->id}, T("Form"), [&](Clip& clip) {
        for (const auto& [param, v] : values) Keys::setValue(clip, AnimParam(param), t, v); // animated: a keyframe
    }, QString("shape:overlay:%1").arg(c->id));
}

bool ShapeOverlay::mousePress(QMouseEvent* e, const QRectF& frame)
{
    if (e->button() != Qt::LeftButton) return false;
    const int part = partAt(e->position(), frame);
    if (part == None) return false;
    QTransform w;
    if (!transforms(frame, &w, nullptr, nullptr, nullptr)) return false;
    const Clip* c = clip();
    const int t = localFrame(*c);
    m_part = part;
    m_clipId = c->id;
    m_press = w.inverted().map(e->position());
    m_startX = Keys::valueAt(*c, AnimParam::TitlePosX, t);
    m_startY = Keys::valueAt(*c, AnimParam::TitlePosY, t);
    m_startW = Keys::valueAt(*c, AnimParam::ShapeWidth, t);
    m_startH = Keys::valueAt(*c, AnimParam::ShapeHeight, t);
    return true;
}

bool ShapeOverlay::mouseMove(QMouseEvent* e, const QRectF& frame)
{
    if (m_part == None) return false;
    QTransform w, l;
    if (!transforms(frame, &w, &l, nullptr, nullptr)) return true;
    const QPointF pos = w.inverted().map(e->position()); // project pixels
    if (m_part == Inside) {
        const QPointF d = pos - m_press;
        set({{int(AnimParam::TitlePosX), std::round(m_startX + d.x())}, {int(AnimParam::TitlePosY), std::round(m_startY - d.y())}});
    } else if (m_part == kRotate) {
        const QPointF v = pos - l.map(QPointF(0, 0));
        double angle = std::atan2(v.y(), v.x()) * 180 / M_PI + 90; // clockwise, 0 = the knob straight up
        if (e->modifiers() & Qt::ShiftModifier) angle = std::round(angle / 15) * 15;
        while (angle > 180) angle -= 360;
        while (angle < -180) angle += 360;
        set({{int(AnimParam::ShapeRotation), angle}});
    } else {
        const QPointF q = l.inverted().map(pos); // around the centre
        const bool sx = kHandles[m_part][0] != 0, sy = kHandles[m_part][1] != 0;
        double nw = sx ? std::abs(q.x()) * 2 : m_startW, nh = sy ? std::abs(q.y()) * 2 : m_startH;
        if (sx && sy && (e->modifiers() & Qt::ShiftModifier) && m_startW > 0 && m_startH > 0) {
            const double k = std::max(nw / m_startW, nh / m_startH); // keeps its proportions
            nw = m_startW * k;
            nh = m_startH * k;
        }
        QVector<QPair<int, double>> values;
        if (sx) values.append({int(AnimParam::ShapeWidth), std::round(nw)});
        if (sy) values.append({int(AnimParam::ShapeHeight), std::round(nh)});
        set(values);
    }
    return true;
}

bool ShapeOverlay::mouseRelease(QMouseEvent* e, const QRectF&)
{
    if (m_part == None || e->button() != Qt::LeftButton) return false;
    m_part = None;
    m_editor->project()->closeMerge();
    return true;
}
