#include "ui/Mixer.h"

#include "app/Theme.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Loudness.h"
#include "engine/Engine.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QSettings>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>

namespace {

constexpr int kStripW = 70;
constexpr float kSilent = -200.f;
constexpr double kMeterFloor = -60.0; // unterster Wert der Pegelanzeige (dBFS)
constexpr double kMeterFall = 24.0;   // Abfall in dB/s (Anstieg sofort)
constexpr double kPeakHold = 1.0;     // Peak-Hold in Sekunden
constexpr double kClipDb = -0.001;    // ab hier Übersteuerung (0 dBFS; volle s16-Aussteuerung zählt mit)

// Fader-Skala wie die Lautstärkelinie in der Timeline (0 = unten, 1 = oben):
// unteres Viertel -∞..-20 dB, Mitte -20..0 dB, oberes Viertel 0..+12 dB
double faderPos(double db)
{
    db = std::clamp(db, kMinVolumeDb, kMaxVolumeDb);
    if (db < -20) return 0.25 * (db - kMinVolumeDb) / (-20 - kMinVolumeDb);
    if (db < 0) return 0.25 + 0.5 * (db + 20) / 20;
    return 0.75 + 0.25 * db / kMaxVolumeDb;
}

double faderDb(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    if (t < 0.25) return kMinVolumeDb + t / 0.25 * (-20 - kMinVolumeDb);
    if (t < 0.75) return -20 + (t - 0.25) / 0.5 * 20;
    return (t - 0.75) / 0.25 * kMaxVolumeDb;
}

double meterPos(double db) { return std::clamp((db - kMeterFloor) / -kMeterFloor, 0.0, 1.0); }

QColor meterColor(double db)
{
    if (db >= -3) return Theme::meterHigh;
    if (db >= -12) return Theme::meterMid;
    return Theme::meterLow;
}

// Spitzenwert-Anzeige über dem Pegel: "-3.2", "+1.5", "-∞"
QString peakText(double db)
{
    if (db <= -99) return QStringLiteral("-∞");
    return QString("%1%2").arg(db > 0.05 ? "+" : "").arg(db, 0, 'f', 1);
}

QString panText(double pan)
{
    const int v = int(std::lround(pan));
    if (v == 0) return "C";
    return QString("%1 %2").arg(v < 0 ? "L" : "R").arg(std::abs(v));
}

} // namespace

// Pegelanzeige (Stereo) + Fader in einem Widget: links die Balken, Mitte die Fader-Skala, rechts der Fader.
// Ziehen = Wert ändern (Shift = fein), 0 dB rastet ein, Doppelklick = 0 dB.
class FaderMeter : public QWidget {
public:
    FaderMeter()
    {
        setMinimumHeight(120);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        setFixedWidth(kStripW - 6);
        setCursor(Qt::SizeVerCursor);
        setMouseTracking(true); // Zeiger über der Clip-Anzeige
    }

    std::function<void(double)> onChange;
    std::function<void()> onFinish;
    std::function<void()> onResetPeak; // Klick auf die Clip-Anzeige

