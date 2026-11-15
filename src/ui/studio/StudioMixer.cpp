// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/studio/StudioMixer.h"
#include "ui/studio/StudioTools.h"

#include "app/DynamicsDialog.h"
#include "app/EqDialog.h"
#include "app/Theme.h"
#include "core/Automation.h"
#include "core/ChannelLayout.h"
#include "core/Dynamics.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Loudness.h"
#include "core/Project.h"
#include "core/VolumeDraw.h"
#include "engine/Engine.h"
#include "ui/EqCurve.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

namespace {

constexpr float kSilent = -200.f;
constexpr double kFloor = -60.0; // lowest value of the meters (dBFS)
constexpr double kFall = 24.0;   // meter decay dB/s
constexpr double kHold = 1.0;    // peak hold, s
constexpr int kCurveH = 56;      // EQ and compressor curves in a strip

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
                         .arg(Theme::control.name(), Theme::text.name(), on.name(), Theme::readableOn(on).name()));
    return b;
}

} // namespace

// Level meter with any number of channels next to a fader: bars on the left, the fader's scale and handle on the
// right. Drag = value (Shift = fine), 0 dB snaps, double click = 0 dB. Red over a bar = overloaded (click resets).
class StudioFader : public QWidget {
public:
    StudioFader()
    {
        setMinimumSize(60, 90);
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
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!m_dragging) return;
        const double dy = e->position().y() - m_lastY;
        m_lastY = e->position().y();
        const double h = std::max(1, bottom() - top());
        double pos = VolumeDraw::posOf(m_db) - dy / h * ((e->modifiers() & Qt::ShiftModifier) ? 0.2 : 1.0);
        double db = VolumeDraw::dbAt(pos);
        if (std::abs(db) < 0.4) db = 0; // 0 dB snaps
        change(db);
    }
    void mouseReleaseEvent(QMouseEvent*) override
    {
        if (!m_dragging) return;
        m_dragging = false;
        if (onFinish) onFinish();
    }
    void mouseDoubleClickEvent(QMouseEvent*) override
    {
        change(0);
        if (onFinish) onFinish();
    }

private:
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
};

// The compressor's static curve (input -60..0 dB across, output up): the threshold point slides sideways, the end
// point at 0 dB input up and down sets the ratio. A drag switches the compressor on; one undo step per drag. Double
// click = the full compressor window.
class DynCurve : public QWidget {
public:
    DynCurve(Editor* editor, int track) : m_editor(editor), m_track(track)
    {
        setMinimumSize(80, 40);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(kCurveH);
        setMouseTracking(true);
        setToolTip(T("Kompressor: Punkt seitlich = Schwelle, Ende hoch/runter = Verhältnis\nDoppelklick = alle Werte"));
        connect(editor->project(), &Project::timelineChanged, this, qOverload<>(&QWidget::update));
    }
    std::function<void()> onOpen;

protected:
    TrackDynamics dyn() const
    {
        const auto& audio = m_editor->project()->timeline().audio;
        return m_track < audio.size() ? audio[m_track].dynamics : TrackDynamics{};
    }
    QRectF plot() const { return QRectF(rect()).adjusted(5, 5, -5, -5); }
    QPointF at(double in, double out) const
    {
        const QRectF r = plot();
        return {r.left() + (in + 60) / 60 * r.width(), r.bottom() - (std::clamp(out, -60.0, 12.0) + 60) / 72 * r.height()};
    }
    QPointF thresholdPoint() const
    {
        const TrackDynamics d = dyn();
        return at(d.thresholdDb, Compressor::curveDb(d, d.thresholdDb));
    }
    QPointF endPoint() const { return at(0, Compressor::curveDb(dyn(), 0)); }

