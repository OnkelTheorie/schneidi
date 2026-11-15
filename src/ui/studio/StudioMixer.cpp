// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/studio/StudioMixer.h"

#include "app/DynamicsDialog.h"
#include "app/EqDialog.h"
#include "app/Theme.h"
#include "core/Automation.h"
#include "core/ChannelLayout.h"
#include "core/Dynamics.h"
#include "core/Equalizer.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Loudness.h"
#include "core/Project.h"
#include "core/VolumeDraw.h"
#include "engine/Engine.h"
#include "ui/ElidedLabel.h"

#include <QApplication>
#include <QDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

namespace {

constexpr float kSilent = -200.f;
constexpr double kFloor = -60.0; // lowest value of the meters (dBFS)
constexpr double kFall = 24.0;   // meter decay dB/s
constexpr double kHold = 1.0;    // peak hold, s
constexpr int kThumbH = 28;      // the EQ and compressor pictures in a strip

double meterPos(double db) { return std::clamp((db - kFloor) / -kFloor, 0.0, 1.0); }

QString number(double v, int decimals = 1)
{
    return QString::number(v, 'f', decimals).replace('.', I18n::decimalPoint());
}

QString signedDb(double db)
{
    if (db <= -99) return QStringLiteral("-∞");
    return (db > 0.05 ? "+" : "") + number(db);
}

QToolButton* smallButton(const QString& text, const QColor& on, const QString& tip)
{
    auto* b = new QToolButton;
    b->setText(text);
    b->setCheckable(true);
    b->setFocusPolicy(Qt::NoFocus);
    b->setToolTip(tip);
    b->setMinimumHeight(20); // the width from its text (an explicit minimum width would let the layout cut it)
    b->setStyleSheet(QString("QToolButton { background: %1; color: %2; font-weight: 600; padding: 0 3px; border: none; }"
                             "QToolButton:checked { background: %3; color: %4; }")
                             .arg(Theme::control.name(), Theme::text.name(), on.name(), Theme::readableOn(on).name()) +
                         Theme::disabledToolButtonStyle());
    return b;
}

} // namespace

// Level meter with any number of channels next to a fader: bars on the left, the fader's scale and handle on the
// right. Drag = value (Shift = fine), 0 dB snaps, double click = 0 dB. Red over a bar = overloaded (click resets).
class StudioFader : public QWidget {
public:
    StudioFader()
    {
        setMinimumSize(50, 80);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        setCursor(Qt::SizeVerCursor);
    }

    std::function<void(double)> onChange;
    std::function<void()> onFinish;

    void setChannels(int n)
    {
        n = std::clamp(n, 1, 16);
        if (n == int(m_target.size())) return;
        m_target.assign(size_t(n), kSilent);
        m_disp.assign(size_t(n), kSilent);
        m_hold.assign(size_t(n), kSilent);
        m_age.assign(size_t(n), 0.0);
        m_clip.assign(size_t(n), false);
        update();
    }
    int channels() const { return int(m_target.size()); }
    bool dragging() const { return m_dragging; }
    // A name under every bar (the master in 5.1/7.1: L R C LFE Ls Rs …); the bars get as wide as the longest name.
    // Empty = none (narrow bars, as in stereo).
    void setLabels(const QStringList& labels)
    {
        if (labels == m_labels) return;
        m_labels = labels;
        m_barW = 0;
        for (const QString& l : labels) m_barW = std::max(m_barW, QFontMetrics(labelFont()).horizontalAdvance(l) + 2);
        setMinimumWidth(m_labels.isEmpty() ? 60 : meterWidth() + 6 + 50);
        update();
    }
    const QStringList& labels() const { return m_labels; }
    void setOver(double db) { m_over = db; }
    void setValue(double db)
    {
        if (db == m_db) return;
        m_db = db;
        update();
    }
    void setLevels(const float* db, int n)
    {
        for (int c = 0; c < channels(); ++c) {
            const float v = c < n ? db[c] : kSilent;
            m_target[size_t(c)] = v;
            m_max = std::max(m_max, v);
            if (v >= m_over) m_clip[size_t(c)] = true;
        }
    }
    void silence()
    {
        for (float& t : m_target) t = kSilent;
    }
    float maxPeak() const { return m_max; }
    bool clipped() const { return std::any_of(m_clip.begin(), m_clip.end(), [](bool b) { return b; }); }
    void resetPeak()
    {
        m_max = kSilent;
        std::fill(m_clip.begin(), m_clip.end(), false);
        update();
    }
    void tick(double dt)
    {
        bool changed = false;
        for (size_t c = 0; c < m_target.size(); ++c) {
            const float before = m_disp[c];
            m_disp[c] = std::max(m_target[c], float(m_disp[c] - kFall * dt));
            if (m_disp[c] >= m_hold[c]) {
                m_hold[c] = m_disp[c];
                m_age[c] = 0;
            } else if ((m_age[c] += dt) > kHold) {
                m_hold[c] = m_disp[c];
            }
            changed |= meterPos(before) != meterPos(m_disp[c]);
        }
        if (changed) update();
    }

protected:
    int top() const { return 10; }
    bool labelled() const { return !m_labels.isEmpty(); }
    int labelH() const { return QFontMetrics(labelFont()).height(); }
    int bottom() const { return height() - 6 - (labelled() ? labelH() + 2 : 0); }
    int meterWidth() const
    {
        if (labelled()) return channels() * (m_barW + 1) - 1;
        return std::min(width() / 2, channels() * 6 + 2);
    }
    QFont labelFont() const
    {
        QFont f = font();
        f.setPointSizeF(7);
        return f;
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const int t = top(), b = bottom(), h = b - t;
        const int n = channels();
        const int mw = meterWidth();
        const double bw = std::max(2.0, (mw - (n - 1)) / double(n));
        const auto yOf = [&](double db) { return b - int(std::lround(meterPos(db) * h)); };
        for (int c = 0; c < n; ++c) {
            const QRectF bar(c * (bw + 1), t, bw, h + 1);
            p.fillRect(QRectF(bar.x(), 2, bw, 5), m_clip[size_t(c)] ? Theme::warning : Theme::well);
            p.fillRect(bar, Theme::well);
            const int y = yOf(m_disp[size_t(c)]);
            auto zone = [&](int from, int to, const QColor& col) {
                const int a = std::max(y, to);
                if (from > a) p.fillRect(QRectF(bar.x(), a, bw, from - a), col);
            };
            zone(b + 1, yOf(-12), Theme::meterLow);
            zone(yOf(-12), yOf(-3), Theme::meterMid);
            zone(yOf(-3), t, Theme::meterHigh);
            if (m_hold[size_t(c)] > kFloor) p.fillRect(QRectF(bar.x(), yOf(m_hold[size_t(c)]), bw, 2), Theme::text);
        }
        if (labelled()) {
            p.setFont(labelFont());
            p.setPen(Theme::textDim);
            for (int c = 0; c < n && c < m_labels.size(); ++c)
                p.drawText(QRectF(c * (bw + 1) - 2, b + 3, bw + 4, labelH()), Qt::AlignHCenter | Qt::AlignTop, m_labels[c]);
        }
        // Fader: scale and handle right of the meters
        const int fx = mw + 6, fw = width() - fx;
        const int slotX = fx + fw / 2;
        p.fillRect(QRect(slotX - 1, t, 3, h), Theme::well);
        QFont f = font();
        f.setPointSizeF(7);
        p.setFont(f);
        // Labels only where they do not run into the one above (a low fader skips -30/-40)
        int lastLabel = -1000;
        const int lineH = QFontMetrics(f).height();
        for (double db : {12.0, 6.0, 0.0, -6.0, -12.0, -20.0, -30.0, -40.0, -60.0}) {
            const int y = b - int(std::lround(VolumeDraw::posOf(db) * h));
            p.setPen(db == 0 ? Theme::text : Theme::textFaint);
            p.drawLine(slotX - 6, y, slotX - 3, y);
            if (fw <= 40 || y - lastLabel < lineH) continue;
            p.drawText(QRect(fx, y - lineH / 2, slotX - fx - 8, lineH), Qt::AlignRight | Qt::AlignVCenter,
                       db <= kMinVolumeDb ? QStringLiteral("∞") : QString::number(int(db)));
            lastLabel = y;
        }
        const int y = b - int(std::lround(VolumeDraw::posOf(m_db) * h));
        const QRect handle(slotX - 11, y - 6, 22, 12);
        p.setPen(QPen(Theme::border, 1));
        p.setBrush(Theme::controlLight);
        p.drawRoundedRect(handle, 2, 2);
        p.fillRect(QRect(handle.left() + 3, y, handle.width() - 6, 1), Theme::primary);
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        if (e->position().y() < top() && e->position().x() < meterWidth()) { // the overload marks
            resetPeak();
            return;
        }
        m_dragging = true;
        m_lastY = e->position().y();
        if (std::abs(e->position().y() - posY()) > 8) jumpTo(e->position().y()); // a click beside the handle: there
        m_dragPos = VolumeDraw::posOf(m_db);
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!m_dragging) return;
        if (!(e->buttons() & Qt::LeftButton)) return endDrag(); // the release went elsewhere (a page change)
        const double dy = e->position().y() - m_lastY;
        m_lastY = e->position().y();
        const double h = std::max(1, bottom() - top());
        // The drag position adds up every move unrounded: a slow drag (1 px per move, less than the 0.1 dB rounding
        // or the 0 dB snap) still gets somewhere instead of falling back to the shown value each time
        m_dragPos = std::clamp(m_dragPos - dy / h * ((e->modifiers() & Qt::ShiftModifier) ? 0.2 : 1.0), 0.0, 1.0);
        double db = VolumeDraw::dbAt(m_dragPos);
        if (std::abs(db) < 0.4) db = 0; // 0 dB snaps
        change(db);
    }
    void mouseReleaseEvent(QMouseEvent*) override { endDrag(); }
    void hideEvent(QHideEvent* e) override
    {
        QWidget::hideEvent(e);
        endDrag(); // hidden mid-drag (another page): the drag ends as on a release, its undo step closed
    }
    void mouseDoubleClickEvent(QMouseEvent*) override
    {
        change(0);
        if (onFinish) onFinish();
    }