    void setValue(double db)
    {
        if (db == m_db) return;
        m_db = db;
        update();
    }
    void setLevel(float l, float r)
    {
        m_target[0] = l;
        m_target[1] = r;
        for (int ch = 0; ch < 2; ++ch) {
            m_max = std::max(m_max, m_target[ch]);
            if (m_target[ch] >= kClipDb && !m_clip[ch]) {
                m_clip[ch] = true; // bleibt stehen bis Klick (wie DaVinci)
                update();
            }
        }
    }
    // Höchster Pegel seit dem letzten Zurücksetzen (dBFS) und ob übersteuert wurde
    float maxPeak() const { return m_max; }
    bool clipped() const { return m_clip[0] || m_clip[1]; }
    void resetPeak()
    {
        m_max = kSilent;
        m_clip[0] = m_clip[1] = false;
        update();
    }
    // Anzeige nachführen; true = neu zeichnen nötig
    bool tick(double dt)
    {
        bool changed = false;
        for (int ch = 0; ch < 2; ++ch) {
            const float before = m_disp[ch], holdBefore = m_hold[ch];
            m_disp[ch] = std::max(m_target[ch], float(m_disp[ch] - kMeterFall * dt));
            if (m_disp[ch] >= m_hold[ch]) {
                m_hold[ch] = m_disp[ch];
                m_holdAge[ch] = 0;
            } else if ((m_holdAge[ch] += dt) > kPeakHold) {
                m_hold[ch] = m_disp[ch];
            }
            changed |= meterPos(before) != meterPos(m_disp[ch]) || meterPos(holdBefore) != meterPos(m_hold[ch]);
        }
        if (changed) update();
        return changed;
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        const int top = trackTop(), bottom = trackBottom(), h = bottom - top;

        // Pegel: zwei Balken, darüber je eine Clip-Anzeige (rot = übersteuert, bleibt bis Klick)
        const int mx = 3, bw = 5;
        for (int ch = 0; ch < 2; ++ch) {
            p.fillRect(clipRect(ch), m_clip[ch] ? Theme::warning : Theme::well);
            const QRect bar(mx + ch * (bw + 1), top, bw, h + 1);
            p.fillRect(bar, Theme::well);
            const auto yOf = [&](double db) { return bottom - int(std::lround(meterPos(db) * h)); };
            const int y = yOf(m_disp[ch]);
            if (y < bottom) {
                // Farbzonen: grün bis -12, gelb bis -3, rot darüber
                const int yY = yOf(-12), yR = yOf(-3);
                auto zone = [&](int from, int to, const QColor& c) {
                    const int a = std::max(y, to), b = from;
                    if (b > a) p.fillRect(bar.x(), a, bw, b - a, c);
                };
                zone(bottom + 1, yY, meterColor(-20));
                zone(yY, yR, meterColor(-6));
                zone(yR, top, meterColor(0));
            }
            if (m_hold[ch] > kMeterFloor) p.fillRect(bar.x(), yOf(m_hold[ch]), bw, 2, meterColor(m_hold[ch]));
        }
        // Segment-Linien über den Balken (alle 6 dB)
        for (int db = -6; db > kMeterFloor; db -= 6) {
            const int y = bottom - int(std::lround(meterPos(db) * h));
            p.fillRect(mx, y, 2 * bw + 1, 1, Theme::alpha(Theme::well, 160));
        }

        // Fader-Skala
        QFont f = font();
        f.setPointSizeF(7);
        p.setFont(f);
        const int fx = faderX();
        const int lx = mx + 2 * bw + 2;
        for (int db : {12, 6, 0, -6, -12, -20, -40, -60}) {
            const int y = faderY(db);
            p.setPen(db == 0 ? Theme::text : Theme::textDim);
            const QString t = db <= kMinVolumeDb ? QStringLiteral("-∞") : QString::number(db);
            p.drawText(QRect(lx, y - 6, fx - 14 - lx, 12), Qt::AlignRight | Qt::AlignVCenter, t);
            p.fillRect(fx - 12, y, 3, 1, db == 0 ? Theme::text : Theme::textDim);
        }

        // Fader: Schiene + Kappe
        p.fillRect(fx - 1, top, 3, h + 1, Theme::well);
        const int ky = faderY(m_db);
        const QRect knob(fx - 9, ky - 6, 19, 12);
        p.setRenderHint(QPainter::Antialiasing, true);
        QLinearGradient g(knob.topLeft(), knob.bottomLeft());
        g.setColorAt(0, Theme::mix(Theme::controlLight, Theme::text, 0.37));
        g.setColorAt(1, Theme::mix(Theme::controlLight, Theme::controlOff, 0.3));
        p.setPen(Theme::well);
        p.setBrush(g);
        p.drawRoundedRect(knob, 2, 2);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.fillRect(knob.left() + 3, ky, knob.width() - 6, 1, Theme::mix(Theme::text, Qt::white, 0.5));
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        if (clipRect(0).united(clipRect(1)).adjusted(-2, -2, 2, 2).contains(e->position().toPoint())) {
            if (onResetPeak) onResetPeak();
            return;
        }
        m_dragging = true;
        m_pressY = e->position().y();
        m_startDb = m_db;
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!m_dragging) {
            // Über der Clip-Anzeige Handzeiger (Klick setzt zurück), sonst Fader
            const bool onClip = clipRect(0).united(clipRect(1)).adjusted(-2, -2, 2, 2).contains(e->position().toPoint());
            setCursor(onClip ? Qt::PointingHandCursor : Qt::SizeVerCursor);
            return;
        }
        const double dy = e->position().y() - m_pressY;
        double db;
        if (e->modifiers() & Qt::ShiftModifier) {
            db = m_startDb - dy * 0.1; // fein: 0,1 dB pro Pixel
        } else {
            db = faderDb(faderPos(m_startDb) - dy / std::max(1, trackBottom() - trackTop()));
            if (std::abs(db) < 0.5) db = 0; // 0 dB rastet ein
        }
        db = std::clamp(std::round(db * 10) / 10, kMinVolumeDb, kMaxVolumeDb);
        if (db == m_db) return;
        setValue(db);
        if (onChange) onChange(db);
    }
    void mouseReleaseEvent(QMouseEvent*) override
    {
        if (!m_dragging) return;
        m_dragging = false;
        if (onFinish) onFinish();
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (clipRect(0).united(clipRect(1)).adjusted(-2, -2, 2, 2).contains(e->position().toPoint())) return;
        m_dragging = false;
        setValue(0);
        if (onChange) onChange(0);
        if (onFinish) onFinish();
    }

