// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/DrawOverlay.h"

#include "app/Theme.h"
#include "core/Drawing.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"

#include <QColorDialog>
#include <QEvent>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointingDevice>
#include <QSettings>
#include <QSpinBox>
#include <QTabletEvent>
#include <QToolButton>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kHandle = 4.5; // half size of a corner handle (widget pixels)
constexpr double kHit = 8;      // grab distance
constexpr double kEraser = 6;   // smallest eraser radius (widget pixels)

} // namespace

// ---------- DrawBrush ----------

DrawBrush& DrawBrush::instance()
{
    static DrawBrush* brush = new DrawBrush; // lives as long as the app (a QObject without parent)
    return *brush;
}

DrawBrush::DrawBrush()
{
    const QSettings s;
    const QColor c(s.value("draw/color").toString());
    if (c.isValid()) m_color = c;
    if (s.contains("draw/widthPx")) m_widthPx = s.value("draw/widthPx").toDouble();
    else if (s.contains("draw/width")) m_widthPx = s.value("draw/width").toDouble() * 1080; // 0.22 betas: of the frame height
    m_widthPx = std::clamp(m_widthPx, kMinPx, kMaxPx);
    m_opacity = std::clamp(s.value("draw/opacity", m_opacity).toDouble(), 0.05, 1.0);
}

void DrawBrush::save() const
{
    QSettings s;
    s.setValue("draw/color", m_color.name());
    s.setValue("draw/widthPx", m_widthPx);
    s.setValue("draw/opacity", m_opacity);
}

void DrawBrush::setColor(const QColor& c)
{
    if (!c.isValid() || c == m_color) return;
    m_color = c;
    save();
    emit changed();
}

double DrawBrush::width(int frameHeight) const
{
    return std::min(0.2, m_widthPx / std::max(1, frameHeight));
}

void DrawBrush::setWidthPx(double px)
{
    px = std::clamp(px, kMinPx, kMaxPx);
    if (px == m_widthPx) return;
    m_widthPx = px;
    save();
    emit changed();
}

void DrawBrush::setOpacity(double o)
{
    o = std::clamp(o, 0.05, 1.0);
    if (o == m_opacity) return;
    m_opacity = o;
    save();
    emit changed();
}

void DrawBrush::setEraser(bool on)
{
    if (on == m_eraser) return;
    m_eraser = on;
    emit changed();
}

// ---------- DrawOverlay ----------

DrawOverlay::DrawOverlay(Editor* editor, Selection* selection, std::function<int()> playhead, QObject* parent,
                         std::function<bool()> timelineShown, ViewerOverlay* below)
    : ViewerOverlay(parent), m_editor(editor), m_selection(selection), m_playhead(std::move(playhead)),
      m_timelineShown(std::move(timelineShown)), m_below(below)
{
    connect(selection, &Selection::changed, this, &ViewerOverlay::changed);
    connect(&DrawBrush::instance(), &DrawBrush::changed, this, &ViewerOverlay::changed);
    if (below) connect(below, &ViewerOverlay::changed, this, &ViewerOverlay::changed);
}

void DrawOverlay::setActive(bool on)
{
    if (on == m_active) return;
    m_active = on;
    m_painting = m_erasing = m_tablet = false;
    m_stroke.clear();
    m_pressure.clear();
    emit activeChanged(on);
    emit changed();
}

const Clip* DrawOverlay::drawingAt() const
{
    if (!m_selection || m_selection->ids().size() != 1) return nullptr;
    if (m_timelineShown && !m_timelineShown()) return nullptr; // a source clip in the viewer: not the timeline's
    const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), *m_selection->ids().begin());
    if (!c || !c->isTitle() || !c->title.isDrawing()) return nullptr;
    const int f = m_playhead ? m_playhead() : c->start;
    return f >= c->start && f < c->end() ? c : nullptr;
}

bool DrawOverlay::locked(const Clip& c) const
{
    TrackRef ref;
    return TimelineOps::findClip(m_editor->project()->timeline(), c.id, &ref) && m_editor->isTrackLocked(ref);
}