private:
    void endDrag()
    {
        if (!m_dragging) return;
        m_dragging = false;
        if (onFinish) onFinish();
    }
    double posY() const { return bottom() - VolumeDraw::posOf(m_db) * (bottom() - top()); }
    void jumpTo(double y) { change(VolumeDraw::dbAt(double(bottom() - y) / std::max(1, bottom() - top()))); }
    void change(double db)
    {
        db = std::round(std::clamp(db, kMinVolumeDb, kMaxVolumeDb) * 10) / 10;
        if (db == m_db) return;
        setValue(db);
        if (onChange) onChange(db);
    }

    std::vector<float> m_target, m_disp, m_hold;
    std::vector<double> m_age;
    std::vector<bool> m_clip;
    QStringList m_labels;
    int m_barW = 0; // with labels: width of a bar
    float m_max = kSilent;
    double m_over = -0.001;
    double m_db = 0;
    bool m_dragging = false;
    double m_lastY = 0;
    double m_dragPos = 0; // while dragging: the handle's position (0..1) before rounding and snapping
};

// Pan as a horizontal bar: drag sideways, double click = centre
class PanBar : public QWidget {
public:
    PanBar()
    {
        setFixedHeight(16);
        setMinimumWidth(40);
        setCursor(Qt::SizeHorCursor);
        setToolTip(T("Panorama: ziehen = links/rechts, Doppelklick = Mitte"));
    }
    std::function<void(double)> onChange;
    std::function<void()> onFinish;
    bool dragging() const { return m_drag; }
    void setValue(double v)
    {
        if (v == m_v) return;
        m_v = v;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const QRect r = rect().adjusted(0, 3, -1, -3);
        p.fillRect(r, Theme::well);
        const int mid = r.center().x();
        const int x = mid + int(std::lround(m_v / 100.0 * (r.width() / 2.0)));
        p.fillRect(QRect(std::min(mid, x), r.top(), std::abs(x - mid) + 1, r.height()), Theme::alpha(Theme::primary, 160));
        p.fillRect(QRect(mid, r.top() - 2, 1, r.height() + 4), Theme::textFaint);
        p.fillRect(QRect(x - 1, r.top() - 3, 3, r.height() + 6), Theme::text);
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        m_drag = e->button() == Qt::LeftButton;
        m_lastX = e->position().x();
        m_dragV = m_v;
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!m_drag) return;
        if (!(e->buttons() & Qt::LeftButton)) return endDrag(); // the release went elsewhere (a page change)
        const double dx = e->position().x() - m_lastX;
        m_lastX = e->position().x();
        // Added up unrounded: small moves (Shift) get past the centre snap and the rounding
        m_dragV = std::clamp(m_dragV + dx / std::max(1, width() / 2) * 100 * ((e->modifiers() & Qt::ShiftModifier) ? 0.2 : 1.0),
                             -100.0, 100.0);
        set(m_dragV);
    }
    void mouseReleaseEvent(QMouseEvent*) override { endDrag(); }
    void hideEvent(QHideEvent* e) override
    {
        QWidget::hideEvent(e);
        endDrag(); // hidden mid-drag (another page): the drag ends as on a release
    }
    void mouseDoubleClickEvent(QMouseEvent*) override
    {
        set(0);
        if (onFinish) onFinish();
    }

private:
    void endDrag()
    {
        if (!m_drag) return;
        m_drag = false;
        if (onFinish) onFinish();
    }
    void set(double v)
    {
        v = std::clamp(v, -100.0, 100.0);
        if (std::abs(v) < 2) v = 0;
        if (v == m_v) return;
        m_v = v;
        update();
        if (onChange) onChange(std::round(v));
    }
    double m_v = 0;
    bool m_drag = false;
    double m_lastX = 0;
    double m_dragV = 0; // while dragging: the value before rounding and snapping
};