    void paintEvent(QPaintEvent*) override
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
        p.setPen(QPen(d.on ? Theme::primary : Theme::controlOff, 1.5));
        p.drawPath(path);
        p.setPen(QPen(Theme::border, 1));
        p.setBrush(d.on ? Theme::secondary : Theme::controlOff);
        p.drawEllipse(thresholdPoint(), 3.5, 3.5);
        p.drawEllipse(endPoint(), 3.5, 3.5);
        QFont f = font();
        f.setPointSizeF(7);
        p.setFont(f);
        p.setPen(Theme::textDim);
        p.drawText(r.adjusted(2, 0, 0, 0), Qt::AlignLeft | Qt::AlignTop,
                   QString("%1 dB  %2:1").arg(number(d.thresholdDb, 0), number(d.ratio, d.ratio < 10 ? 1 : 0)));
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const QPointF pos = e->position();
        const auto near = [&](const QPointF& a) { return std::hypot(a.x() - pos.x(), a.y() - pos.y()) <= 10; };
        m_drag = near(endPoint()) ? 2 : near(thresholdPoint()) ? 1 : 0;
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        const QPointF pos = e->position();
        if (!m_drag) {
            const auto near = [&](const QPointF& a) { return std::hypot(a.x() - pos.x(), a.y() - pos.y()) <= 10; };
            setCursor(near(endPoint()) ? Qt::SizeVerCursor : near(thresholdPoint()) ? Qt::SizeHorCursor : Qt::ArrowCursor);
            return;
        }
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
        m_editor->setTrackDynamics(m_track, d, QString("studio-dyn-%1").arg(m_track));
    }
    void mouseReleaseEvent(QMouseEvent*) override
    {
        if (m_drag) m_editor->project()->closeMerge();
        m_drag = 0;
    }
    void mouseDoubleClickEvent(QMouseEvent*) override
    {
        m_drag = 0;
        if (onOpen) onOpen();
    }

private:
    Editor* m_editor;
    int m_track;
    int m_drag = 0; // 1 = threshold, 2 = ratio
};

// Pan as a horizontal bar: drag sideways, double click = centre
class PanBar : public QWidget {
public:
    PanBar()
    {
        setFixedHeight(16);
        setMinimumWidth(60);
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
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!m_drag) return;
        const double dx = e->position().x() - m_lastX;
        m_lastX = e->position().x();
        set(m_v + dx / std::max(1, width() / 2) * 100 * ((e->modifiers() & Qt::ShiftModifier) ? 0.2 : 1.0));
    }
    void mouseReleaseEvent(QMouseEvent*) override
    {
        if (m_drag && onFinish) onFinish();
        m_drag = false;
    }
    void mouseDoubleClickEvent(QMouseEvent*) override
    {
        set(0);
        if (onFinish) onFinish();
    }

private:
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
};

// One surround send of a strip (5.1/7.1 projects): a bar filled from the left with its name and value in it; drag
// sideways, double click = its default
class SendBar : public QWidget {
public:
    SendBar(const QString& name, double lo, double hi, double def, const QString& unit)
        : m_name(name), m_lo(lo), m_hi(hi), m_def(def), m_v(def), m_unit(unit)
    {
        setFixedHeight(16);
        setMinimumWidth(60);
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
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!m_drag) return;
        const double dx = e->position().x() - m_lastX;
        m_lastX = e->position().x();
        set(m_v + dx / std::max(1, width()) * (m_hi - m_lo) * ((e->modifiers() & Qt::ShiftModifier) ? 0.2 : 1.0));
    }
    void mouseReleaseEvent(QMouseEvent*) override
    {
        if (m_drag && onFinish) onFinish();
        m_drag = false;
    }
    void mouseDoubleClickEvent(QMouseEvent*) override
    {
        set(m_def);
        if (onFinish) onFinish();
    }

private:
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
};