int DrawOverlay::target() const
{
    const Clip* c = drawingAt();
    // following a tracker or on a locked track: the stroke goes into a new drawing on a free track
    return c && c->title.shape.followClip <= 0 && !locked(*c) ? c->id : 0;
}

int DrawOverlay::localFrame(const Clip& c) const
{
    return std::clamp((m_playhead ? m_playhead() : c.start) - c.start, 0, std::max(0, c.length() - 1));
}

QTransform DrawOverlay::toWidget(const Clip* c, const QRectF& frame) const
{
    const ProjectFormat fmt = m_editor->project()->timelineFormat();
    const double W = fmt.width, H = fmt.height;
    QTransform view, flat;
    view.translate(frame.left(), frame.top());
    view.scale(frame.width() / W, frame.height() / H);
    if (c && Keys::hasTransform(*c)) { // the clip's own transform as the picture engine applies it to the title
        const int t = localFrame(*c);
        flat.translate(W / 2 + Keys::valueAt(*c, AnimParam::PosX, t), H / 2 - Keys::valueAt(*c, AnimParam::PosY, t));
        flat.rotate(-Keys::valueAt(*c, AnimParam::Rotation, t));
        flat.scale(Keys::valueAt(*c, AnimParam::ZoomX, t), Keys::valueAt(*c, AnimParam::ZoomY, t));
        flat.translate(-W / 2, -H / 2);
    }
    return flat * view;
}

QPointF DrawOverlay::toDrawing(const Clip* c, const QPointF& widget, const QRectF& frame) const
{
    const ProjectFormat fmt = m_editor->project()->timelineFormat();
    bool ok = false;
    QPointF q = toWidget(c, frame).inverted(&ok).map(widget);
    if (!ok) q = QPointF(fmt.width / 2.0, fmt.height / 2.0);
    if (c) {
        const int t = localFrame(*c);
        q -= QPointF(Keys::valueAt(*c, AnimParam::TitlePosX, t), -Keys::valueAt(*c, AnimParam::TitlePosY, t));
    }
    return QPointF(q.x() / fmt.width, q.y() / fmt.height);
}

QRectF DrawOverlay::localBox(const Clip& c) const
{
    const ProjectFormat fmt = m_editor->project()->timelineFormat();
    return Drawing::bounds(Drawing::place(c, localFrame(c), fmt.width, fmt.height));
}

QRectF DrawOverlay::boxRect(const QRectF& frame) const
{
    if (m_active) return {};
    const Clip* c = drawingAt();
    if (!c || c->title.shape.followClip > 0) return {};
    const QRectF b = localBox(*c);
    if (b.isEmpty()) return {};
    return toWidget(c, frame).mapRect(b);
}

int DrawOverlay::partAt(const QPointF& pos, const QRectF& frame) const
{
    if (m_active) return None;
    const Clip* c = drawingAt();
    if (!c || c->title.shape.followClip > 0) return None;
    const QRectF b = localBox(*c);
    if (b.isEmpty()) return None;
    const QTransform w = toWidget(c, frame);
    const QPointF corners[4] = {b.topLeft(), b.topRight(), b.bottomRight(), b.bottomLeft()};
    for (int i = 0; i < 4; ++i) {
        const QPointF h = w.map(corners[i]);
        if (std::hypot(h.x() - pos.x(), h.y() - pos.y()) <= kHit) return i;
    }
    bool ok = false;
    const QPointF q = w.inverted(&ok).map(pos);
    return ok && b.contains(q) ? Inside : None;
}