private:
    QRect clipRect(int ch) const { return QRect(3 + ch * 6, 0, 5, 5); }
    int trackTop() const { return 8; }
    int trackBottom() const { return height() - 11; }
    int faderX() const { return width() - 12; }
    int faderY(double db) const
    {
        return trackBottom() - int(std::lround(faderPos(db) * (trackBottom() - trackTop())));
    }

    double m_db = 0;
    float m_target[2] = {kSilent, kSilent}, m_disp[2] = {kSilent, kSilent}, m_hold[2] = {kSilent, kSilent};
    double m_holdAge[2] = {0, 0};
    float m_max = kSilent;
    bool m_clip[2] = {false, false};
    bool m_dragging = false;
    double m_pressY = 0, m_startDb = 0;
};

// Pan-Drehregler: ziehen (hoch/rechts = rechts, Shift = fein), Doppelklick = Mitte
class PanKnob : public QWidget {
public:
    PanKnob()
    {
        setFixedSize(30, 30);
        setCursor(Qt::SizeHorCursor);
    }

    std::function<void(double)> onChange;
    std::function<void()> onFinish;

    void setValue(double v)
    {
        if (v == m_value) return;
        m_value = v;
        setToolTip(QStringLiteral("Pan: ") + panText(v));
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(4, 4, -4, -4);
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::control);
        p.drawEllipse(r);
        // Bogen von der Mitte (oben) bis zum Wert, ±135°
        const double angle = m_value / 100.0 * 135.0;
        p.setPen(QPen(Theme::primary, 2.5, Qt::SolidLine, Qt::FlatCap));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5), 90 * 16, int(-angle * 16));
        const double rad = (90 - angle) * M_PI / 180.0;
        const QPointF c = r.center();
        p.setPen(QPen(Theme::text, 1.5));
        p.drawLine(c, c + QPointF(std::cos(rad), -std::sin(rad)) * (r.width() / 2 - 1));
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        m_dragging = true;
        m_press = e->position();
        m_start = m_value;
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!m_dragging) return;
        const QPointF d = e->position() - m_press;
        const double step = (e->modifiers() & Qt::ShiftModifier) ? 0.2 : 1.0;
        const double v = std::clamp(std::round(m_start + (d.x() - d.y()) * step), -100.0, 100.0);
        if (v == m_value) return;
        setValue(v);
        if (onChange) onChange(v);
    }
    void mouseReleaseEvent(QMouseEvent*) override
    {
        if (!m_dragging) return;
        m_dragging = false;
        if (onFinish) onFinish();
    }
    void mouseDoubleClickEvent(QMouseEvent*) override
    {
        m_dragging = false;
        setValue(0);
        if (onChange) onChange(0);
        if (onFinish) onFinish();
    }

private:
    double m_value = 0.1; // != 0, damit der erste setValue(0) den Tooltip setzt
    bool m_dragging = false;
    QPointF m_press;
    double m_start = 0;
};

// Anklickbare Spitzenwert-Anzeige (Klick = zurücksetzen)
class PeakLabel : public QLabel {
public:
    std::function<void()> onClick;

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && onClick) onClick();
    }
};

