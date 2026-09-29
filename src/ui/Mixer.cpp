#include "ui/Mixer.h"

#include "app/Theme.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "engine/Engine.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
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
    if (db >= -3) return QColor(0xe8, 0x41, 0x4a);  // rot
    if (db >= -12) return QColor(0xe0, 0xc0, 0x3a); // gelb
    return QColor(0x3c, 0xc0, 0x5a);                // grün
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
    }

    std::function<void(double)> onChange;
    std::function<void()> onFinish;

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

        // Pegel: zwei Balken
        const int mx = 3, bw = 5;
        for (int ch = 0; ch < 2; ++ch) {
            const QRect bar(mx + ch * (bw + 1), top, bw, h + 1);
            p.fillRect(bar, QColor(0x14, 0x14, 0x17));
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
            p.fillRect(mx, y, 2 * bw + 1, 1, QColor(0x14, 0x14, 0x17, 160));
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
        p.fillRect(fx - 1, top, 3, h + 1, QColor(0x14, 0x14, 0x17));
        const int ky = faderY(m_db);
        const QRect knob(fx - 9, ky - 6, 19, 12);
        p.setRenderHint(QPainter::Antialiasing, true);
        QLinearGradient g(knob.topLeft(), knob.bottomLeft());
        g.setColorAt(0, QColor(0x7a, 0x7a, 0x82));
        g.setColorAt(1, QColor(0x4a, 0x4a, 0x52));
        p.setPen(QColor(0x14, 0x14, 0x17));
        p.setBrush(g);
        p.drawRoundedRect(knob, 2, 2);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.fillRect(knob.left() + 3, ky, knob.width() - 6, 1, QColor(0xe8, 0xe8, 0xec));
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        m_dragging = true;
        m_pressY = e->position().y();
        m_startDb = m_db;
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!m_dragging) return;
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
    void mouseDoubleClickEvent(QMouseEvent*) override
    {
        m_dragging = false;
        setValue(0);
        if (onChange) onChange(0);
        if (onFinish) onFinish();
    }

private:
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
        p.setBrush(QColor(0x3a, 0x3a, 0x42));
        p.drawEllipse(r);
        // Bogen von der Mitte (oben) bis zum Wert, ±135°
        const double angle = m_value / 100.0 * 135.0;
        p.setPen(QPen(Theme::accent, 2.5, Qt::SolidLine, Qt::FlatCap));
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

// Ein Kanalzug: Name, Pan, M/S, Pegel+Fader, Wert. Beim Master bleiben Pan/M/S leer (gleiche Höhe).
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
        auto makeButton = [&](const QString& text, const QString& on, const QString& tip) {
            auto* b = new QToolButton;
            b->setText(text);
            b->setCheckable(true);
            b->setFocusPolicy(Qt::NoFocus);
            b->setToolTip(tip);
            b->setFixedSize(26, 18);
            b->setStyleSheet(QString("QToolButton { background: #3a3a42; color: %1; font-weight: 600; padding: 0; }"
                                     "QToolButton:checked { background: %2; color: #141417; }")
                                 .arg(Theme::text.name(), on));
            ms->addWidget(b);
            return b;
        };
        mute = makeButton("M", "#e8414a", T("Stumm (Mute)"));
        solo = makeButton("S", "#e0c03a", "Solo");
        lay->addLayout(ms);

        fader = new FaderMeter;
        lay->addWidget(fader, 1, Qt::AlignHCenter);
        value = new QLabel;
        value->setAlignment(Qt::AlignCenter);
        value->setStyleSheet(QString("color: %1; font-size: 8pt;").arg(Theme::text.name()));
        lay->addWidget(value);

        if (master) {
            for (QWidget* w : {static_cast<QWidget*>(pan), static_cast<QWidget*>(panLabel),
                               static_cast<QWidget*>(mute), static_cast<QWidget*>(solo)}) {
                QSizePolicy sp = w->sizePolicy();
                sp.setRetainSizeWhenHidden(true);
                w->setSizePolicy(sp);
                w->hide();
            }
        }
        setVolume(0);
        setPan(0);
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
    FaderMeter* fader;
    QLabel* value;
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
    row->addWidget(scroll, 1);

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
    row->addWidget(m_master);
    root->addLayout(row, 1);

    setMinimumWidth(kStripW * 2 + 20);

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
            m_project->edit(T("Spur stumm"), [i](Timeline& tl) {
                if (i < tl.audio.size()) tl.audio[i].muted = !tl.audio[i].muted;
            });
        });
        connect(s->solo, &QToolButton::clicked, this, [this, i] {
            m_project->edit(T("Spur solo"), [i](Timeline& tl) {
                if (i < tl.audio.size()) tl.audio[i].solo = !tl.audio[i].solo;
            });
        });
        m_stripLayout->insertWidget(i, s);
        m_strips << s;
    }
}

void Mixer::sync()
{
    const Timeline& tl = m_project->timeline();
    if (m_strips.size() != tl.audio.size()) rebuildStrips(tl.audio.size());
    for (int i = 0; i < m_strips.size(); ++i) {
        const Track& t = tl.audio[i];
        ChannelStrip* s = m_strips[i];
        s->name->setText(trackShortName({TrackKind::Audio, i})); // wie DaVinci „A1“, Name als Tooltip
        s->name->setToolTip(trackDisplayName(t, {TrackKind::Audio, i}));
        s->setVolume(t.volumeDb);
        s->setPan(t.pan);
        s->mute->setChecked(t.muted); // setChecked löst kein clicked aus
        s->solo->setChecked(t.solo);
    }
    m_master->setVolume(tl.masterVolumeDb);
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
}