void DrawOverlay::paint(QPainter& p, const QRectF& frame)
{
    if (m_below) m_below->paint(p, frame);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const DrawBrush& brush = DrawBrush::instance();
    if (!m_active) {
        if (const Clip* c = drawingAt(); c && c->title.shape.followClip <= 0) {
            const QRectF b = localBox(*c);
            if (!b.isEmpty()) {
                const QTransform w = toWidget(c, frame);
                QPainterPath box;
                box.addRect(b);
                const QPainterPath shown = w.map(box);
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(QColor(0, 0, 0, 140), 3)); // dark under the light line: visible on any picture
                p.drawPath(shown);
                p.setPen(QPen(QColor(255, 255, 255, 230), 1.2, Qt::DashLine));
                p.drawPath(shown);
                p.setBrush(QColor(255, 255, 255));
                p.setPen(QPen(QColor(0, 0, 0, 160), 1));
                for (const QPointF& corner : {b.topLeft(), b.topRight(), b.bottomRight(), b.bottomLeft()}) {
                    const QPointF h = w.map(corner);
                    p.drawRect(QRectF(h.x() - kHandle, h.y() - kHandle, 2 * kHandle, 2 * kHandle));
                }
            }
        }
        p.restore();
        return;
    }
    p.setClipRect(frame);
    const ProjectFormat fmt = m_editor->project()->timelineFormat();
    const double scale = frame.height() / std::max(1, fmt.height);
    if (m_painting && !m_stroke.isEmpty()) { // the stroke being painted, as it will look
        QColor col = brush.color();
        const double width = std::max(1.0, brush.width(fmt.height) * fmt.height * scale);
        p.setOpacity(brush.opacity());
        if (m_stroke.size() == 1) {
            p.setPen(Qt::NoPen);
            p.setBrush(col);
            p.drawEllipse(m_stroke[0], width / 2, width / 2);
        } else {
            QPainterPath path(m_stroke[0]);
            for (int i = 1; i < m_stroke.size(); ++i) path.lineTo(m_stroke[i]);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(col, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPath(path);
        }
        p.setOpacity(1);
    }
    if (m_hovering || m_erasing) { // where the brush or eraser touches
        const double r = std::max(brush.eraser() ? kEraser : 1.0, brush.width(fmt.height) * fmt.height * scale / 2);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 150), 2.5));
        p.drawEllipse(m_hover, r, r);
        p.setPen(QPen(brush.eraser() ? QColor(255, 255, 255, 220) : brush.color(), 1.2));
        p.drawEllipse(m_hover, r, r);
    }
    p.restore();
}

bool DrawOverlay::mousePress(QMouseEvent* e, const QRectF& frame)
{
    if (!m_active) {
        if (e->button() == Qt::LeftButton) {
            const int part = partAt(e->position(), frame);
            if (part != None) {
                const Clip* c = drawingAt();
                const int t = localFrame(*c);
                m_part = part;
                m_clipId = c->id;
                bool ok = false;
                m_press = part == Inside ? toWidget(c, frame).inverted(&ok).map(e->position()) : e->position();
                m_startX = Keys::valueAt(*c, AnimParam::TitlePosX, t);
                m_startY = Keys::valueAt(*c, AnimParam::TitlePosY, t);
                const bool tr = Keys::hasTransform(*c);
                m_startZoomX = tr ? Keys::valueAt(*c, AnimParam::ZoomX, t) : 1.0;
                m_startZoomY = tr ? Keys::valueAt(*c, AnimParam::ZoomY, t) : 1.0;
                m_startPosX = tr ? Keys::valueAt(*c, AnimParam::PosX, t) : 0.0;
                m_startPosY = tr ? Keys::valueAt(*c, AnimParam::PosY, t) : 0.0;
                return true;
            }
        }
        m_belowDrag = m_below && m_below->mousePress(e, frame);
        return m_belowDrag;
    }
    if (e->button() != Qt::LeftButton) return false;
    if (m_timelineShown && !m_timelineShown()) return false; // a source clip: nothing of the timeline to paint on
    m_hover = e->position();
    if (DrawBrush::instance().eraser()) {
        m_erasing = true;
        m_lockedSaid = false;
        ++m_eraseSerial;
        m_stroke = {e->position()};
        erase(frame);
        return true;
    }
    m_painting = true;
    m_stroke = {e->position()};
    const QPointingDevice* dev = e->pointingDevice();
    const bool pen = dev && dev->type() == QInputDevice::DeviceType::Stylus;
    m_pressure = {pen ? float(std::clamp(e->points().value(0).pressure(), 0.0, 1.0)) : 1.0f};
    emit changed();
    return true;
}

