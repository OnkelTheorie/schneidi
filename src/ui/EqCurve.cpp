// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/EqCurve.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/Equalizer.h"
#include "core/Project.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace {
constexpr double kRange = 24; // dB above and below 0
constexpr int kRate = 48000;  // for the curve (the response hardly depends on it in the audible range)

const TrackEq& eqOf(Editor* editor, int track)
{
    static const TrackEq none;
    const Timeline& tl = editor->project()->timeline();
    return track >= 0 && track < tl.audio.size() ? tl.audio[track].eq : none;
}

bool hasGain(const EqBand& b) { return b.kind != EqBand::Kind::HighPass && b.kind != EqBand::Kind::LowPass; }
} // namespace

EqCurve::EqCurve(Editor* editor, int track, QWidget* parent) : QWidget(parent), m_editor(editor), m_track(track)
{
    setMinimumSize(320, 160);
    setMouseTracking(true);
    setToolTip(T("Punkte ziehen: seitlich = Frequenz, hoch/runter = Verstärkung\nDoppelklick auf einen Punkt = 0 dB"));
    connect(editor->project(), &Project::timelineChanged, this, qOverload<>(&QWidget::update));
}

QRectF EqCurve::plot() const { return QRectF(rect()).adjusted(28, 8, -8, -18); }

double EqCurve::xOf(double hz) const
{
    const QRectF r = plot();
    const double t = std::log10(std::clamp(hz, Equalizer::kMinFreq, Equalizer::kMaxFreq) / Equalizer::kMinFreq)
                     / std::log10(Equalizer::kMaxFreq / Equalizer::kMinFreq);
    return r.left() + t * r.width();
}

double EqCurve::hzAt(double x) const
{
    const QRectF r = plot();
    const double t = std::clamp((x - r.left()) / r.width(), 0.0, 1.0);
    return Equalizer::kMinFreq * std::pow(Equalizer::kMaxFreq / Equalizer::kMinFreq, t);
}

double EqCurve::yOf(double db) const
{
    const QRectF r = plot();
    return r.center().y() - std::clamp(db, -kRange, kRange) / kRange * r.height() / 2;
}

double EqCurve::dbAt(double y) const
{
    const QRectF r = plot();
    return std::clamp((r.center().y() - y) / (r.height() / 2) * kRange, -kRange, kRange);
}

QPointF EqCurve::pointFor(int band) const
{
    const TrackEq& eq = eqOf(m_editor, m_track);
    if (band < 0 || band >= int(eq.bands.size())) return {};
    const EqBand& b = eq.bands[size_t(band)];
    return {xOf(b.freq), yOf(hasGain(b) ? b.gainDb : 0)};
}

int EqCurve::bandAt(const QPointF& p) const
{
    int best = -1;
    double bestDist = 12;
    for (int i = 0; i < 6; ++i) {
        const double d = std::hypot(pointFor(i).x() - p.x(), pointFor(i).y() - p.y());
        if (d <= bestDist) {
            best = i;
            bestDist = d;
        }
    }
    return best;
}

void EqCurve::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    m_drag = bandAt(e->position());
    if (m_drag >= 0) m_grab = e->position() - pointFor(m_drag);
}

void EqCurve::mouseMoveEvent(QMouseEvent* e)
{
    if (m_drag < 0) {
        setCursor(bandAt(e->position()) >= 0 ? Qt::SizeAllCursor : Qt::ArrowCursor);
        return;
    }
    const QPointF p = e->position() - m_grab;
    TrackEq eq = eqOf(m_editor, m_track);
    EqBand& b = eq.bands[size_t(m_drag)];
    eq.on = true;
    b.on = true;
    b.freq = hzAt(p.x());
    if (hasGain(b)) b.gainDb = std::round(dbAt(p.y()) * 10) / 10;
    m_editor->setTrackEq(m_track, eq, QString("eq-drag-%1").arg(m_track));
}

void EqCurve::mouseReleaseEvent(QMouseEvent* e)
{
    if (m_drag >= 0 && e->button() == Qt::LeftButton) {
        m_drag = -1;
        m_editor->project()->closeMerge();
    }
}

void EqCurve::mouseDoubleClickEvent(QMouseEvent* e)
{
    const int band = bandAt(e->position());
    if (band < 0) return;
    TrackEq eq = eqOf(m_editor, m_track);
    eq.bands[size_t(band)].gainDb = 0;
    m_editor->setTrackEq(m_track, eq);
    m_drag = -1;
}

void EqCurve::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), Theme::lane);
    const QRectF r = plot();
    QFont f = font();
    f.setPointSizeF(7.5);
    p.setFont(f);
    // Grid: decades and 1-2-5 steps, gain every 6 dB
    for (double dec = 10; dec <= 10000; dec *= 10)
        for (int m : {1, 2, 5}) {
            const double hz = dec * m;
            if (hz < Equalizer::kMinFreq || hz > Equalizer::kMaxFreq) continue;
            const double x = xOf(hz);
            p.setPen(QPen(m == 1 ? Theme::controlLight : Theme::control, 1));
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
            p.setPen(Theme::textDim);
            const QString label = hz >= 1000 ? QString("%1k").arg(hz / 1000) : QString::number(hz);
            p.drawText(QRectF(x - 20, r.bottom() + 2, 40, 14), Qt::AlignCenter, label);
        }
    for (int db = -24; db <= 24; db += 6) {
        const double y = yOf(db);
        p.setPen(QPen(db == 0 ? Theme::controlLight : Theme::control, 1));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        p.setPen(Theme::textDim);
        p.drawText(QRectF(0, y - 7, r.left() - 4, 14), Qt::AlignRight | Qt::AlignVCenter,
                   db > 0 ? QString("+%1").arg(db) : QString::number(db));
    }
    const TrackEq& eq = eqOf(m_editor, m_track);
    TrackEq shown = eq;
    shown.on = true; // show the curve also while the EQ is off (dimmed)
    QPainterPath path;
    for (int x = int(r.left()); x <= int(r.right()); ++x) {
        const QPointF pt(x, yOf(Equalizer::responseDb(shown, hzAt(x), kRate)));
        if (x == int(r.left())) path.moveTo(pt);
        else path.lineTo(pt);
    }
    const QColor curve = eq.on ? Theme::primary : Theme::controlOff;
    p.setPen(QPen(curve, 2));
    p.drawPath(path);
    for (int i = 0; i < 6; ++i) {
        const EqBand& b = eq.bands[size_t(i)];
        const QPointF pt = pointFor(i);
        p.setPen(QPen(Theme::border, 1));
        p.setBrush(b.on && eq.on ? Theme::secondary : Theme::controlOff);
        p.drawEllipse(pt, 6, 6);
        p.setPen(Theme::readableOn(b.on && eq.on ? Theme::secondary : Theme::controlOff));
        p.drawText(QRectF(pt.x() - 6, pt.y() - 6, 12, 12), Qt::AlignCenter, QString::number(i + 1));
    }
}