class StudioStrip : public QWidget {
public:
    StudioStrip(Editor* editor, int track) // track -1 = the master
    {
        const bool master = track < 0;
        setObjectName("StudioStrip");
        setAttribute(Qt::WA_StyledBackground);
        setFixedWidth(StudioMixer::kStripW + (master ? 10 : 0));
        setStyleSheet(QString("QWidget#StudioStrip { background: %1; border-right: 1px solid %2; %4 }"
                              "QLabel { color: %3; }")
                          .arg(Theme::panel.name(), Theme::border.name(), Theme::textDim.name(),
                               master ? QString("border-left: 2px solid %1;").arg(Theme::border.name()) : QString()));
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(6, 4, 6, 4);
        lay->setSpacing(4);
        name = new QLabel;
        name->setAlignment(Qt::AlignCenter);
        name->setStyleSheet(QString("color: %1; font-weight: 600; background: %2; padding: 2px;")
                                .arg(Theme::text.name(), Theme::panelHeader.name()));
        lay->addWidget(name);
        const QString tag = master ? QStringLiteral("master") : trackShortName({TrackKind::Audio, track});
        if (!master) {
            // EQ and compressor: their switches and windows ("…") in one row, below them their curves (draggable)
            auto* fxRow = new QHBoxLayout;
            fxRow->setSpacing(3);
            eq = smallButton(QStringLiteral("EQ"), Theme::primary, T("EQ der Spur an/aus"));
            eq->setObjectName("studio_eq_" + tag);
            eqOpen = smallButton(QStringLiteral("…"), Theme::primary, T("EQ-Fenster öffnen"));
            eqOpen->setCheckable(false);
            dyn = smallButton(QStringLiteral("DYN"), Theme::primary, T("Kompressor der Spur an/aus"));
            dyn->setObjectName("studio_dyn_" + tag);
            dynOpen = smallButton(QStringLiteral("…"), Theme::primary, T("Kompressor-Fenster öffnen"));
            dynOpen->setCheckable(false);
            fxRow->addWidget(eq);
            fxRow->addWidget(eqOpen);
            fxRow->addStretch(1);
            fxRow->addWidget(dyn);
            fxRow->addWidget(dynOpen);
            lay->addLayout(fxRow);
            eqCurve = new EqCurve(editor, track);
            eqCurve->setCompact(true);
            eqCurve->setObjectName("studio_eqcurve_" + tag);
            eqCurve->setFixedHeight(kCurveH);
            eqCurve->setToolTip(T("EQ: Punkte ziehen = Frequenz und Verstärkung, Doppelklick = 0 dB"));
            lay->addWidget(eqCurve);
            dynCurve = new DynCurve(editor, track);
            dynCurve->setObjectName("studio_dyncurve_" + tag);
            lay->addWidget(dynCurve);
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
            // Mute, solo, pan value and the peak in one row (room for the fader)
            auto* ms = new QHBoxLayout;
            ms->setSpacing(3);
            mute = smallButton(QStringLiteral("M"), Theme::warning, T("Stumm"));
            solo = smallButton(QStringLiteral("S"), Theme::meterMid, T("Solo: nur Spuren mit „S“ sind zu hören"));
            ms->addWidget(mute);
            ms->addWidget(solo);
            panLabel = new QLabel(QStringLiteral("C"));
            panLabel->setAlignment(Qt::AlignCenter);
            ms->addWidget(panLabel, 1);
            peak = new QLabel;
            ms->addWidget(peak, 1);
            lay->addLayout(ms);
        } else {
            limiter = smallButton(T("Limiter"), Theme::primary, {});
            limiter->setObjectName("studio_limiter");
            lay->addWidget(limiter);
            peak = new QLabel;
            lay->addWidget(peak);
        }
        peak->setAlignment(Qt::AlignCenter);
        peak->setToolTip(master ? T("True Peak (dBTP) seit dem Zurücksetzen – Klick auf die roten Marken setzt zurück")
                                : T("Spitzenpegel (dBFS) seit dem Zurücksetzen"));
        fader = new StudioFader;
        fader->setObjectName("studio_fader_" + tag);
        lay->addWidget(fader, 1);
        value = new QLabel;
        value->setAlignment(Qt::AlignCenter);
        value->setStyleSheet(QString("color: %1;").arg(Theme::text.name()));
        lay->addWidget(value);
        setVolume(0);
        updatePeak();
        m_master = master;
        fitWidth();
    }