bool DrawOverlay::mouseMove(QMouseEvent* e, const QRectF& frame)
{
    if (m_part != None) {
        const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), m_clipId);
        if (!c) return true;
        const int t = localFrame(*c);
        const ProjectFormat fmt = m_editor->project()->timelineFormat();
        const double W = fmt.width, H = fmt.height;
        QVector<QPair<AnimParam, double>> values;
        if (m_part == Inside) {
            // in the drawing's own space (before its transform): a move of the title position
            bool ok = false;
            const QPointF d = toWidget(c, frame).inverted(&ok).map(e->position()) - m_press;
            values = {{AnimParam::TitlePosX, std::round(m_startX + d.x())}, {AnimParam::TitlePosY, std::round(m_startY - d.y())}};
        } else {
            // A corner: the zoom by how much farther from the box's middle the mouse is than at the press; the
            // position keeps that middle where it is
            const QRectF b = localBox(*c);
            const QPointF mid = toWidget(c, frame).map(b.center());
            const double before = std::hypot(m_press.x() - mid.x(), m_press.y() - mid.y());
            const double now = std::hypot(e->position().x() - mid.x(), e->position().y() - mid.y());
            if (before < 1) return true;
            const double k = now / before;
            const double zx = std::clamp(m_startZoomX * k, 0.01, 100.0), zy = std::clamp(m_startZoomY * k, 0.01, 100.0);
            QTransform r;
            r.rotate(-(Keys::hasTransform(*c) ? Keys::valueAt(*c, AnimParam::Rotation, t) : 0.0));
            // the middle of the box at the press (its zoom then) in the drawing's space
            const QPointF c0 = b.center();
            const QPointF v = r.map(QPointF((m_startZoomX - zx) * (c0.x() - W / 2), (m_startZoomY - zy) * (c0.y() - H / 2)));
            values = {{AnimParam::ZoomX, zx}, {AnimParam::ZoomY, zy}, {AnimParam::PosX, m_startPosX + v.x()},
                      {AnimParam::PosY, m_startPosY - v.y()}};
        }
        m_editor->modifyClips({m_clipId}, T("Zeichnung"), [&](Clip& clip) {
            if (m_part != Inside) clip.transform.transformOn = true;
            for (const auto& [param, v] : values) Keys::setValue(clip, param, t, v); // animated: a keyframe
        }, QString("draw:overlay:%1").arg(m_clipId));
        return true;
    }
    if (!m_active) return m_belowDrag && m_below ? m_below->mouseMove(e, frame) : false;
    m_hover = e->position();
    if (m_erasing) {
        m_stroke << e->position();
        if (m_stroke.size() > 2) m_stroke.remove(0, m_stroke.size() - 2); // the last piece of the eraser's path
        erase(frame);
        emit changed();
        return true;
    }
    if (!m_painting) return false;
    const QPointingDevice* dev = e->pointingDevice();
    const bool pen = dev && dev->type() == QInputDevice::DeviceType::Stylus;
    extendStroke(e->position(), pen ? float(std::clamp(e->points().value(0).pressure(), 0.0, 1.0)) : 1.0f);
    return true;
}

void DrawOverlay::extendStroke(const QPointF& pos, float pressure)
{
    const QPointF& last = m_stroke.last();
    if (std::hypot(pos.x() - last.x(), pos.y() - last.y()) < 0.75) return;
    m_stroke << pos;
    m_pressure << pressure;
    emit changed();
}