// One surround send of a strip (5.1/7.1 projects): a bar filled from the left with its name and value in it; drag
// sideways, double click = its default
class SendBar : public QWidget {
public:
    SendBar(const QString& name, double lo, double hi, double def, const QString& unit)
        : m_name(name), m_lo(lo), m_hi(hi), m_def(def), m_v(def), m_unit(unit)
    {
        setFixedHeight(16);
        setMinimumWidth(40);
        setCursor(Qt::SizeHorCursor);
    }
    std::function<void(double)> onChange;
    std::function<void()> onFinish;
    void setValue(double v)
    {
        if (v == m_v) return;
        m_v = v;
        update();
    }
    double value() const { return m_v; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const QRect r = rect().adjusted(0, 1, -1, -1);
        p.fillRect(r, Theme::well);
        const double t = (m_v - m_lo) / (m_hi - m_lo);
        p.fillRect(QRect(r.left(), r.top(), int(std::lround(t * r.width())), r.height()), Theme::alpha(Theme::primary, 140));
        QFont f = font();
        f.setPointSizeF(7);
        p.setFont(f);
        p.setPen(Theme::text);
        const QString value = m_v <= m_lo && m_unit == "dB" ? QStringLiteral("-∞") : number(m_v, 0);
        p.drawText(r.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, m_name);
        p.drawText(r.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::AlignRight, value + (m_unit.isEmpty() ? "" : " " + m_unit));
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        m_drag = e->button() == Qt::LeftButton;
        m_lastX = e->position().x();
        m_dragV = m_v;
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!m_drag) return;
        if (!(e->buttons() & Qt::LeftButton)) return endDrag(); // the release went elsewhere (a page change)
        const double dx = e->position().x() - m_lastX;
        m_lastX = e->position().x();
        // Added up unrounded: the model sends the rounded value back after every move (setValue), small moves would
        // be lost in it
        m_dragV = std::clamp(m_dragV + dx / std::max(1, width()) * (m_hi - m_lo) * ((e->modifiers() & Qt::ShiftModifier) ? 0.2 : 1.0),
                             m_lo, m_hi);
        set(m_dragV);
    }
    void mouseReleaseEvent(QMouseEvent*) override { endDrag(); }
    void hideEvent(QHideEvent* e) override
    {
        QWidget::hideEvent(e);
        endDrag(); // hidden mid-drag (another page): the drag ends as on a release
    }
    void mouseDoubleClickEvent(QMouseEvent*) override
    {
        set(m_def);
        if (onFinish) onFinish();
    }

private:
    void endDrag()
    {
        if (!m_drag) return;
        m_drag = false;
        if (onFinish) onFinish();
    }
    void set(double v)
    {
        v = std::clamp(v, m_lo, m_hi);
        if (v == m_v) return;
        m_v = v;
        update();
        if (onChange) onChange(std::round(v));
    }
    QString m_name;
    double m_lo, m_hi, m_def, m_v;
    QString m_unit;
    bool m_drag = false;
    double m_lastX = 0;
    double m_dragV = 0; // while dragging: the value before rounding
};
// A small picture of a track's EQ or compressor curve in its strip. A click opens the full window (EqDialog with the
// big EQ curve, DynamicsDialog with the big compressor curve - both draggable there); a click on the "EQ" / "DYN" tag
// in its corner switches it on or off. Dimmed while off.
class CurveThumb : public QWidget {
public:
    CurveThumb(Editor* editor, int track, bool eq) : m_editor(editor), m_track(track), m_eq(eq)
    {
        setFixedHeight(kThumbH);
        setMinimumWidth(40);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setCursor(Qt::PointingHandCursor);
        setToolTip(eq ? T("EQ der Spur: Klick öffnet das EQ-Fenster mit der großen Kurve, Klick auf „EQ“ schaltet ihn "
                          "an oder aus")
                      : T("Kompressor der Spur: Klick öffnet das Kompressor-Fenster mit der großen Kurve, Klick auf "
                          "„DYN“ schaltet ihn an oder aus"));
    }
    std::function<void()> onOpen;
    std::function<void()> onToggle;
    // The model changed: drawn anew only when this track's EQ / compressor did (every timeline change lands here)
    void refresh()
    {
        const auto& audio = m_editor->project()->timeline().audio;
        if (m_track >= audio.size()) {
            m_shownEq.reset();
            m_shownDyn.reset();
            return update();
        }
        const Track& t = audio[m_track];
        if (m_eq) {
            if (m_shownEq && *m_shownEq == t.eq) return;
            m_shownEq = t.eq;
        } else {
            if (m_shownDyn && *m_shownDyn == t.dynamics) return;
            m_shownDyn = t.dynamics;
        }
        update();
    }
    bool isOn() const
    {
        const auto& audio = m_editor->project()->timeline().audio;
        if (m_track >= audio.size()) return false;
        return m_eq ? audio[m_track].eq.on : audio[m_track].dynamics.on;
    }
    // The on/off tag in the top left corner
    QRect tagRect() const
    {
        const QFontMetrics fm(tagFont());
        return QRect(1, 1, fm.horizontalAdvance(tag()) + 6, fm.height());
    }

protected:
    QString tag() const { return m_eq ? QStringLiteral("EQ") : QStringLiteral("DYN"); }
    QFont tagFont() const
    {
        QFont f = font();
        f.setPointSizeF(7);
        f.setBold(true);
        return f;
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), Theme::lane);
        const auto& audio = m_editor->project()->timeline().audio;
        if (m_track >= audio.size()) return;
        const Track& t = audio[m_track];
        const bool on = isOn();
        const QRectF r = QRectF(rect()).adjusted(2, 2, -2, -2);
        QPainterPath path;
        if (m_eq) {
            // Log frequency across, +-24 dB up (the EQ window's scale)
            TrackEq shown = t.eq;
            shown.on = true; // the curve also while it is off (dimmed)
            const double lo = std::log(Equalizer::kMinFreq), hi = std::log(Equalizer::kMaxFreq);
            p.setPen(QPen(Theme::control, 1));
            p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
            for (int x = int(r.left()); x <= int(r.right()); ++x) {
                const double hz = std::exp(lo + (x - r.left()) / r.width() * (hi - lo));
                const double db = std::clamp(Equalizer::responseDb(shown, hz, 48000), -24.0, 24.0);
                const QPointF pt(x, r.center().y() - db / 24 * r.height() / 2);
                if (x == int(r.left())) path.moveTo(pt);
                else path.lineTo(pt);
            }
        } else {
            // Input -60..0 dB across, output -60..+12 up (the compressor window's scale)
            const TrackDynamics& d = t.dynamics;
            const auto at = [&](double in, double out) {
                return QPointF(r.left() + (in + 60) / 60 * r.width(), r.bottom() - (std::clamp(out, -60.0, 12.0) + 60) / 72 * r.height());
            };
            p.setPen(QPen(Theme::control, 1, Qt::DashLine));
            p.drawLine(at(-60, -60), at(0, 0));
            for (int x = int(r.left()); x <= int(r.right()); ++x) {
                const double in = (x - r.left()) / r.width() * 60 - 60;
                const QPointF pt = at(in, Compressor::curveDb(d, in));
                if (x == int(r.left())) path.moveTo(pt);
                else path.lineTo(pt);
            }
        }
        p.setPen(QPen(on ? Theme::primary : Theme::controlOff, 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        const QRect tr = tagRect();
        p.setRenderHint(QPainter::Antialiasing, false);
        p.fillRect(tr, on ? Theme::primary : Theme::control);
        p.setFont(tagFont());
        p.setPen(on ? Theme::readableOn(Theme::primary) : Theme::textDim);
        p.drawText(tr, Qt::AlignCenter, tag());
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        if (tagRect().adjusted(-2, -2, 2, 2).contains(e->position().toPoint())) {
            if (onToggle) onToggle();
        } else if (onOpen) {
            onOpen();
        }
    }

private:
    Editor* m_editor;
    int m_track;
    bool m_eq;
    std::optional<TrackEq> m_shownEq; // what the last refresh() asked to be drawn
    std::optional<TrackDynamics> m_shownDyn;
};