// Ein Kanalzug: Name, Pan, M/S, Spitzenwert, Pegel+Fader, Wert. Beim Master bleiben Pan/M/S leer (gleiche Höhe),
// statt M/S sitzt dort der Limiter-Schalter.
class ChannelStrip : public QWidget {
public:
    explicit ChannelStrip(bool master)
    {
        setFixedWidth(kStripW);
        setAttribute(Qt::WA_StyledBackground);
        setObjectName("Strip");
        setStyleSheet(QString("QWidget#Strip { background: %1; border-right: 1px solid %2; }"
                              "QLabel { color: %3; font-size: 8pt; }")
                          .arg(Theme::panel.name(), Theme::border.name(), Theme::textDim.name()));
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(3, 4, 3, 4);
        lay->setSpacing(3);

        name = new QLabel;
        name->setAlignment(Qt::AlignCenter);
        name->setStyleSheet(QString("color: %1; font-weight: 600; background: %2; padding: 2px;")
                                .arg(Theme::text.name(), Theme::panelHeader.name()));
        lay->addWidget(name);

        pan = new PanKnob;
        lay->addWidget(pan, 0, Qt::AlignHCenter);
        panLabel = new QLabel("C");
        panLabel->setAlignment(Qt::AlignCenter);
        lay->addWidget(panLabel);

        auto* ms = new QHBoxLayout;
        ms->setSpacing(3);
        auto makeButton = [&](const QString& text, const QColor& on, const QString& tip) {
            auto* b = new QToolButton;
            b->setText(text);
            b->setCheckable(true);
            b->setFocusPolicy(Qt::NoFocus);
            b->setToolTip(tip);
            b->setFixedSize(26, 18);
            b->setStyleSheet(QString("QToolButton { background: %3; color: %1; font-weight: 600; padding: 0; }"
                                     "QToolButton:checked { background: %2; color: %4; }")
                                 .arg(Theme::text.name(), on.name(), Theme::control.name(), Theme::readableOn(on).name()));
            ms->addWidget(b);
            return b;
        };
        mute = makeButton("M", Theme::warning, T("Stumm (Mute)"));
        solo = makeButton("S", Theme::meterMid, T("Solo: nur Spuren mit „S“ sind zu hören, die anderen werden "
                                                   "stumm (ausgegraut)"));
        if (master) {
            limiter = new QToolButton;
            limiter->setText("LIM");
            limiter->setCheckable(true);
            limiter->setFocusPolicy(Qt::NoFocus);
            limiter->setFixedSize(55, 18);
            limiter->setContextMenuPolicy(Qt::CustomContextMenu);
            limiter->setStyleSheet(QString("QToolButton { background: %2; color: %1; font-weight: 600; padding: 0; }"
                                           "QToolButton:checked { background: %3; color: %4; }")
                                       .arg(Theme::text.name(), Theme::control.name(), Theme::primary.name(),
                                            Theme::onPrimary.name()));
            ms->addWidget(limiter);
        }
        lay->addLayout(ms);

        // Spitzenwert seit dem letzten Zurücksetzen (rot hinterlegt = übersteuert); Klick setzt zurück
        peak = new PeakLabel;
        peak->setAlignment(Qt::AlignCenter);
        peak->setCursor(Qt::PointingHandCursor);
        peak->setToolTip(T("Spitzenpegel (dBFS) – Klick setzt zurück"));
        lay->addWidget(peak);

        fader = new FaderMeter;
        lay->addWidget(fader, 1, Qt::AlignHCenter);
        value = new QLabel;
        value->setAlignment(Qt::AlignCenter);
        value->setStyleSheet(QString("color: %1; font-size: 8pt;").arg(Theme::text.name()));
        lay->addWidget(value);

        peak->onClick = fader->onResetPeak = [this] {
            fader->resetPeak();
            updatePeak();
        };
        if (master) {
            for (QWidget* w : {static_cast<QWidget*>(pan), static_cast<QWidget*>(panLabel)}) {
                QSizePolicy sp = w->sizePolicy();
                sp.setRetainSizeWhenHidden(true);
                w->setSizePolicy(sp);
                w->hide();
            }
            // M/S gibt es am Master nicht; an ihrer Stelle (gleiche Höhe) sitzt der Limiter-Schalter
            for (QToolButton* b : {mute, solo}) {
                ms->removeWidget(b);
                b->hide();
            }
        }
        setVolume(0);
        setPan(0);
        updatePeak();
    }