bool DrawOverlay::tablet(QTabletEvent* e, const QRectF& frame)
{
    // The pen's own events carry its pressure (the mouse events Qt makes of them do not); the eraser and the box
    // take those mouse events
    if (!m_active || DrawBrush::instance().eraser() || (m_timelineShown && !m_timelineShown())) return false;
    const float pressure = float(std::clamp(e->pressure(), 0.0, 1.0));
    switch (e->type()) {
    case QEvent::TabletPress:
        if (e->button() != Qt::LeftButton || m_painting) return false;
        m_painting = m_tablet = true;
        m_hover = e->position();
        m_stroke = {e->position()};
        m_pressure = {pressure};
        emit changed();
        return true;
    case QEvent::TabletMove:
        if (!m_painting || !m_tablet) return false; // hovering: the mouse event shows where it is
        m_hover = e->position();
        extendStroke(e->position(), pressure);
        return true;
    case QEvent::TabletRelease:
        if (!m_painting || !m_tablet) return false;
        m_painting = m_tablet = false;
        commitStroke(frame);
        m_stroke.clear();
        m_pressure.clear();
        emit changed();
        return true;
    default: return false;
    }
}

bool DrawOverlay::mouseRelease(QMouseEvent* e, const QRectF& frame)
{
    if (m_part != None) {
        if (e->button() != Qt::LeftButton) return true;
        m_part = None;
        m_editor->project()->closeMerge();
        return true;
    }
    if (!m_active) {
        const bool took = m_belowDrag && m_below && m_below->mouseRelease(e, frame);
        m_belowDrag = false;
        return took;
    }
    if (e->button() != Qt::LeftButton) return m_painting || m_erasing;
    if (m_erasing) {
        m_erasing = false;
        m_stroke.clear();
        m_editor->project()->closeMerge();
        emit changed();
        return true;
    }
    if (!m_painting) return false;
    m_painting = false;
    commitStroke(frame);
    m_stroke.clear();
    m_pressure.clear();
    emit changed();
    return true;
}

void DrawOverlay::commitStroke(const QRectF& frame)
{
    if (m_stroke.isEmpty()) return;
    const ProjectFormat fmt = m_editor->project()->timelineFormat();
    const int id = target();
    const Clip* c = id ? TimelineOps::findClip(m_editor->project()->timeline(), id) : nullptr;
    const DrawBrush& brush = DrawBrush::instance();
    DrawStroke s;
    s.color = brush.color();
    s.opacity = brush.opacity();
    s.width = brush.width(fmt.height);
    if (c && Keys::hasTransform(*c)) { // as wide on the screen as the brush, whatever the drawing's zoom
        const int t = localFrame(*c);
        const double z = std::sqrt(std::abs(Keys::valueAt(*c, AnimParam::ZoomX, t) * Keys::valueAt(*c, AnimParam::ZoomY, t)));
        if (z > 1e-3) s.width = std::min(s.width / z, Drawing::kMaxWidth); // as a file keeps it
    }
    for (const QPointF& w : m_stroke) s.points << Drawing::within(toDrawing(c, w, frame)); // far off: at -1..2
    if (std::any_of(m_pressure.begin(), m_pressure.end(), [](float v) { return v < 0.999f; })) s.pressure = m_pressure;
    Drawing::smooth(s, fmt.width, fmt.height);
    if (c) {
        m_editor->modifyClips({id}, T("Zeichnen"), [&](Clip& clip) { clip.title.drawing.strokes << s; });
        m_editor->project()->closeMerge(); // the next stroke is its own undo step
        return;
    }
    const int playhead = m_playhead ? m_playhead() : 0;
    m_editor->addTitle(playhead, -1, 0, TitleKind::Drawing, [&](Clip& clip) { clip.title.drawing.strokes << s; });
    m_editor->project()->closeMerge();
}