// One narrow mixer strip (a fixed width; many tracks scroll beside each other). From the top: name, EQ and compressor
// pictures (a click opens their window), pan (plus the surround sends in 5.1/7.1), mute/solo with the pan value, the
// peak since the reset, fader with meters, the fader's value. The master: name, limiter, peak, fader, value.
class StudioStrip : public QWidget {
public:
    StudioStrip(Editor* editor, int track) // track -1 = the master
    {
        const bool master = track < 0;
        m_master = master;
        setObjectName("StudioStrip");
        setAttribute(Qt::WA_StyledBackground);
        setFixedWidth(baseWidth());
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        setStyleSheet(QString("QWidget#StudioStrip { background: %1; border-right: 1px solid %2; %4 }"
                              "QLabel { color: %3; }")
                          .arg(Theme::panel.name(), Theme::border.name(), Theme::textDim.name(),
                               master ? QString("border-left: 2px solid %1;").arg(Theme::border.name()) : QString()));
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(3, 3, 3, 3);
        lay->setSpacing(2);
        name = new ElidedLabel;
        name->setAlignment(Qt::AlignHCenter);
        name->setStyleSheet(QString("color: %1; font-weight: 600; background: %2; padding: 1px;")
                                .arg(Theme::text.name(), Theme::panelHeader.name()));
        lay->addWidget(name);
        const QString tag = master ? QStringLiteral("master") : trackShortName({TrackKind::Audio, track});
        if (!master) {
            eqThumb = new CurveThumb(editor, track, true);
            eqThumb->setObjectName("studio_eqthumb_" + tag);
            lay->addWidget(eqThumb);
            dynThumb = new CurveThumb(editor, track, false);
            dynThumb->setObjectName("studio_dynthumb_" + tag);
            lay->addWidget(dynThumb);
            // Too little height for the pictures (a small window, a large font): two small buttons instead
            fxRow = new QWidget;
            auto* fx = new QHBoxLayout(fxRow);
            fx->setContentsMargins(0, 0, 0, 0);
            fx->setSpacing(3);
            eqOpen = smallButton(QStringLiteral("EQ"), Theme::primary, T("EQ-Fenster öffnen"));
            eqOpen->setCheckable(false);
            eqOpen->setObjectName("studio_eq_" + tag);
            dynOpen = smallButton(QStringLiteral("DYN"), Theme::primary, T("Kompressor-Fenster öffnen"));
            dynOpen->setCheckable(false);
            dynOpen->setObjectName("studio_dyn_" + tag);
            fx->addWidget(eqOpen, 1);
            fx->addWidget(dynOpen, 1);
            fxRow->hide();
            lay->addWidget(fxRow);
            pan = new PanBar;
            pan->setObjectName("studio_pan_" + tag);
            lay->addWidget(pan);
            // Surround panner (5.1/7.1 projects only): front -> surround, share in the centre, send to the LFE
            surround = new QWidget;
            surround->setObjectName("studio_surround_" + tag);
            auto* sl = new QVBoxLayout(surround);
            sl->setContentsMargins(0, 0, 0, 0);
            sl->setSpacing(2);
            depth = new SendBar(T("Tiefe"), 0, 100, 0, {});
            depth->setToolTip(T("Surround-Tiefe: 0 = vorn, 100 = hinten (7.1: 50 = Seite)"));
            center = new SendBar(T("Mitte"), 0, 100, 0, {});
            center->setToolTip(T("Anteil im Center-Lautsprecher"));
            lfe = new SendBar(QStringLiteral("LFE"), kMinVolumeDb, 0, kMinVolumeDb, QStringLiteral("dB"));
            lfe->setToolTip(T("Anteil im Subwoofer (LFE, unter 120 Hz)"));
            for (SendBar* b : {depth, center, lfe}) sl->addWidget(b);
            surround->hide();
            lay->addWidget(surround);
            // Mute, solo and the pan value in one row
            auto* ms = new QHBoxLayout;
            ms->setSpacing(3);
            mute = smallButton(QStringLiteral("M"), Theme::warning, T("Stumm"));
            solo = smallButton(QStringLiteral("S"), Theme::meterMid, T("Solo: nur Spuren mit „S“ sind zu hören"));
            ms->addWidget(mute);
            ms->addWidget(solo);
            panLabel = new QLabel(QStringLiteral("C"));
            panLabel->setAlignment(Qt::AlignCenter);
            panLabel->setFont(smallFont());
            ms->addWidget(panLabel, 1);
            lay->addLayout(ms);
        } else {
            limiter = smallButton(T("Limiter"), Theme::primary, {});
            limiter->setObjectName("studio_limiter");
            lay->addWidget(limiter);
        }
        peak = new QLabel;
        peak->setAlignment(Qt::AlignCenter);
        peak->setFont(smallFont());
        peak->setToolTip(master ? T("True Peak (dBTP) seit dem Zurücksetzen – Klick auf die roten Marken setzt zurück")
                                : T("Spitzenpegel (dBFS) seit dem Zurücksetzen"));
        lay->addWidget(peak);
        fader = new StudioFader;
        fader->setObjectName("studio_fader_" + tag);
        lay->addWidget(fader, 1);
        value = new QLabel;
        value->setAlignment(Qt::AlignCenter);
        value->setStyleSheet(QString("color: %1;").arg(Theme::text.name()));
        lay->addWidget(value);
        setVolume(0);
        updatePeak();
        fitWidth();
    }

    int baseWidth() const { return StudioMixer::kStripW + (m_master ? 14 : 0); }
    static QFont smallFont()
    {
        QFont f = QApplication::font();
        f.setPointSizeF(f.pointSizeF() * 0.85);
        return f;
    }

    // The usual narrow width; wider only when a row needs it (a large font, the master's speaker names); again once
    // polished (the style sheet's fonts and paddings count)
    void fitWidth()
    {
        ensurePolished();
        for (QWidget* c : findChildren<QWidget*>()) c->ensurePolished();
        layout()->invalidate();
        setFixedWidth(std::max(baseWidth(), layout()->minimumSize().width()));
    }