    // Spitzenwert-Anzeige an den Pegelmesser angleichen (nur bei Änderung neu setzen)
    void updatePeak()
    {
        const float db = fader->maxPeak();
        const bool clip = fader->clipped();
        const QString text = peakText(db);
        if (text == peak->text() && clip == m_peakClip && !peak->styleSheet().isEmpty()) return;
        m_peakClip = clip;
        peak->setText(text);
        peak->setStyleSheet(clip ? QString("color: %1; background: %2; font-size: 8pt; font-weight: 600;")
                                       .arg(Theme::readableOn(Theme::warning).name(), Theme::warning.name())
                                 : QString("color: %1; background: %2; font-size: 8pt;")
                                       .arg(Theme::textDim.name(), Theme::well.name()));
    }

    void setVolume(double db)
    {
        fader->setValue(db);
        value->setText(formatVolumeDb(db));
    }
    void setPan(double v)
    {
        pan->setValue(v);
        panLabel->setText(panText(v));
    }

    QLabel* name;
    PanKnob* pan;
    QLabel* panLabel;
    QToolButton* mute;
    QToolButton* solo;
    QToolButton* limiter = nullptr; // nur Master
    PeakLabel* peak;
    FaderMeter* fader;
    QLabel* value;

private:
    bool m_peakClip = false;
};

// Loudness-Meter wie in DaVinci (Fairlight): Balken für Momentary (M) und Short-term (S) in LUFS, darunter
// Integrated, Short-term, Momentary und Loudness Range. Ziellinie orange (Rechtsklick = Ziel wählen),
// Integrated grün im Zielbereich (±1 LU), gelb darunter, rot darüber. Reset beginnt die Messung neu.
class LoudnessView : public QWidget {
public:
    static constexpr double kFloor = -48.0; // unterster Wert der Skala (LUFS)

    LoudnessView()
    {
        setMinimumHeight(120);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        setFixedWidth(kStripW + 20);
    }

    void setReading(const LoudnessReading& r, bool live)
    {
        // Nur bei sichtbarer Änderung neu zeichnen
        auto round1 = [](double v) { return std::lround(v * 10); };
        const bool changed = live != m_live || round1(r.momentary) != round1(m_r.momentary)
                             || round1(r.shortTerm) != round1(m_r.shortTerm)
                             || round1(r.integrated) != round1(m_r.integrated) || round1(r.range) != round1(m_r.range);
        m_r = r;
        m_live = live;
        if (changed) update();
    }
    void setTarget(double lufs)
    {
        m_target = lufs;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        QFont f = font();
        f.setPointSizeF(7);
        p.setFont(f);
        const QFontMetrics fm(f);
        const int rowH = fm.height() + 1;
        const int textH = 4 * rowH + 4;
        const int top = 14, bottom = height() - textH - 6, h = std::max(10, bottom - top);
        const auto yOf = [&](double lufs) {
            return bottom - int(std::lround(std::clamp((lufs - kFloor) / -kFloor, 0.0, 1.0) * h));
        };
        // Balken M und S
        const int bw = 9, x0 = 6;
        const double values[2] = {m_live ? m_r.momentary : LoudnessMeter::kSilence,
                                  m_live ? m_r.shortTerm : LoudnessMeter::kSilence};
        const char* names[2] = {"M", "S"};
        for (int i = 0; i < 2; ++i) {
            const QRect bar(x0 + i * (bw + 3), top, bw, h + 1);
            p.fillRect(bar, Theme::well);
            const int y = yOf(values[i]);
            if (y < bottom) p.fillRect(bar.x(), y, bw, bottom - y + 1, levelColor(values[i]));
            p.setPen(Theme::textDim);
            p.drawText(QRect(bar.x() - 2, 0, bw + 4, top - 2), Qt::AlignCenter, names[i]);
        }
        // Skala (LUFS) und Ziellinie
        const int sx = x0 + 2 * (bw + 3);
        for (int v = 0; v >= int(kFloor); v -= 6) {
            const int y = yOf(v);
            p.fillRect(x0, y, sx - x0 - 3, 1, Theme::alpha(Theme::well, 160));
            p.setPen(Theme::textDim);
            p.drawText(QRect(sx + 2, y - 6, width() - sx - 4, 12), Qt::AlignLeft | Qt::AlignVCenter, QString::number(v));
        }
        const int ty = yOf(m_target);
        p.fillRect(x0 - 2, ty, sx - x0 + 1, 2, Theme::primary);

        // Werte
        const int ly = bottom + 8;
        auto row = [&](int i, const QString& label, const QString& value, const QColor& color) {
            const QRect r(4, ly + i * rowH, width() - 8, rowH);
            p.setPen(Theme::textDim);
            p.drawText(r, Qt::AlignLeft | Qt::AlignVCenter, label);
            p.setPen(color);
            p.drawText(r, Qt::AlignRight | Qt::AlignVCenter, value);
        };
        const double I = m_r.integrated;
        QColor ic = Theme::text;
        if (I > LoudnessMeter::kSilence)
            ic = std::abs(I - m_target) <= 1.0 ? Theme::meterLow
                 : I < m_target                ? Theme::meterMid
                                               : Theme::meterHigh;
        row(0, "I", lufsText(I), ic);
        row(1, "S", lufsText(values[1]), Theme::text);
        row(2, "M", lufsText(values[0]), Theme::text);
        row(3, "LRA", QString("%1 LU").arg(m_r.range, 0, 'f', 1), Theme::text);
    }

private:
    QColor levelColor(double lufs) const
    {
        if (lufs > m_target + 1) return Theme::meterHigh;
        if (lufs >= m_target - 1) return Theme::meterLow;
        return Theme::quiet;
    }
    static QString lufsText(double v)
    {
        if (v <= -99) return QStringLiteral("–");
        return QString::number(v, 'f', 1);
    }