    // As wide as its widest row needs (a large font, a long language), never narrower than the usual strip; again
    // once polished (the style sheet's fonts and paddings count)
    void fitWidth()
    {
        ensurePolished();
        for (QWidget* c : findChildren<QWidget*>()) c->ensurePolished();
        layout()->invalidate();
        setFixedWidth(std::max(StudioMixer::kStripW + (m_master ? 10 : 0), layout()->minimumSize().width()));
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
    // The curves need room: a low mixer hides them (their switches stay), the fader keeps its height
    bool curvesShown() const { return eqCurve && !eqCurve->isHidden(); }
    void setCurvesShown(bool on)
    {
        if (!eqCurve || curvesShown() == on) return;
        eqCurve->setVisible(on);
        dynCurve->setVisible(on);
    }
    // Height the strip needs with its curves shown
    int heightWithCurves() const
    {
        const int h = layout()->minimumSize().height();
        return eqCurve && !curvesShown() ? h + 2 * (kCurveH + layout()->spacing()) : h;
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

    QLabel* name;
    QToolButton* eq = nullptr;
    QToolButton* eqOpen = nullptr;
    EqCurve* eqCurve = nullptr;
    QToolButton* dyn = nullptr;
    QToolButton* dynOpen = nullptr;
    DynCurve* dynCurve = nullptr;
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
    bool m_clip = false;
    bool m_master = false;
};

// Loudness at the master, live (ITU-R BS.1770): momentary, short-term, integrated, range, true peak. Integrated green
// within ±1 LU of the target (the mixer's setting, default -14 LUFS), yellow below, red above; true peak red over the
// true-peak limit. Reset restarts the measurement.
class StudioLoudness : public QWidget {
public:
    StudioLoudness()
    {
        setObjectName("studio_loudness");
        setProperty("tourTarget", "studio_loudness");
        setAttribute(Qt::WA_StyledBackground);
        setStyleSheet(QString("QWidget#studio_loudness { background: %1; }").arg(Theme::panel.name()));
        setMinimumWidth(150);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(8, 4, 8, 6);
        lay->setSpacing(2);
        auto* title = new QLabel(T("Lautheit"));
        title->setStyleSheet(QString("color: %1; font-weight: 600; background: %2; padding: 2px;")
                                 .arg(Theme::text.name(), Theme::panelHeader.name()));
        title->setAlignment(Qt::AlignCenter);
        lay->addWidget(title);
        const struct { const char* shortName; const char* tip; } rows[] = {
            {"I", N_("Integriert (LUFS): Lautheit seit dem Zurücksetzen")},
            {"S", N_("Kurzzeit (LUFS, 3 s)")},
            {"M", N_("Momentan (LUFS, 400 ms)")},
            {"LRA", N_("Lautheitsumfang (LU)")},
            {"TP", N_("True Peak (dBTP), höchster Wert seit dem Zurücksetzen")},
        };
        for (const auto& r : rows) {
            auto* row = new QHBoxLayout;
            auto* label = new QLabel(QString::fromLatin1(r.shortName)); // EBU R128 names, as on every meter
            label->setToolTip(T(r.tip));
            label->setMinimumWidth(28);
            auto* v = new QLabel(QStringLiteral("–"));
            v->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            v->setToolTip(T(r.tip));
            QFont f = v->font();
            f.setPointSizeF(f.pointSizeF() * (values.isEmpty() ? 1.9 : 1.15));
            f.setBold(values.isEmpty());
            v->setFont(f);
            row->addWidget(label);
            row->addWidget(v, 1);
            lay->addLayout(row);
            values << v;
        }
        values[0]->setObjectName("studio_lufs");
        values[4]->setObjectName("studio_truepeak");
        target = new QLabel;
        target->setAlignment(Qt::AlignCenter);
        lay->addWidget(target);
        lay->addStretch(1);
        reset = new QToolButton;
        reset->setText(T("Zurücksetzen"));
        reset->setFocusPolicy(Qt::NoFocus);
        reset->setToolTip(T("Messung neu beginnen"));
        lay->addWidget(reset, 0, Qt::AlignHCenter);
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
        target->setText(T("Ziel %1 LUFS").arg(number(targetLufs, 0)));
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

namespace {
double loudnessTarget() { return QSettings().value("audio/loudnessTarget", -14.0).toDouble(); }
double truePeakLimit() { return QSettings().value("audio/truePeakLimit", -1.0).toDouble(); }
} // namespace

StudioMixer::StudioMixer(Editor* editor, Engine* engine, QWidget* parent)
    : QWidget(parent), m_editor(editor), m_project(editor->project()), m_engine(engine), m_writer(editor)
{
    setObjectName("studio_mixer");
    setProperty("tourTarget", "studio_mixer");
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    auto* strips = new QWidget;
    m_stripLayout = new QHBoxLayout(strips);
    m_stripLayout->setContentsMargins(0, 0, 0, 0);
    m_stripLayout->setSpacing(0);
    m_stripLayout->addStretch(1);
    m_scroll = new QScrollArea;
    m_scroll->setWidget(strips);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->horizontalScrollBar()->setSingleStep(kStripW);
    row->addWidget(m_scroll, 1);

    m_master = new StudioStrip(editor, -1);
    m_master->name->setText(T("Master"));
    m_master->fader->setOver(truePeakLimit());
    m_master->fader->setChannels(m_channels);
    m_master->fader->onChange = [this](double db) {
        m_master->setVolume(db);
        m_editor->setTrackVolume(-1, db, "studio-master");
    };
    m_master->fader->onFinish = [this] { m_project->closeMerge(); };
    connect(m_master->limiter, &QToolButton::clicked, this, [this](bool on) { m_editor->setMasterLimiter(on); });
    row->addWidget(m_master);
    m_loudness = new StudioLoudness;
    connect(m_loudness->reset, &QToolButton::clicked, this, [this] {
        m_engine->resetLoudness();
        m_master->fader->resetPeak();
        m_loudness->setReading(m_engine->loudness(), false, loudnessTarget(), truePeakLimit());
    });
    row->addWidget(m_loudness);

    connect(m_project, &Project::timelineChanged, this, &StudioMixer::sync);
    connect(m_project, &Project::formatChanged, this, [this] {
        sync();
        fitHeight(); // the surround sends take room
    });
    connect(m_engine, &Engine::audioSpeakerLevels, this, &StudioMixer::onLevels);
    m_timer = new QTimer(this);
    m_timer->setInterval(30);
    connect(m_timer, &QTimer::timeout, this, &StudioMixer::tick);
    m_clock.start(); // the timer runs while the mixer shows (showEvent)
    sync();
    m_loudness->setReading(m_engine->loudness(), false, loudnessTarget(), truePeakLimit());
}

void StudioMixer::setTools(QWidget* tools)
{
    m_tools = tools;
    auto* row = static_cast<QHBoxLayout*>(layout());
    row->insertWidget(row->indexOf(m_master), tools, 1);
    row->setStretchFactor(m_scroll, 0);
    m_scroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    fitStripsWidth();
}

void StudioMixer::fitStripsWidth()
{
    if (!m_tools) return;
    int natural = 0;
    for (StudioStrip* s : m_strips) natural += s->width();
    m_scroll->setMaximumWidth(std::max(natural, 1) + m_scroll->frameWidth() * 2);
    // A narrow page (small window, large font): the master and the loudness keep their size, the tools one card if
    // there is room beside one strip (else less, their cards scroll), the strips what is left up to all of them (they
    // scroll beyond). Without the strips' share as a minimum the scroll area took its own small size hint: one strip
    // of three on a 1920 px page, the tools the rest
    const int room = width() - m_master->width() - m_loudness->minimumWidth();
    const auto* tools = qobject_cast<StudioTools*>(m_tools);
    const int card = tools ? tools->cardWidth() + 12 : 240;
    const int toolsMin = std::clamp(room - std::min(natural, kStripW), std::min(160, card), card);
    m_tools->setMinimumWidth(toolsMin);
    m_scroll->setMinimumWidth(std::clamp(room - toolsMin, 0, natural + m_scroll->frameWidth() * 2));
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
    fitHeight();
    m_clock.restart(); // the meters fall from now, not by the time it was hidden
    m_timer->start();
}

void StudioMixer::hideEvent(QHideEvent* e)
{
    QWidget::hideEvent(e);
    m_timer->stop(); // another page: no meters to move
    // Without the timer tick() cannot see playback stop: a write pass ends here (open touches released, one undo step)
    if (m_writer.inPass()) m_writer.endPass(m_engine->position());
}

void StudioMixer::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    fitHeight();
}

bool StudioMixer::playing() const { return !std::isnan(m_engine->soundFrame()); }

void StudioMixer::setWriteMode(bool on)
{
    if (on == m_writeMode) return;
    m_writeMode = on;
    if (!on) m_writer.endPass(m_engine->position());
}

void StudioMixer::fitHeight()
{
    const int room = m_scroll->viewport()->height();
    for (StudioStrip* s : m_strips) {
        s->fitWidth();
        s->setCurvesShown(room >= s->heightWithCurves());
    }
    m_master->fitWidth();
    // The loudness keeps its text whole (a large font, a long language): at least what its labels need
    if (QLayout* l = m_loudness->layout()) {
        l->invalidate();
        m_loudness->setMinimumWidth(std::max(150, l->minimumSize().width()));
    }
    fitStripsWidth();
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
                m_writer.move(i, AutoTarget::Volume, m_engine->position(), db);
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
                m_writer.move(i, AutoTarget::Pan, m_engine->position(), v);
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
            if (m_writer.touching(i, AutoTarget::Volume)) m_writer.release(i, AutoTarget::Volume, m_engine->position());
            else if (!m_writer.inPass()) m_project->closeMerge();
        };
        s->pan->onFinish = [this, i] {
            if (m_writer.touching(i, AutoTarget::Pan)) m_writer.release(i, AutoTarget::Pan, m_engine->position());
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
        connect(s->eq, &QToolButton::clicked, this, [this, i, track](bool on) {
            if (const Track* t = track()) {
                TrackEq eq = t->eq;
                eq.on = on;
                m_editor->setTrackEq(i, eq);
            }
        });
        connect(s->dyn, &QToolButton::clicked, this, [this, i, track](bool on) {
            if (const Track* t = track()) {
                TrackDynamics d = t->dynamics;
                d.on = on;
                m_editor->setTrackDynamics(i, d);
            }
        });
        connect(s->eqOpen, &QToolButton::clicked, this, [this, i] { openEditor(i, true); });
        connect(s->dynOpen, &QToolButton::clicked, this, [this, i] { openEditor(i, false); });
        s->dynCurve->onOpen = [this, i] { openEditor(i, false); };
        m_stripLayout->insertWidget(i, s);
        m_strips << s;
    }
    fitHeight();
}

void StudioMixer::openEditor(int track, bool eq)
{
    // One window per track and kind: a second click brings the open one up
    for (QDialog* d : findChildren<QDialog*>(Qt::FindDirectChildrenOnly))
        if (d->isVisible() && d->property("studioTrack").toInt() == track && d->property("studioEq").toBool() == eq) {
            d->raise();
            d->activateWindow();
            return;
        }
    QDialog* d = eq ? static_cast<QDialog*>(new EqDialog(m_editor, track, this))
                    : static_cast<QDialog*>(new DynamicsDialog(m_editor, track, this));
    d->setProperty("studioTrack", track);
    d->setProperty("studioEq", eq);
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->show(); // not modal: the preview keeps playing
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
        s->name->setText(name == trackShortName(ref) ? name : QString("%1  %2").arg(trackShortName(ref), name));
        s->name->setToolTip(name);
        const bool silenced = anySolo && !t.solo && !t.muted;
        // A style sheet costs (Qt polishes the label again): set only when it changes
        const QString style = QString("color: %1; font-weight: 600; background: %2; padding: 2px;")
                                  .arg((silenced ? Theme::textFaint : Theme::text).name(), Theme::panelHeader.name());
        if (s->name->styleSheet() != style) s->name->setStyleSheet(style);
        s->setAutomated(!t.volumeAuto.isEmpty());
        if (!s->fader->dragging()) s->setVolume(s->automated ? Automation::levelAt(t, m_engine->position()) : t.volumeDb);
        s->setPanAutomated(!t.panAuto.isEmpty());
        if (!s->pan->dragging()) s->setPan(s->panAutomated ? Automation::panAt(t, m_engine->position()) : t.pan);
        s->surround->setVisible(surround);
        s->depth->setValue(t.panDepth);
        s->center->setValue(t.panCenter);
        s->lfe->setValue(t.lfeDb);
        s->mute->setChecked(t.muted);
        s->solo->setChecked(t.solo);
        s->eq->setChecked(t.eq.on);
        s->dyn->setChecked(t.dynamics.on);
        s->eqCurve->update();
        s->dynCurve->update();
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
        fitStripsWidth();
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
    // Read mode: automated faders follow their curve at the playhead (playing or not)
    const Timeline& tl = m_project->timeline();
    for (int i = 0; i < m_strips.size() && i < tl.audio.size(); ++i)
        if (StudioStrip* s = m_strips[i]) {
            if (s->automated && !s->fader->dragging()) s->setVolume(Automation::levelAt(tl.audio[i], m_engine->position()));
            if (s->panAutomated && !s->pan->dragging()) s->setPan(Automation::panAt(tl.audio[i], m_engine->position()));
        }
    // Write mode: playback stopped - the pass ends (open touches released, one undo step closed)
    if (m_writer.inPass() && !playing()) m_writer.endPass(m_engine->position());
    for (StudioStrip* s : m_strips) {
        if (silent) s->fader->silence();
        s->fader->tick(dt);
        s->updatePeak();
    }
    if (silent) m_master->fader->silence();
    m_master->fader->tick(dt);
    m_master->updatePeak();
    m_loudness->setReading(m_engine->loudness(), !silent, loudnessTarget(), truePeakLimit());
}