    void setVolume(double db)
    {
        shown = db;
        fader->setValue(db);
        const QString text = formatVolumeDb(db);
        if (value->text() != text) value->setText(text);
    }
    // Track automation: the fader shows the level at the playhead (fader + curve) and moving it trims the whole curve
    void setAutomated(bool on)
    {
        if (on == automated) return;
        automated = on;
        value->setStyleSheet(QString("color: %1;").arg((on ? Theme::secondary : Theme::text).name()));
        const QString tip = on ? T("Automation: zeigt den Pegel am Abspielkopf; der Fader verschiebt die ganze Kurve") : QString();
        value->setToolTip(tip);
        fader->setToolTip(tip);
    }
    double shown = 0;       // what the fader shows (with automation: the level at the playhead)
    bool automated = false;
    // Pan automation: the pan bar shows the pan at the playhead (knob + curve), moving it trims the whole curve
    void setPanAutomated(bool on)
    {
        if (on == panAutomated || !panLabel) return;
        panAutomated = on;
        panLabel->setStyleSheet(on ? QString("color: %1;").arg(Theme::secondary.name()) : QString());
        panLabel->setToolTip(on ? T("Pan-Automation: zeigt den Pan am Abspielkopf; der Regler verschiebt die ganze Kurve") : QString());
    }
    double shownPan = 0;
    bool panAutomated = false;
    void setPan(double v)
    {
        if (!pan) return;
        shownPan = v;
        pan->setValue(v);
        const int i = int(std::lround(v));
        panLabel->setText(i == 0 ? QStringLiteral("C") : QString("%1%2").arg(i < 0 ? QStringLiteral("L") : QStringLiteral("R")).arg(std::abs(i)));
    }
    // The curve pictures need room: a low mixer shows two small buttons instead and leaves out the peak (the fader's
    // overload marks stay); the fader keeps its height
    bool curvesShown() const { return eqThumb && !eqThumb->isHidden(); }
    void setCurvesShown(bool on)
    {
        if (!eqThumb || curvesShown() == on) return;
        eqThumb->setVisible(on);
        dynThumb->setVisible(on);
        peak->setVisible(on);
        fxRow->setVisible(!on);
    }
    // Height the strip needs with the small buttons (its least) and with the pictures
    int compactHeight() const { return baseHeight() + fxRow->sizeHint().height() + layout()->spacing(); }
    int heightWithCurves() const
    {
        if (!eqThumb) return layout()->minimumSize().height();
        return baseHeight() + 2 * kThumbH + peak->sizeHint().height() + 3 * layout()->spacing();
    }

    void updatePeak()
    {
        const QString text = signedDb(fader->maxPeak());
        const bool clip = fader->clipped();
        if (text == peak->text() && clip == m_clip && !peak->styleSheet().isEmpty()) return;
        m_clip = clip;
        peak->setText(text);
        peak->setStyleSheet(clip ? QString("color: %1; background: %2; font-weight: 600;")
                                       .arg(Theme::readableOn(Theme::warning).name(), Theme::warning.name())
                                 : QString("color: %1; background: %2;").arg(Theme::textDim.name(), Theme::well.name()));
    }

    ElidedLabel* name;
    CurveThumb* eqThumb = nullptr;
    CurveThumb* dynThumb = nullptr;
    QWidget* fxRow = nullptr;
    QToolButton* eqOpen = nullptr;
    QToolButton* dynOpen = nullptr;
    PanBar* pan = nullptr;
    QWidget* surround = nullptr; // depth, center, LFE: only in a 5.1/7.1 project
    SendBar* depth = nullptr;
    SendBar* center = nullptr;
    SendBar* lfe = nullptr;
    QLabel* panLabel = nullptr;
    QToolButton* mute = nullptr;
    QToolButton* solo = nullptr;
    QToolButton* limiter = nullptr;
    QLabel* peak;
    StudioFader* fader;
    QLabel* value;

private:
    // The strip's least height without the parts that come and go with the room (pictures + peak or the buttons)
    int baseHeight() const
    {
        const int h = layout()->minimumSize().height(), sp = layout()->spacing();
        if (!eqThumb) return h;
        return curvesShown() ? h - 2 * kThumbH - peak->sizeHint().height() - 3 * sp : h - fxRow->sizeHint().height() - sp;
    }
    bool m_clip = false;
    bool m_master = false;
};

// Loudness at the master, live (ITU-R BS.1770), compact above the mixer: integrated large (green within ±1 LU of the
// target - the mixer's setting, default -14 LUFS -, yellow below, red above) with the target under it; short-term,
// momentary, range and true peak (red over the true-peak limit) in a 2 x 2 grid beside it; ↺ restarts the measurement.
class StudioLoudness : public QWidget {
public:
    StudioLoudness()
    {
        setObjectName("studio_loudness");
        setProperty("tourTarget", "studio_loudness");
        setAttribute(Qt::WA_StyledBackground);
        setStyleSheet(QString("QWidget#studio_loudness { background: %1; border-bottom: 1px solid %2; }")
                          .arg(Theme::panel.name(), Theme::border.name()));
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        auto* grid = new QGridLayout(this);
        grid->setContentsMargins(8, 2, 8, 2);
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(0);
        const struct { const char* shortName; const char* tip; } rows[] = {
            {"I", N_("Integriert (LUFS): Lautheit seit dem Zurücksetzen")},
            {"S", N_("Kurzzeit (LUFS, 3 s)")},
            {"M", N_("Momentan (LUFS, 400 ms)")},
            {"LRA", N_("Lautheitsumfang (LU)")},
            {"TP", N_("True Peak (dBTP), höchster Wert seit dem Zurücksetzen")},
        };
        // Where each reading goes: I big on the left (two rows), the others two by two
        const QPoint cells[] = {{0, 0}, {2, 0}, {4, 0}, {2, 1}, {4, 1}}; // (column, row); 1 and 3 keep a gap
        for (int i = 0; i < 5; ++i) {
            auto* label = new QLabel(QString::fromLatin1(rows[i].shortName)); // EBU R128 names, as on every meter
            label->setToolTip(T(rows[i].tip));
            auto* v = new QLabel(QStringLiteral("–"));
            v->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            v->setToolTip(T(rows[i].tip));
            if (i == 0) {
                QFont f = v->font();
                f.setPointSizeF(f.pointSizeF() * 1.6);
                f.setBold(true);
                v->setFont(f);
                v->setMinimumWidth(QFontMetrics(f).horizontalAdvance(QStringLiteral("−88,8")));
                auto* iRow = new QHBoxLayout;
                iRow->setSpacing(6);
                iRow->addWidget(label);
                iRow->addWidget(v);
                iRow->addStretch(1);
                grid->addLayout(iRow, 0, 0);
            } else {
                v->setMinimumWidth(v->fontMetrics().horizontalAdvance(QStringLiteral("−88,8")));
                grid->addWidget(label, cells[i].y(), cells[i].x());
                grid->addWidget(v, cells[i].y(), cells[i].x() + 1);
            }
            values << v;
        }
        values[0]->setObjectName("studio_lufs");
        values[4]->setObjectName("studio_truepeak");
        auto* targetRow = new QHBoxLayout;
        targetRow->setSpacing(4);
        target = new QLabel;
        target->setFont(StudioStrip::smallFont());
        targetRow->addWidget(target, 1);
        reset = new QToolButton;
        reset->setObjectName("studio_loudness_reset");
        reset->setText(QStringLiteral("↺"));
        reset->setFocusPolicy(Qt::NoFocus);
        reset->setAutoRaise(true);
        reset->setToolTip(T("Messung neu beginnen"));
        targetRow->addWidget(reset);
        grid->addLayout(targetRow, 1, 0);
        grid->setColumnMinimumWidth(1, 10); // a gap after the integrated value
        grid->setColumnStretch(6, 1);
    }