    LoudnessReading m_r;
    bool m_live = false;
    double m_target = -14.0;
};

// Kasten rechts neben dem Master: Titel, Anzeige, Reset
class LoudnessStrip : public QWidget {
public:
    LoudnessStrip()
    {
        setAttribute(Qt::WA_StyledBackground);
        setObjectName("Strip");
        setStyleSheet(QString("QWidget#Strip { background: %1; border-left: 1px solid %2; }")
                          .arg(Theme::panel.name(), Theme::border.name()));
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(3, 4, 3, 4);
        lay->setSpacing(3);
        auto* name = new QLabel(T("Lautheit"));
        name->setAlignment(Qt::AlignCenter);
        name->setStyleSheet(QString("color: %1; font-weight: 600; font-size: 8pt; background: %2; padding: 2px;")
                                .arg(Theme::text.name(), Theme::panelHeader.name()));
        name->setToolTip(T("Loudness-Meter (ITU-R BS.1770-4, LUFS) am Master\n"
                           "Misst bei Wiedergabe der Timeline. Rechtsklick = Ziellautheit"));
        lay->addWidget(name);
        view = new LoudnessView;
        view->setToolTip(name->toolTip());
        lay->addWidget(view, 1, Qt::AlignHCenter);
        reset = new QToolButton;
        reset->setText(T("Reset"));
        reset->setFocusPolicy(Qt::NoFocus);
        reset->setToolTip(T("Messung neu beginnen (Integrated, LRA)"));
        reset->setFixedHeight(18);
        reset->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        reset->setStyleSheet(QString("QToolButton { background: %2; color: %1; font-size: 8pt; padding: 0; }")
                                 .arg(Theme::text.name(), Theme::control.name()));
        lay->addWidget(reset);
        setContextMenuPolicy(Qt::CustomContextMenu);
    }
    LoudnessView* view;
    QToolButton* reset;
};