void DrawOverlay::erase(const QRectF& frame)
{
    const Clip* c = drawingAt();
    if (!c || c->title.shape.followClip > 0 || m_stroke.isEmpty() || c->title.drawing.strokes.isEmpty()) return;
    if (locked(*c)) {
        if (!m_lockedSaid) emit notice(T("Die Spur der Zeichnung ist gesperrt"));
        m_lockedSaid = true;
        return;
    }
    const int id = c->id;
    const ProjectFormat fmt = m_editor->project()->timelineFormat();
    QVector<QPointF> path;
    for (const QPointF& w : m_stroke) {
        const QPointF q = toDrawing(c, w, frame);
        path << QPointF(q.x() * fmt.width, q.y() * fmt.height);
    }
    // radius in the drawing's pixels: the brush's half width, at least a few screen pixels
    const QTransform tw = toWidget(c, frame);
    const double perPixel = std::max(1e-6, std::sqrt(std::abs(tw.determinant()))); // widget pixels per drawing pixel
    // the brush is as wide on the screen whatever the drawing's zoom (as a new stroke): its pixels divided by the zoom
    double zoom = 1;
    if (Keys::hasTransform(*c)) {
        const int t = localFrame(*c);
        zoom = std::sqrt(std::abs(Keys::valueAt(*c, AnimParam::ZoomX, t) * Keys::valueAt(*c, AnimParam::ZoomY, t)));
    }
    const double radius = std::max(DrawBrush::instance().width(fmt.height) * fmt.height / 2 / std::max(1e-3, zoom), kEraser / perPixel);
    QVector<int> gone = Drawing::hits(c->title.drawing, path, radius, fmt.width, fmt.height);
    if (gone.isEmpty()) return;
    m_editor->modifyClips({id}, T("Radieren"), [&](Clip& clip) {
        QVector<DrawStroke>& strokes = clip.title.drawing.strokes;
        for (int k = int(gone.size()) - 1; k >= 0; --k)
            if (gone[k] < strokes.size()) strokes.remove(gone[k]);
    }, QString("draw:erase:%1:%2").arg(id).arg(m_eraseSerial));
}

Qt::CursorShape DrawOverlay::cursorAt(const QPointF& pos, const QRectF& frame) const
{
    if (m_active) return m_timelineShown && !m_timelineShown() ? Qt::ArrowCursor : Qt::CrossCursor;
    const int part = partAt(pos, frame);
    if (part == Inside) return Qt::SizeAllCursor;
    if (part == 0 || part == 2) return Qt::SizeFDiagCursor;
    if (part == 1 || part == 3) return Qt::SizeBDiagCursor;
    return m_below ? m_below->cursorAt(pos, frame) : Qt::ArrowCursor;
}

void DrawOverlay::hover(const QPointF& pos, const QRectF& frame)
{
    if (!m_active) {
        if (m_below) m_below->hover(pos, frame);
        return;
    }
    m_hover = pos;
    m_hovering = frame.contains(pos);
    emit changed();
}

void DrawOverlay::leave()
{
    if (m_below) m_below->leave();
    if (m_hovering) {
        m_hovering = false;
        emit changed();
    }
}

// ---------- DrawBar ----------

DrawBar::DrawBar(std::function<int()> frameHeight, QWidget* parent) : QWidget(parent), m_frameHeight(std::move(frameHeight))
{
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Theme::panelHeader);
    setPalette(pal);
    auto* rows = new QVBoxLayout(this);
    rows->setContentsMargins(6, 3, 6, 3);
    rows->setSpacing(2);
    m_row1 = new QHBoxLayout;
    m_row2 = new QHBoxLayout;
    for (QHBoxLayout* r : {m_row1, m_row2}) {
        r->setContentsMargins(0, 0, 0, 0);
        r->setSpacing(4);
        rows->addLayout(r);
    }
    const auto tool = [this](const QString& text, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        return b;
    };
    m_brush = tool(T("Pinsel"), T("Striche malen (ein Strich = ein Rückgängig-Schritt)"));
    m_eraser = tool(T("Radierer"), T("Entfernt die Striche, die er berührt"));
    m_color = new QToolButton(this);
    m_color->setToolTip(T("Farbe neuer Striche"));
    m_color->setAutoRaise(true);
    m_color->setFocusPolicy(Qt::NoFocus);
    m_width = new QSpinBox(this);
    m_width->setRange(1, 216);
    m_width->setSuffix(" px");
    m_width->setToolTip(T("Breite neuer Striche in Pixeln des Timeline-Bilds"));
    m_width->setFocusPolicy(Qt::ClickFocus);
    m_opacity = new QSpinBox(this);
    m_opacity->setRange(5, 100);
    m_opacity->setSuffix(" %");
    m_opacity->setToolTip(T("Deckkraft neuer Striche"));
    m_opacity->setFocusPolicy(Qt::ClickFocus);
    auto* done = m_done = new QToolButton(this);
    done->setText(T("Fertig"));
    done->setToolTip(T("Malen beenden (P)"));
    done->setAutoRaise(true);
    done->setFocusPolicy(Qt::NoFocus);
    m_row1->addWidget(m_brush);
    m_row1->addWidget(m_eraser);
    m_row1->addWidget(m_color);
    m_row1->addWidget(m_width);
    m_row1->addWidget(m_opacity);
    m_row1->addWidget(done);
    m_row1->addStretch(1);
    m_row2->addStretch(1);
    // Over the viewer's picture without a layout: follows its width (a narrow window), never makes it wider
    if (parent) parent->installEventFilter(this);
    DrawBrush& b = DrawBrush::instance();
    connect(m_brush, &QToolButton::clicked, this, [] { DrawBrush::instance().setEraser(false); });
    connect(m_eraser, &QToolButton::clicked, this, [] { DrawBrush::instance().setEraser(true); });
    connect(m_color, &QToolButton::clicked, this, [this] {
        const QColor c = QColorDialog::getColor(DrawBrush::instance().color(), this, T("Farbe neuer Striche"));
        if (c.isValid()) DrawBrush::instance().setColor(c);
    });
    connect(m_width, &QSpinBox::valueChanged, this, [](int px) { DrawBrush::instance().setWidthPx(px); });
    connect(m_opacity, &QSpinBox::valueChanged, this, [](int pct) { DrawBrush::instance().setOpacity(pct / 100.0); });
    connect(done, &QToolButton::clicked, this, &DrawBar::closeWanted);
    connect(&b, &DrawBrush::changed, this, &DrawBar::sync);
    sync();
}