    void setReading(const LoudnessReading& r, bool live, double targetLufs, double tpLimit)
    {
        const auto lufs = [](double v) { return v <= LoudnessMeter::kSilence + 1 ? QStringLiteral("–") : number(v); };
        const double I = r.integrated;
        set(0, lufs(I), I <= LoudnessMeter::kSilence + 1 ? Theme::textDim
                        : std::abs(I - targetLufs) <= 1 ? Theme::meterLow
                        : I < targetLufs              ? Theme::meterMid
                                                      : Theme::warning);
        set(1, live ? lufs(r.shortTerm) : QStringLiteral("–"), Theme::text);
        set(2, live ? lufs(r.momentary) : QStringLiteral("–"), Theme::text);
        set(3, r.seconds > 0 ? number(r.range) : QStringLiteral("–"), Theme::text);
        set(4, r.truePeak <= -99 ? QStringLiteral("–") : number(r.truePeak), r.truePeak > tpLimit ? Theme::warning : Theme::text);
        const QString t = T("Ziel %1 LUFS").arg(number(targetLufs, 0));
        if (target->text() != t) target->setText(t);
    }

    QVector<QLabel*> values;
    QLabel* target;
    QToolButton* reset;

private:
    void set(int i, const QString& text, const QColor& color)
    {
        if (values[i]->text() == text && values[i]->property("color").value<QColor>() == color) return;
        values[i]->setText(text);
        values[i]->setProperty("color", color);
        values[i]->setStyleSheet(QString("color: %1;").arg(color.name()));
    }
};

// ---------- StudioMixer ----------

void StudioMixer::refreshLoudnessSettings()
{
    // Read here, not in tick() (every 30 ms): QSettings costs
    m_loudnessTarget = QSettings().value("audio/loudnessTarget", -14.0).toDouble();
    m_truePeakLimit = QSettings().value("audio/truePeakLimit", -1.0).toDouble();
    if (m_master) m_master->fader->setOver(m_truePeakLimit);
    if (m_loudness) m_loudness->setReading(m_engine->loudness(), false, m_loudnessTarget, m_truePeakLimit);
}

StudioMixer::StudioMixer(Editor* editor, Engine* engine, QWidget* parent)
    : QWidget(parent), m_editor(editor), m_project(editor->project()), m_engine(engine), m_writer(editor)
{
    setObjectName("studio_mixer");
    setProperty("tourTarget", "studio_mixer");
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    auto* strips = new QWidget;
    strips->setObjectName("studio_strips");
    m_stripLayout = new QHBoxLayout(strips);
    m_stripLayout->setContentsMargins(0, 0, 0, 0);
    m_stripLayout->setSpacing(0);
    m_stripLayout->addStretch(1);
    // Many tracks: the strips scroll sideways inside the column, the master stays at its right end
    m_scroll = new QScrollArea;
    m_scroll->setObjectName("studio_strips_scroll");
    m_scroll->setWidget(strips);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scroll->horizontalScrollBar()->setSingleStep(kStripW);
    m_scroll->setMinimumWidth(kStripW);
    m_scroll->viewport()->installEventFilter(this);
    row->addWidget(m_scroll, 1);

    m_master = new StudioStrip(editor, -1);
    m_master->name->setFullText(T("Master"));
    m_master->fader->setChannels(m_channels);
    m_master->fader->onChange = [this](double db) {
        m_master->setVolume(db);
        m_editor->setTrackVolume(-1, db, "studio-master");
    };
    m_master->fader->onFinish = [this] { m_project->closeMerge(); };
    connect(m_master->limiter, &QToolButton::clicked, this, [this](bool on) { m_editor->setMasterLimiter(on); });
    row->addWidget(m_master);
    // The loudness reading: StudioPage puts it above the mixer (the tours point at it on its own)
    m_loudness = new StudioLoudness;
    connect(m_loudness->reset, &QToolButton::clicked, this, [this] {
        m_engine->resetLoudness();
        m_master->fader->resetPeak();
        m_loudness->setReading(m_engine->loudness(), false, m_loudnessTarget, m_truePeakLimit);
    });

    connect(m_project, &Project::timelineChanged, this, &StudioMixer::sync);
    connect(m_project, &Project::formatChanged, this, [this] {
        sync();
        fitHeight(); // the surround sends take room
    });
    connect(m_engine, &Engine::audioSpeakerLevels, this, &StudioMixer::onLevels);
    // A seek or a loop wrap while a control is held: the touches end where they were played (the stretch jumped over
    // keeps its curve); the next move starts a new touch
    connect(m_engine, &Engine::seeked, this, [this] {
        if (m_writer.touching()) m_writer.jumped();
    });
    m_timer = new QTimer(this);
    m_timer->setInterval(30);
    connect(m_timer, &QTimer::timeout, this, &StudioMixer::tick);
    m_clock.start(); // the timer runs while the mixer shows (showEvent)
    sync();
    refreshLoudnessSettings();
}

QWidget* StudioMixer::loudness() const { return m_loudness; }

QScrollArea* StudioMixer::stripsArea() const { return m_scroll; }

QWidget* StudioMixer::masterStrip() const { return m_master; }

int StudioMixer::preferredWidth(int strips) const
{
    int w = 0;
    for (int i = 0; i < std::min<int>(strips, int(m_strips.size())); ++i) w += m_strips[i]->width();
    if (m_strips.isEmpty()) w = kStripW;
    return w + m_master->width();
}

bool StudioMixer::eventFilter(QObject* watched, QEvent* e)
{
    // The strips' height follows the viewport's (the horizontal scrollbar takes some when it shows)
    if (watched == m_scroll->viewport() && e->type() == QEvent::Resize) fitHeight();
    return QWidget::eventFilter(watched, e);
}

QStringList StudioMixer::meterLabels(int track) const
{
    if (track < 0) return m_master->fader->labels();
    return track < m_strips.size() ? m_strips[track]->fader->labels() : QStringList{};
}

int StudioMixer::meterChannels(int track) const
{
    if (track < 0) return m_master->fader->channels();
    return track < m_strips.size() ? m_strips[track]->fader->channels() : 0;
}

void StudioMixer::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    if (m_syncPending) sync();
    refreshLoudnessSettings(); // the target / true-peak limit may have changed on another page (the Edit mixer)
    fitHeight();
    m_clock.restart(); // the meters fall from now, not by the time it was hidden
    m_timer->start();
}