Mixer::Mixer(Project* project, Engine* engine, QWidget* parent)
    : QWidget(parent), m_project(project), m_engine(engine)
{
    setObjectName("Panel");
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto* title = new QLabel("Mixer");
    title->setObjectName("PanelTitle");
    root->addWidget(title);

    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    auto* strips = new QWidget;
    m_stripLayout = new QHBoxLayout(strips);
    m_stripLayout->setContentsMargins(0, 0, 0, 0);
    m_stripLayout->setSpacing(0);
    m_stripLayout->addStretch(1);
    auto* scroll = new QScrollArea;
    scroll->setWidget(strips);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Streifen immer dicht an Master/Lautheit (feste Breite wie im kleinen Fenster), übriger Platz bleibt rechts leer;
    // erst wenn der Platz nicht reicht, scrollen die Spuren (Breite setzt rebuildStrips)
    m_stripScroll = scroll;
    // hoher Stretch: Spuren bekommen den Platz zuerst (bis zur Maximalbreite), erst der Rest geht an den Platzhalter rechts
    row->addWidget(scroll, 100);

    // Master rechts, fest (wie der Bus in DaVinci)
    m_master = new ChannelStrip(true);
    m_master->name->setText("Master");
    m_master->setStyleSheet(m_master->styleSheet() +
                            QString("QWidget#Strip { border-left: 2px solid %1; }").arg(Theme::border.name()));
    m_master->fader->onChange = [this](double db) {
        m_master->setVolume(db);
        m_engine->mixerOnlyNext();
        m_project->edit(T("Master-Lautstärke"), [db](Timeline& tl) { tl.masterVolumeDb = db; }, "mixer-master");
    };
    m_master->fader->onFinish = [this] { m_project->closeMerge(); };
    // Limiter am Master (wie ein Bus-Limiter in DaVinci Fairlight): Klick = an/aus, Rechtsklick = Ceiling
    connect(m_master->limiter, &QToolButton::clicked, this, [this](bool on) {
        m_engine->mixerOnlyNext();
        m_project->edit(on ? T("Limiter an") : T("Limiter aus"), [on](Timeline& tl) { tl.masterLimiter = on; });
    });
    connect(m_master->limiter, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QMenu menu(this);
        menu.addSection(T("Limiter-Ceiling"));
        const double current = m_project->timeline().masterLimiterDb;
        for (double db : {0.0, -0.1, -0.3, -0.5, -1.0, -2.0, -3.0, -6.0}) {
            QAction* a = menu.addAction(QString("%1 dBFS").arg(db, 0, 'f', 1));
            a->setCheckable(true);
            a->setChecked(std::abs(current - db) < 0.001);
            connect(a, &QAction::triggered, this, [this, db] {
                m_engine->mixerOnlyNext();
                m_project->edit(T("Limiter-Ceiling"), [db](Timeline& tl) {
                    tl.masterLimiterDb = db;
                    tl.masterLimiter = true; // Ceiling wählen schaltet ein
                });
            });
        }
        menu.exec(m_master->limiter->mapToGlobal(pos));
    });
    row->addWidget(m_master);

    // Loudness-Meter (Ziel in den Einstellungen, Standard -14 LUFS wie YouTube)
    m_loudness = new LoudnessStrip;
    m_loudness->view->setTarget(QSettings().value("audio/loudnessTarget", -14.0).toDouble());
    connect(m_loudness->reset, &QToolButton::clicked, this, [this] {
        m_engine->resetLoudness();
        m_loudness->view->setReading(m_engine->loudness(), false);
    });
    connect(m_loudness, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QMenu menu(this);
        menu.addSection(T("Ziellautheit"));
        const double current = QSettings().value("audio/loudnessTarget", -14.0).toDouble();
        const std::pair<double, const char*> targets[] = {{-14.0, "YouTube / Spotify"}, {-16.0, "Apple Music / Podcast"},
                                                          {-23.0, "EBU R128"}, {-24.0, "ATSC A/85"}};
        for (const auto& [lufs, name] : targets) {
            QAction* a = menu.addAction(QString("%1 LUFS  (%2)").arg(lufs, 0, 'f', 0).arg(name));
            a->setCheckable(true);
            a->setChecked(std::abs(current - lufs) < 0.01);
            connect(a, &QAction::triggered, this, [this, lufs = lufs] {
                QSettings().setValue("audio/loudnessTarget", lufs);
                m_loudness->view->setTarget(lufs);
            });
        }
        menu.exec(m_loudness->mapToGlobal(pos));
    });
    row->addWidget(m_loudness);
    row->addStretch(1);
    root->addLayout(row, 1);

    setMinimumWidth(kStripW * 3 + 40);

    connect(m_project, &Project::timelineChanged, this, &Mixer::sync);
    connect(m_engine, &Engine::audioLevels, this, &Mixer::onLevels);

    m_timer = new QTimer(this);
    m_timer->setInterval(30);
    connect(m_timer, &QTimer::timeout, this, &Mixer::tick);
    m_timer->start();
    m_clock.start();
    sync();
}