bool DrawBar::eventFilter(QObject* watched, QEvent* e)
{
    if (watched == parentWidget() && e->type() == QEvent::Resize && isVisible()) place();
    return QWidget::eventFilter(watched, e);
}

void DrawBar::place()
{
    const int room = parentWidget() ? std::max(1, parentWidget()->width() - 2 * kMargin) : QWIDGETSIZE_MAX;
    // One row when it fits, else the opacity and "Done" in a second row
    const auto rowWidth = [this] {
        int w = 12 + 4 * 5; // margins and spacing
        for (QWidget* x : {static_cast<QWidget*>(m_brush), static_cast<QWidget*>(m_eraser), static_cast<QWidget*>(m_color),
                           static_cast<QWidget*>(m_width), static_cast<QWidget*>(m_opacity), static_cast<QWidget*>(m_done)})
            w += x->sizeHint().width();
        return w;
    };
    const bool two = rowWidth() > room;
    if (two != (m_row2->indexOf(m_done) >= 0)) {
        for (QWidget* x : {static_cast<QWidget*>(m_opacity), static_cast<QWidget*>(m_done)}) {
            (two ? m_row1 : m_row2)->removeWidget(x);
            QHBoxLayout* to = two ? m_row2 : m_row1;
            to->insertWidget(to->count() - 1, x); // before the stretch
        }
    }
    layout()->activate();
    const QSize hint = sizeHint();
    resize(std::min(hint.width(), room), hint.height());
    move(kMargin, kMargin);
    raise();
}

void DrawBar::sync()
{
    const DrawBrush& b = DrawBrush::instance();
    m_brush->setChecked(!b.eraser());
    m_eraser->setChecked(b.eraser());
    QPixmap px(14, 14);
    px.fill(b.color());
    m_color->setIcon(QIcon(px));
    const QSignalBlocker b1(m_width), b2(m_opacity);
    const int h = std::max(1, m_frameHeight ? m_frameHeight() : 1080);
    // Pixels of the timeline frame; at most a fifth of its height (DrawBrush::width)
    m_width->setMaximum(std::clamp(int(std::lround(0.2 * h)), 1, int(DrawBrush::kMaxPx)));
    m_width->setValue(std::max(1, int(std::lround(std::min(b.widthPx(), 0.2 * h)))));
    m_opacity->setValue(int(std::lround(b.opacity() * 100)));
}