void StudioMixer::hideEvent(QHideEvent* e)
{
    QWidget::hideEvent(e);
    m_timer->stop(); // another page: no meters to move
    // Without the timer tick() cannot see playback stop: a write pass ends here (open touches released, one undo step)
    if (m_writer.inPass()) m_writer.endPass(timelinePosition());
}

void StudioMixer::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    fitHeight();
}

// Playing the timeline: an effect sound or a file previewed in the viewer (Source mode) plays on its own time axis -
// its positions are frames of that file, nothing to write into the timeline's automation
bool StudioMixer::playing() const { return m_engine->timelinePositions() && !std::isnan(m_engine->soundFrame()); }

int StudioMixer::timelinePosition() const
{
    // In Source mode the engine's position is a frame of the previewed file: the timeline's playhead (MainWindow
    // tells it; also when the page first shows while a file is previewed), else the last one seen here
    if (m_engine->timelinePositions()) m_lastTimelinePos = m_engine->position();
    else if (m_timelinePlayhead) m_lastTimelinePos = m_timelinePlayhead();
    return m_lastTimelinePos;
}

void StudioMixer::setWriteMode(bool on)
{
    if (on == m_writeMode) return;
    m_writeMode = on;
    if (!on) m_writer.endPass(timelinePosition());
}

void StudioMixer::fitHeight()
{
    if (m_fitting) return; // showing/hiding the pictures resizes the viewport again
    m_fitting = true;
    const int room = m_scroll->viewport()->height();
    int least = 0;
    for (StudioStrip* s : m_strips) {
        s->fitWidth();
        s->setCurvesShown(room >= s->heightWithCurves());
        least = std::max(least, s->compactHeight());
    }
    m_master->fitWidth();
    // The strips never get cut at the bottom: the column keeps at least their height with the small buttons (the
    // viewer above gives way), plus the scrollbar when they scroll
    const QScrollBar* bar = m_scroll->horizontalScrollBar();
    const int need = least + (bar->isVisible() || m_scroll->widget()->minimumSizeHint().width() > m_scroll->width()
                                  ? bar->sizeHint().height() : 0);
    if (m_scroll->minimumHeight() != need) m_scroll->setMinimumHeight(need);
    m_fitting = false;
}

int StudioMixer::curvesShown() const
{
    int n = 0;
    for (StudioStrip* s : m_strips) n += s->curvesShown() ? 1 : 0;
    return n;
}

void StudioMixer::rebuild(int count)
{
    qDeleteAll(m_strips);
    m_strips.clear();
    for (int i = 0; i < count; ++i) {
        auto* s = new StudioStrip(m_editor, i);
        s->fader->setChannels(m_channels);
        s->fader->onChange = [this, i, s](double db) {
            // Write mode while playing: the move is recorded into the automation (AutomationWriter)
            if (m_writeMode && playing()) {
                s->setVolume(db);
                m_writer.move(i, AutoTarget::Volume, timelinePosition(), db);
                return;
            }
            const Track* t = i < m_project->timeline().audio.size() ? &m_project->timeline().audio[i] : nullptr;
            // With automation the fader shows the level at the playhead: a move trims the whole curve by as much
            const double fader = t && !t->volumeAuto.isEmpty() ? t->volumeDb + (db - s->shown) : db;
            s->setVolume(db);
            m_editor->setTrackVolume(i, fader, QString("studio-vol-%1").arg(i));
        };
        s->pan->onChange = [this, i, s](double v) {
            if (m_writeMode && playing()) {
                s->setPan(v);
                m_writer.move(i, AutoTarget::Pan, timelinePosition(), v);
                return;
            }
            const Track* t = i < m_project->timeline().audio.size() ? &m_project->timeline().audio[i] : nullptr;
            // With pan automation the bar shows the pan at the playhead: a move trims the whole curve by as much
            const double knob = t && !t->panAuto.isEmpty() ? t->pan + (v - s->shownPan) : v;
            s->setPan(v);
            m_editor->setTrackPan(i, knob, QString("studio-pan-%1").arg(i));
        };
        // A touch ends on release (the pass stays one undo step until playback stops); otherwise one step per drag
        s->fader->onFinish = [this, i] {
            if (m_writer.touching(i, AutoTarget::Volume)) m_writer.release(i, AutoTarget::Volume, timelinePosition());
            else if (!m_writer.inPass()) m_project->closeMerge();
        };
        s->pan->onFinish = [this, i] {
            if (m_writer.touching(i, AutoTarget::Pan)) m_writer.release(i, AutoTarget::Pan, timelinePosition());
            else if (!m_writer.inPass()) m_project->closeMerge();
        };
        // Surround: the three sends go together (Editor::setTrackSurround), one undo step per drag
        const auto surroundChanged = [this, i, s] {
            m_editor->setTrackSurround(i, s->depth->value(), s->center->value(), s->lfe->value(),
                                       QString("studio-surround-%1").arg(i));
        };
        for (SendBar* b : {s->depth, s->center, s->lfe}) {
            b->onChange = [surroundChanged](double) { surroundChanged(); };
            b->onFinish = [this] { m_project->closeMerge(); };
        }
        const auto track = [this, i]() -> const Track* {
            return i < m_project->timeline().audio.size() ? &m_project->timeline().audio[i] : nullptr;
        };
        connect(s->mute, &QToolButton::clicked, this, [this, i, track] {
            if (const Track* t = track()) m_editor->setTrackMuted(i, !t->muted);
        });
        connect(s->solo, &QToolButton::clicked, this, [this, i, track] {
            if (const Track* t = track()) m_editor->setTrackSolo(i, !t->solo);
        });
        s->eqThumb->onToggle = [this, i, track] {
            if (const Track* t = track()) {
                TrackEq eq = t->eq;
                eq.on = !eq.on;
                m_editor->setTrackEq(i, eq);
            }
        };
        s->dynThumb->onToggle = [this, i, track] {
            if (const Track* t = track()) {
                TrackDynamics d = t->dynamics;
                d.on = !d.on;
                m_editor->setTrackDynamics(i, d);
            }
        };
        s->eqThumb->onOpen = [this, i] { openEditor(i, true); };
        s->dynThumb->onOpen = [this, i] { openEditor(i, false); };
        connect(s->eqOpen, &QToolButton::clicked, this, [this, i] { openEditor(i, true); });
        connect(s->dynOpen, &QToolButton::clicked, this, [this, i] { openEditor(i, false); });
        m_stripLayout->insertWidget(i, s);
        m_strips << s;
    }
    fitHeight();
}