void Mixer::rebuildStrips(int count)
{
    qDeleteAll(m_strips);
    m_strips.clear();
    for (int i = 0; i < count; ++i) {
        auto* s = new ChannelStrip(false);
        s->fader->onChange = [this, i, s](double db) {
            s->setVolume(db);
            m_engine->mixerOnlyNext();
            m_project->edit(T("Spurlautstärke"), [i, db](Timeline& tl) {
                if (i < tl.audio.size()) tl.audio[i].volumeDb = db;
            }, "mixer-vol-" + QString::number(i));
        };
        s->pan->onChange = [this, i, s](double v) {
            s->setPan(v);
            m_engine->mixerOnlyNext();
            m_project->edit(T("Spur-Pan"), [i, v](Timeline& tl) {
                if (i < tl.audio.size()) tl.audio[i].pan = v;
            }, "mixer-pan-" + QString::number(i));
        };
        s->fader->onFinish = s->pan->onFinish = [this] { m_project->closeMerge(); };
        connect(s->mute, &QToolButton::clicked, this, [this, i] {
            m_engine->mixerOnlyNext(); // live umschalten, kein Neuaufbau (sonst ruckelt die Wiedergabe)
            m_project->edit(T("Spur stumm"), [i](Timeline& tl) {
                if (i < tl.audio.size()) tl.audio[i].muted = !tl.audio[i].muted;
            });
        });
        connect(s->solo, &QToolButton::clicked, this, [this, i] {
            m_engine->mixerOnlyNext();
            m_project->edit(T("Spur solo"), [i](Timeline& tl) {
                if (i < tl.audio.size()) tl.audio[i].solo = !tl.audio[i].solo;
            });
        });
        m_stripLayout->insertWidget(i, s);
        m_strips << s;
    }
    m_stripScroll->setMaximumWidth(count * kStripW);
}

void Mixer::sync()
{
    const Timeline& tl = m_project->timeline();
    if (m_strips.size() != tl.audio.size()) rebuildStrips(tl.audio.size());
    const bool anySolo = std::any_of(tl.audio.begin(), tl.audio.end(), [](const Track& t) { return t.solo; });
    for (int i = 0; i < m_strips.size(); ++i) {
        const Track& t = tl.audio[i];
        ChannelStrip* s = m_strips[i];
        s->name->setText(trackShortName({TrackKind::Audio, i})); // wie DaVinci „A1“, Name als Tooltip
        s->name->setToolTip(trackDisplayName(t, {TrackKind::Audio, i}));
        s->setVolume(t.volumeDb);
        s->setPan(t.pan);
        s->mute->setChecked(t.muted); // setChecked löst kein clicked aus
        s->solo->setChecked(t.solo);
        // Durch Solo einer anderen Spur stumm: Name ausgrauen, damit man sieht, dass der Knopf wirkt
        const bool silenced = anySolo && !t.solo && !t.muted;
        s->name->setStyleSheet(QString("color: %1; font-weight: 600; background: %2; padding: 2px;")
                                   .arg((silenced ? Theme::textFaint : Theme::text).name(), Theme::panelHeader.name()));
        s->name->setToolTip(silenced ? T("%1 – stumm durch Solo einer anderen Spur").arg(s->name->toolTip())
                                     : s->name->toolTip());
    }
    m_master->setVolume(tl.masterVolumeDb);
    m_master->limiter->setChecked(tl.masterLimiter);
    m_master->limiter->setToolTip(T("Limiter am Master (Ceiling %1 dBFS)\nKlick = an/aus, Rechtsklick = Ceiling")
                                      .arg(tl.masterLimiterDb, 0, 'f', 1));
}

void Mixer::onLevels(const QVector<float>& db)
{
    if (db.size() != 2 * (m_strips.size() + 1)) return; // Spuranzahl ändert sich gerade
    for (int i = 0; i < m_strips.size(); ++i) m_strips[i]->fader->setLevel(db[2 * i], db[2 * i + 1]);
    m_master->fader->setLevel(db[db.size() - 2], db[db.size() - 1]);
    m_lastLevels.start();
}

void Mixer::tick()
{
    const double dt = m_clock.restart() / 1000.0;
    if (!isVisible()) return;
    // Keine Pegel mehr (Pause, Stopp, Quellmodus) -> Anzeige fällt ab
    const bool silent = !m_lastLevels.isValid() || m_lastLevels.elapsed() > 150;
    auto step = [&](ChannelStrip* s) {
        if (silent) s->fader->setLevel(kSilent, kSilent);
        s->fader->tick(dt);
    };
    for (ChannelStrip* s : m_strips) step(s);
    step(m_master);
    for (ChannelStrip* s : m_strips) s->updatePeak();
    m_master->updatePeak();
    m_loudness->view->setReading(m_engine->loudness(), !silent);
}
