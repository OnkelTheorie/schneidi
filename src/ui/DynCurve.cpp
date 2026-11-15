// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/DynCurve.h"

#include "app/Theme.h"
#include "core/Dynamics.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace {
QString number(double v, int decimals)
{
    return QString::number(v, 'f', decimals).replace('.', I18n::decimalPoint());
}
} // namespace

DynCurve::DynCurve(Editor* editor, int track, QWidget* parent) : QWidget(parent), m_editor(editor), m_track(track)
{
    setMinimumSize(200, 120);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    setToolTip(T("Kompressor: Punkt seitlich = Schwelle, Ende hoch/runter = Verhältnis"));
    connect(editor->project(), &Project::timelineChanged, this, qOverload<>(&QWidget::update));
}

TrackDynamics DynCurve::dyn() const
{
    const auto& audio = m_editor->project()->timeline().audio;
    return m_track >= 0 && m_track < audio.size() ? audio[m_track].dynamics : TrackDynamics{};
}

QRectF DynCurve::plot() const { return QRectF(rect()).adjusted(8, 8, -8, -8); }

QPointF DynCurve::at(double in, double out) const
{
    const QRectF r = plot();
    return {r.left() + (in + 60) / 60 * r.width(), r.bottom() - (std::clamp(out, -60.0, 12.0) + 60) / 72 * r.height()};
}

QPointF DynCurve::thresholdPoint() const
{
    const TrackDynamics d = dyn();
    return at(d.thresholdDb, Compressor::curveDb(d, d.thresholdDb));
}

QPointF DynCurve::endPoint() const { return at(0, Compressor::curveDb(dyn(), 0)); }

void DynCurve::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), Theme::lane);
    const QRectF r = plot();
    p.setPen(QPen(Theme::control, 1));
    for (double db : {-48.0, -36.0, -24.0, -12.0}) p.drawLine(at(db, -60), at(db, 12));
    p.setPen(QPen(Theme::controlLight, 1, Qt::DashLine));
    p.drawLine(at(-60, -60), at(0, 0)); // 1:1
    const TrackDynamics d = dyn();
    QPainterPath path;
    for (int x = int(r.left()); x <= int(r.right()); ++x) {
        const double in = (x - r.left()) / r.width() * 60 - 60;
        const QPointF pt = at(in, Compressor::curveDb(d, in));
        if (x == int(r.left())) path.moveTo(pt);
        else path.lineTo(pt);
    }
    p.setPen(QPen(d.on ? Theme::primary : Theme::controlOff, 2));
    p.drawPath(path);
    p.setPen(QPen(Theme::border, 1));
    p.setBrush(d.on ? Theme::secondary : Theme::controlOff);
    p.drawEllipse(thresholdPoint(), 5, 5);
    p.drawEllipse(endPoint(), 5, 5);
    p.setPen(Theme::textDim);
    p.drawText(r.adjusted(4, 2, 0, 0), Qt::AlignLeft | Qt::AlignTop,
               QString("%1 dB  %2:1").arg(number(d.thresholdDb, 0), number(d.ratio, d.ratio < 10 ? 1 : 0)));
}

void DynCurve::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    const QPointF pos = e->position();
    const auto near = [&](const QPointF& a) { return std::hypot(a.x() - pos.x(), a.y() - pos.y()) <= 10; };
    m_drag = near(endPoint()) ? 2 : near(thresholdPoint()) ? 1 : 0;
}

void DynCurve::mouseMoveEvent(QMouseEvent* e)
{
    const QPointF pos = e->position();
    if (!m_drag) {
        const auto near = [&](const QPointF& a) { return std::hypot(a.x() - pos.x(), a.y() - pos.y()) <= 10; };
        setCursor(near(endPoint()) ? Qt::SizeVerCursor : near(thresholdPoint()) ? Qt::SizeHorCursor : Qt::ArrowCursor);
        return;
    }
    if (m_track < 0 || m_track >= m_editor->project()->timeline().audio.size()) return;
    TrackDynamics d = dyn();
    d.on = true;
    const QRectF r = plot();
    if (m_drag == 1) {
        d.thresholdDb = std::round((pos.x() - r.left()) / r.width() * 60 - 60);
    } else {
        // Output at 0 dB input (without make-up and knee): thr - thr / ratio
        const double out = (r.bottom() - pos.y()) / r.height() * 72 - 60 - d.makeupDb;
        const double above = out - d.thresholdDb; // what is left of the -threshold dB above it
        d.ratio = above <= 0.01 ? Compressor::kMaxRatio : std::round(-d.thresholdDb / above * 10) / 10;
    }
    d = Compressor::clamped(d);
    m_editor->setTrackDynamics(m_track, d, QString("dyn-curve-%1").arg(m_track));
}

void DynCurve::mouseReleaseEvent(QMouseEvent*)
{
    if (m_drag) m_editor->project()->closeMerge();
    m_drag = 0;
}

void DynCurve::mouseDoubleClickEvent(QMouseEvent*)
{
    m_drag = 0;
    if (onOpen) onOpen();
}