QDialog* StudioMixer::openEditor(int track, bool eq)
{
    // One window per track and kind: a second click brings the open one up (the windows follow their track when
    // tracks are added, removed or reordered: compare the track they show now)
    const auto shows = [](const QDialog* d) {
        if (const auto* e = qobject_cast<const EqDialog*>(d)) return e->track();
        if (const auto* c = qobject_cast<const DynamicsDialog*>(d)) return c->track();
        return -1;
    };
    for (QDialog* d : findChildren<QDialog*>(Qt::FindDirectChildrenOnly))
        if (d->isVisible() && track >= 0 && shows(d) == track && d->property("studioEq").toBool() == eq) {
            d->raise();
            d->activateWindow();
            return d;
        }
    QDialog* d = eq ? static_cast<QDialog*>(new EqDialog(m_editor, track, this))
                    : static_cast<QDialog*>(new DynamicsDialog(m_editor, track, this));
    d->setObjectName(eq ? "studio_eq_window" : "studio_dyn_window");
    d->setProperty("studioEq", eq);
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->adjustSize();
    // Beside its strip, left of the mixer column (over the lanes, not over the strips), inside the screen
    if (track >= 0 && track < m_strips.size()) {
        const QWidget* s = m_strips[track];
        QPoint at = s->mapToGlobal(QPoint(0, 0)) - QPoint(d->width() + 8, 0);
        at.setX(std::min(at.x(), mapToGlobal(QPoint(0, 0)).x() - d->width() - 8));
        if (const QScreen* screen = s->screen()) {
            const QRect avail = screen->availableGeometry();
            at.setX(std::clamp(at.x(), avail.left(), std::max(avail.left(), avail.right() - d->width())));
            at.setY(std::clamp(at.y(), avail.top(), std::max(avail.top(), avail.bottom() - d->height())));
        }
        d->move(at);
    }
    d->show(); // not modal: the preview keeps playing
    return d;
}

void StudioMixer::sync()
{
    // Every change of the timeline lands here: on another page only remembered (the strips follow when it shows)
    if (!isVisible()) {
        m_syncPending = true;
        return;
    }
    m_syncPending = false;
    const Timeline& tl = m_project->timeline();
    if (m_strips.size() != tl.audio.size()) rebuild(int(tl.audio.size()));
    const bool anySolo = std::any_of(tl.audio.begin(), tl.audio.end(), [](const Track& t) { return t.solo; });
    const bool surround = ChannelLayout::surround(m_project->format().audioChannels);
    for (int i = 0; i < m_strips.size(); ++i) {
        const Track& t = tl.audio[i];
        StudioStrip* s = m_strips[i];
        const TrackRef ref{TrackKind::Audio, i};
        const QString name = trackDisplayName(t, ref);
        const QString full = name == trackShortName(ref) ? name : QString("%1 %2").arg(trackShortName(ref), name);
        if (s->name->fullText() != full) s->name->setFullText(full);
        const bool silenced = anySolo && !t.solo && !t.muted;
        // A style sheet costs (Qt polishes the label again): set only when it changes
        const QString style = QString("color: %1; font-weight: 600; background: %2; padding: 1px;")
                                  .arg((silenced ? Theme::textFaint : Theme::text).name(), Theme::panelHeader.name());
        if (s->name->styleSheet() != style) s->name->setStyleSheet(style);
        s->setAutomated(!t.volumeAuto.isEmpty());
        if (!s->fader->dragging()) s->setVolume(s->automated ? Automation::levelAt(t, timelinePosition()) : t.volumeDb);
        s->setPanAutomated(!t.panAuto.isEmpty());
        if (!s->pan->dragging()) s->setPan(s->panAutomated ? Automation::panAt(t, timelinePosition()) : t.pan);
        s->surround->setVisible(surround);
        s->depth->setValue(t.panDepth);
        s->center->setValue(t.panCenter);
        s->lfe->setValue(t.lfeDb);
        s->mute->setChecked(t.muted);
        s->solo->setChecked(t.solo);
        s->eqOpen->setToolTip(t.eq.on ? T("EQ-Fenster öffnen (EQ an)") : T("EQ-Fenster öffnen (EQ aus)"));
        s->dynOpen->setToolTip(t.dynamics.on ? T("Kompressor-Fenster öffnen (Kompressor an)") : T("Kompressor-Fenster öffnen (Kompressor aus)"));
        s->eqThumb->refresh();
        s->dynThumb->refresh();
    }
    m_master->setVolume(tl.masterVolumeDb);
    m_master->limiter->setChecked(tl.masterLimiter);
    m_master->limiter->setToolTip(T("Limiter am Master (Obergrenze %1 dBFS)").arg(number(tl.masterLimiterDb)));
}

void StudioMixer::onLevels(const QVector<float>& db)
{
    if (!isVisible()) return; // nothing shows them (the levels come again when it plays on this page)
    const int groups = int(m_strips.size()) + 1;
    if (db.isEmpty() || db.size() % groups != 0) return; // the track count is changing right now
    const int ch = int(db.size()) / groups;
    if (ch != m_channels) {
        m_channels = ch;
        for (StudioStrip* s : m_strips) s->fader->setChannels(ch);
        m_master->fader->setChannels(ch);
        // 5.1/7.1: the master names its speakers (sample peaks of the mix); stereo: L, R as true peak as before
        QStringList labels;
        if (ch > 2)
            for (int c = 0; c < ch; ++c) labels << ChannelLayout::label(ch, c);
        m_master->fader->setLabels(labels);
        m_master->peak->setToolTip(
            ch > 2 ? T("Spitzenpegel (dBFS) des lautesten Lautsprechers seit dem Zurücksetzen – Klick auf die roten Marken setzt zurück")
                   : T("True Peak (dBTP) seit dem Zurücksetzen – Klick auf die roten Marken setzt zurück"));
        m_master->fitWidth();
    }
    for (int i = 0; i < m_strips.size(); ++i) m_strips[i]->fader->setLevels(db.constData() + i * ch, ch);
    m_master->fader->setLevels(db.constData() + (groups - 1) * ch, ch);
    m_lastLevels.start();
}

void StudioMixer::tick()
{
    const double dt = m_clock.restart() / 1000.0;
    if (!isVisible()) return;
    const bool silent = !m_lastLevels.isValid() || m_lastLevels.elapsed() > 150;
    // Read mode: automated faders follow their curve at the playhead (playing or not; while a file is previewed in
    // Source mode they stay at the timeline's last position)
    const Timeline& tl = m_project->timeline();
    const int at = timelinePosition();
    for (int i = 0; i < m_strips.size() && i < tl.audio.size(); ++i)
        if (StudioStrip* s = m_strips[i]) {
            if (s->automated && !s->fader->dragging()) s->setVolume(Automation::levelAt(tl.audio[i], at));
            if (s->panAutomated && !s->pan->dragging()) s->setPan(Automation::panAt(tl.audio[i], at));
        }
    // Write mode: playback stopped - the pass ends (open touches released, one undo step closed); playing with a
    // control held still: the touch records the held value (a gap between moves is then only a seek)
    if (m_writer.inPass() && !playing()) m_writer.endPass(at);
    else if (m_writer.touching()) m_writer.hold(at);
    for (StudioStrip* s : m_strips) {
        if (silent) s->fader->silence();
        s->fader->tick(dt);
        s->updatePeak();
    }
    if (silent) m_master->fader->silence();
    m_master->fader->tick(dt);
    m_master->updatePeak();
    m_loudness->setReading(m_engine->loudness(), !silent, m_loudnessTarget, m_truePeakLimit);
}
