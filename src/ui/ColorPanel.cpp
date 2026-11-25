// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/ColorPanel.h"
#include "ui/touch/Touch.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/GradeCurves.h"
#include "core/GradeQualifier.h"
#include "core/GradeStills.h"
#include "core/GradeWindows.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "core/Timecode.h"
#include "engine/ColorGrade.h"
#include "engine/Tracker.h"
#include "ui/CurveEditor.h"
#include "ui/ElidedLabel.h"
#include "ui/FlowLayout.h"
#include "ui/ScrubField.h"
#include "ui/WindowOverlay.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QButtonGroup>
#include <QConicalGradient>
#include <QLinearGradient>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QSignalBlocker>
#include <QPushButton>
#include <QScrollArea>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QRadialGradient>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QThread>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <optional>

namespace {

// Color balance of a wheel as a point (u, v) in the plane, like a vectorscope (BT.709): right = blue, top = red.
// R/G/B changes derived from it have no brightness together -> balance changes the color, the master the brightness.
constexpr double kWr = 0.2126, kWg = 0.7152, kWb = 0.0722;

void uvToRgb(double u, double v, double rgb[3])
{
    rgb[0] = 1.5748 * v;
    rgb[1] = -0.1873 * u - 0.4681 * v;
    rgb[2] = 1.8556 * u;
}

void rgbToUv(const double d[3], double* u, double* v)
{
    const double y = kWr * d[0] + kWg * d[1] + kWb * d[2];
    *u = (d[2] - y) / 1.8556;
    *v = (d[0] - y) / 1.5748;
}

} // namespace

// Color wheel like DaVinci: drag the point = color balance (relative to the mouse movement, Shift = fine), double-click = center.
// Position (u, v) in -1..1 (Rand = 1).
class ColorWheel : public QWidget {
public:
    std::function<void(double u, double v)> onChange;
    std::function<void()> onFinish;

    explicit ColorWheel(QWidget* parent = nullptr) : QWidget(parent)
    {
        setFixedSize(112, 112);
        setCursor(Qt::CrossCursor);
        setProperty("touchDrags", true); // a finger drag moves the balance, never scrolls the page around it
        setToolTip(T("Ziehen = Farbbalance (Shift = fein), Doppelklick = zurücksetzen"));
    }
    void setPoint(double u, double v)
    {
        if (u == m_u && v == m_v) return;
        m_u = u;
        m_v = v;
        update();
    }

protected:
    QRectF disc() const
    {
        const double d = std::min(width(), height()) - 8;
        return QRectF((width() - d) / 2, (height() - d) / 2, d, d);
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = disc();
        const QPointF c = r.center();
        // Color circle: hue per direction like a vectorscope, gray towards the center
        QConicalGradient hue(c, 0);
        for (int i = 0; i <= 12; ++i) {
            const double a = i / 12.0 * 2 * M_PI;
            double rgb[3];
            uvToRgb(std::cos(a) * 0.5, std::sin(a) * 0.5, rgb);
            auto ch = [](double x) { return std::clamp(int(128 + x * 190), 0, 255); };
            hue.setColorAt(i / 12.0, QColor(ch(rgb[0]), ch(rgb[1]), ch(rgb[2])));
        }
        p.setPen(QPen(Theme::well, 2));
        p.setBrush(hue);
        p.drawEllipse(r);
        QRadialGradient fade(c, r.width() / 2);
        fade.setColorAt(0, Theme::alpha(Theme::panel, 255));
        fade.setColorAt(0.75, Theme::alpha(Theme::panel, 120));
        fade.setColorAt(1, Theme::alpha(Theme::panel, 40));
        p.setPen(Qt::NoPen);
        p.setBrush(fade);
        p.drawEllipse(r.adjusted(2, 2, -2, -2));
        // Crosshair
        p.setPen(QPen(QColor(255, 255, 255, 40), 1));
        p.drawLine(QPointF(c.x(), r.top() + 6), QPointF(c.x(), r.bottom() - 6));
        p.drawLine(QPointF(r.left() + 6, c.y()), QPointF(r.right() - 6, c.y()));
        // point
        const QPointF dot(c.x() + m_u * r.width() / 2, c.y() - m_v * r.height() / 2);
        const bool moved = std::abs(m_u) > 1e-6 || std::abs(m_v) > 1e-6;
        p.setPen(QPen(moved ? Theme::primary : Theme::text, 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(dot, 4, 4);
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        m_last = e->position();
        m_dragging = true;
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!m_dragging) return;
        const QPointF d = e->position() - m_last;
        m_last = e->position();
        const double scale = (e->modifiers() & Qt::ShiftModifier ? 0.15 : 1.0) / (disc().width() / 2);
        double u = m_u + d.x() * scale, v = m_v - d.y() * scale;
        const double len = std::hypot(u, v);
        if (len > 1) {
            u /= len;
            v /= len;
        }
        setPoint(u, v);
        if (onChange) onChange(u, v);
    }
    void mouseReleaseEvent(QMouseEvent*) override
    {
        if (!m_dragging) return;
        m_dragging = false;
        if (onFinish) onFinish();
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        setPoint(0, 0);
        if (onChange) onChange(0, 0);
        if (onFinish) onFinish();
    }

private:
    double m_u = 0, m_v = 0;
    QPointF m_last;
    bool m_dragging = false;
};

// Master wheel under a color wheel (like a thumb wheel): drag sideways = the wheel's Y (master) up or down, relative
// to the mouse movement (Shift = fine), double-click = back to neutral. The ridges move with the drag.
class MasterWheel : public QWidget {
public:
    std::function<void(double dx)> onDrag; // pixels moved (Shift already applied)
    std::function<void()> onFinish;
    std::function<void()> onReset;

    explicit MasterWheel(QWidget* parent = nullptr) : QWidget(parent)
    {
        setFixedSize(104, 14);
        setCursor(Qt::SizeHorCursor);
        setProperty("touchDrags", true); // (as the wheel)
        setToolTip(T("Master: zur Seite ziehen = heller/dunkler (Shift = fein), Doppelklick = zurücksetzen"));
    }
    void setActive(bool moved)
    {
        if (moved == m_moved) return;
        m_moved = moved;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QLinearGradient shade(r.topLeft(), r.topRight()); // a drum seen from the front: darker at the sides
        shade.setColorAt(0, Theme::well);
        shade.setColorAt(0.5, Theme::controlLight);
        shade.setColorAt(1, Theme::well);
        p.setPen(QPen(m_moved ? Theme::primary : Theme::border, 1));
        p.setBrush(shade);
        p.drawRoundedRect(r, 3, 3);
        // Ridges: evenly spaced on the drum, squeezed towards its edges
        p.setPen(QPen(Theme::alpha(Theme::text, 70), 1));
        constexpr double kSpacing = 0.22; // radians between ridges
        const double phase = std::fmod(m_phase / (r.width() / 2), kSpacing);
        for (double a = -M_PI / 2 + phase; a < M_PI / 2; a += kSpacing) {
            const double x = r.center().x() + std::sin(a) * (r.width() / 2 - 3);
            p.drawLine(QPointF(x, r.top() + 3), QPointF(x, r.bottom() - 3));
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        m_last = e->position().x();
        m_dragging = true;
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!m_dragging) return;
        const double dx = e->position().x() - m_last;
        m_last = e->position().x();
        m_phase += dx;
        update();
        if (onDrag) onDrag(dx * (e->modifiers() & Qt::ShiftModifier ? 0.15 : 1.0));
    }
    void mouseReleaseEvent(QMouseEvent*) override
    {
        if (!m_dragging) return;
        m_dragging = false;
        if (onFinish) onFinish();
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && onReset) onReset();
    }

private:
    double m_last = 0, m_phase = 0;
    bool m_dragging = false, m_moved = false;
};

// One range of the qualifier as a bar: what it measures (hue spectrum, saturation from gray to colour, brightness from
// black to white), darkened where the selection does not reach. Dragging changes the range: an edge (the white ticks)
// moves that edge, the hue bar's middle moves the whole hue range around the circle.
class QualifierBar : public QWidget {
public:
    enum Kind { Hue, Sat, Lum };
    std::function<void(const QString& key, double value)> onChange;
    std::function<void()> onFinish;

    explicit QualifierBar(Kind kind, QWidget* parent = nullptr) : QWidget(parent), m_kind(kind)
    {
        setMinimumWidth(90);
        setFixedHeight(16);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setCursor(Qt::SizeHorCursor);
        setProperty("touchDrags", true); // (as the wheel)
        setToolTip(kind == Hue ? T("Ränder ziehen = Breite, Mitte ziehen = Farbton verschieben")
                               : T("Ränder ziehen = Bereich ändern"));
    }
    void setRange(const GradeQualifier::Range& r)
    {
        if (r == m_range) return;
        m_range = r;
        update();
    }
    double scale() const { return m_kind == Hue ? 360.0 : 100.0; }
    QRect bar() const { return rect().adjusted(0, 2, 0, -2); }
    double toValue(double x) const { return (x - bar().left()) / std::max(1, bar().width() - 1) * scale(); }
    double toX(double v) const { return bar().left() + v / scale() * (bar().width() - 1); }
    // The edges as values (hue: the ends of the fully selected width, wrapped)
    QVector<double> edges() const
    {
        if (m_kind == Hue) {
            if (m_range.hueWidth >= 360) return {};
            const auto wrap = [](double h) { return std::fmod(std::fmod(h, 360.0) + 360.0, 360.0); };
            return {wrap(m_range.hue - m_range.hueWidth / 2), wrap(m_range.hue + m_range.hueWidth / 2)};
        }
        return m_kind == Sat ? QVector<double>{m_range.satLow, m_range.satHigh}
                             : QVector<double>{m_range.lumLow, m_range.lumHigh};
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const QRect r = bar();
        QLinearGradient g(r.topLeft(), r.topRight());
        if (m_kind == Hue) {
            for (int i = 0; i <= 6; ++i) g.setColorAt(i / 6.0, QColor::fromHsvF(std::fmod(i / 6.0, 1.0), 0.8, 0.9));
        } else if (m_kind == Sat) {
            g.setColorAt(0, QColor(128, 128, 128));
            g.setColorAt(1, QColor::fromHsvF(std::fmod(m_range.hue / 360.0 + 1, 1.0), 1.0, 0.9));
        } else {
            g.setColorAt(0, Qt::black);
            g.setColorAt(1, Qt::white);
        }
        p.fillRect(r, g);
        // Not selected: dark
        for (int x = r.left(); x <= r.right(); ++x) {
            const double v = toValue(x);
            const float w = m_kind == Hue   ? GradeQualifier::hueWeight(m_range, v)
                            : m_kind == Sat ? GradeQualifier::band(v, m_range.satLow, m_range.satHigh, m_range.satSoft)
                                            : GradeQualifier::band(v, m_range.lumLow, m_range.lumHigh, m_range.lumSoft);
            p.setPen(QColor(0, 0, 0, int((1 - w) * 200)));
            p.drawLine(x, r.top(), x, r.bottom());
        }
        p.setPen(Theme::border);
        p.setBrush(Qt::NoBrush);
        p.drawRect(r.adjusted(0, 0, -1, -1));
        // Edge ticks
        for (double e : edges()) {
            const double x = toX(e);
            p.setPen(QPen(QColor(0, 0, 0, 160), 3));
            p.drawLine(QPointF(x, 0), QPointF(x, height()));
            p.setPen(QPen(QColor(255, 255, 255, 230), 1.2));
            p.drawLine(QPointF(x, 0), QPointF(x, height()));
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const QVector<double> ed = edges();
        m_edge = -1;
        double best = 7;
        for (int i = 0; i < ed.size(); ++i)
            if (const double d = std::abs(toX(ed[i]) - e->position().x()); d <= best) {
                best = d;
                m_edge = i;
            }
        // Both ends on one spot: which one moves is decided by the first move (left = low end, right = high end)
        if (ed.size() == 2 && m_edge >= 0 && std::abs(toX(ed[0]) - toX(ed[1])) < 3) m_edge = -2;
        if (m_edge == -1 && m_kind != Hue) { // a range: the nearer edge jumps here
            const double v = toValue(e->position().x());
            m_edge = ed.size() == 2 && std::abs(v - ed[0]) <= std::abs(v - ed[1]) ? 0 : 1;
        }
        m_pressValue = toValue(e->position().x());
        m_pressX = e->position().x();
        m_startHue = m_range.hue;
        m_dragging = true;
        if (m_edge != -2) mouseMoveEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!m_dragging || !onChange) return;
        if (m_edge == -2) {
            if (std::abs(e->position().x() - m_pressX) < 1) return;
            m_edge = e->position().x() < m_pressX ? 0 : 1;
        }
        const double v = std::clamp(toValue(e->position().x()), 0.0, scale());
        if (m_kind == Hue) {
            if (m_edge < 0) { // move the range around the circle
                const double h = std::fmod(std::fmod(m_startHue + v - m_pressValue, 360.0) + 360.0, 360.0);
                onChange("hue", h);
            } else { // width: twice the distance from the middle
                double d = std::fmod(std::abs(v - m_range.hue), 360.0);
                if (d > 180) d = 360 - d;
                onChange("hueWidth", std::clamp(2 * d, 0.0, 359.0));
            }
            return;
        }
        const bool sat = m_kind == Sat;
        const double lo = sat ? m_range.satLow : m_range.lumLow, hi = sat ? m_range.satHigh : m_range.lumHigh;
        if (m_edge == 0) onChange(sat ? "satLow" : "lumLow", std::min(v, hi));
        else onChange(sat ? "satHigh" : "lumHigh", std::max(v, lo));
    }
    void mouseReleaseEvent(QMouseEvent*) override
    {
        if (!m_dragging) return;
        m_dragging = false;
        if (onFinish) onFinish();
    }

private:
    Kind m_kind;
    GradeQualifier::Range m_range;
    bool m_dragging = false;
    int m_edge = -1; // dragged edge (0 low, 1 high), -1 = the hue range as a whole, -2 = by the first move
    double m_pressValue = 0, m_startHue = 0, m_pressX = 0;
};

ColorPanel::ColorPanel(Editor* editor, QWidget* parent) : QWidget(parent), m_editor(editor)
{
    setObjectName("Panel");
    using P = AnimParam;
    m_wheels = {
        {N_("Tiefen"), {P::GradeLiftY, P::GradeLiftR, P::GradeLiftG, P::GradeLiftB}, 0.0, 0.25},
        {N_("Mitten"), {P::GradeGammaY, P::GradeGammaR, P::GradeGammaG, P::GradeGammaB}, 0.0, 0.25},
        {N_("Lichter"), {P::GradeGainY, P::GradeGainR, P::GradeGainG, P::GradeGainB}, 1.0, 0.5},
        {N_("Versatz"), {P::GradeOffsetY, P::GradeOffsetR, P::GradeOffsetG, P::GradeOffsetB}, 25.0, 25.0},
    };

    m_overlay = new WindowOverlay(m_editor, [this] { return targets(); }, this);

    // Header row: title, clip name, on/off, keyframes, reset all
    auto* title = new QLabel(T("Farbkorrektur"));
    title->setObjectName("PanelTitle");
    m_title = title;
    m_clipName = new ElidedLabel; // a long name is cut ("…"), it never widens the page
    m_clipName->setMinimumWidth(20);
    m_clipName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_clipName->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    m_enabled = new QToolButton;
    m_enabled->setCheckable(true);
    // The dot: touch mode's minimum button size (a style sheet rule) must not stretch it into a bar
    const int dot = Touch::modeActive() ? 20 : 10;
    m_enabled->setFixedSize(dot, dot);
    m_enabled->setFocusPolicy(Qt::NoFocus);
    m_enabled->setToolTip(T("Farbkorrektur des Clips an/aus"));
    m_enabled->setStyleSheet(QString("QToolButton { border: none; border-radius: %3px; background: %1; padding: 0;"
                                     " min-width: %4px; max-width: %4px; min-height: %4px; max-height: %4px; }"
                                     "QToolButton:checked { background: %2; }")
                                 .arg(Theme::controlOff.name(), Theme::warning.name())
                                 .arg(dot / 2)
                                 .arg(dot));
    connect(m_enabled, &QToolButton::clicked, this, [this](bool on) { m_editor->setGradeEnabled(targets(), on); });
    m_keyPrev = smallButton("◀", T("Voriger Keyframe"));
    m_keyDiamond = smallButton("◇", T("Keyframe für die ganze Korrektur setzen/entfernen"));
    m_keyNext = smallButton("▶", T("Nächster Keyframe"));
    auto jump = [this](bool forward) {
        const Clip* c = current();
        if (!c) return;
        const int t = localFrame(*c);
        std::optional<int> best;
        for (int k : Keys::keyTimes(*c, layerParams())) {
            if (k < 0 || k >= c->length()) continue;
            if (forward ? k > t && (!best || k < *best) : k < t && (!best || k > *best)) best = k;
        }
        if (best) emit seekRequested(c->start + *best);
    };
    connect(m_keyPrev, &QToolButton::clicked, this, [jump] { jump(false); });
    connect(m_keyNext, &QToolButton::clicked, this, [jump] { jump(true); });
    connect(m_keyDiamond, &QToolButton::clicked, this, [this] {
        const Clip* c = current();
        if (!c) return;
        const bool here = Keys::keyAt(*c, mapped(AnimParam::GradeLiftY), localFrame(*c));
        m_editor->setGradeKeyframe(targets(), m_playhead, !here, m_layer);
    });
    auto* resetAll = smallButton("↺", T("Farbkorrektur zurücksetzen (alles, auch LUT, Fenster und Keyframes)"));
    connect(resetAll, &QToolButton::clicked, this, [this] {
        if (m_layer == GradeQualifier::kLayer) // qualifier: only its correction, the range stays
            m_editor->resetGrade(targets(), ColorGrade::qualifierParams(), m_playhead, T("Qualifier-Korrektur zurücksetzen"));
        else if (m_layer > 0) // window: only its correction, the shape stays
            m_editor->resetGrade(targets(), ColorGrade::windowParams(m_layer), m_playhead, T("Fenster-Korrektur zurücksetzen"));
        else
            m_editor->resetGrade(targets(), {}, m_playhead, T("Farbkorrektur zurücksetzen"));
    });

    // Palettes: wheels, curves, qualifier (like the palette buttons of other graders, as tabs in the header)
    auto* paletteGroup = new QButtonGroup(this);
    auto* palettes = new QHBoxLayout;
    palettes->setSpacing(2);
    const QPair<QString, QString> paletteNames[] = {
        {T("Räder"), T("Farbräder, Master-Räder und Regler")},
        {T("Kurven"), T("Tonkurven (Master, R, G, B) und Farbton-/Sättigungskurven")},
        {T("Qualifier"), T("Farben auswählen (z. B. Haut, Himmel) und nur sie korrigieren")},
        {T("Standbilder"), T("Standbilder mit ihrer Korrektur: vergleichen und auf andere Clips übertragen")},
    };
    for (int i = 0; i < 4; ++i) {
        auto* b = new QToolButton;
        b->setText(paletteNames[i].first);
        b->setToolTip(paletteNames[i].second);
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setObjectName(QString("color_palette_%1").arg(i));
        paletteGroup->addButton(b, i);
        connect(b, &QToolButton::clicked, this, [this, i] { setPalette(i); });
        m_paletteButtons << b;
        palettes->addWidget(b);
    }

    auto* head = new QHBoxLayout;
    head->setContentsMargins(0, 0, 8, 0);
    head->setSpacing(6);
    head->addWidget(title);
    head->addSpacing(8);
    head->addLayout(palettes);
    head->addSpacing(8);
    head->addWidget(m_clipName, 1);
    head->addWidget(m_enabled);
    head->addSpacing(6);
    head->addWidget(m_keyPrev);
    head->addWidget(m_keyDiamond);
    head->addWidget(m_keyNext);
    head->addSpacing(6);
    head->addWidget(resetAll);

    // Sliders below the wheels (like the bar under the DaVinci wheels), two under each wheel, the LUT under Offset
    m_sliders = {{P::GradeContrast}, {P::GradePivot}, {P::GradeSaturation},
                 {P::GradeTemp},     {P::GradeTint},  {P::GradeExposure}};
    const QString sliderNames[] = {T("Kontrast"), T("Mittelpunkt"), T("Sättigung"), T("Temperatur"), T("Tönung"), T("Belichtung")};
    const double ranges[][4] = {{0, 2, 0.002, 3}, {0, 1, 0.001, 3}, {0, 100, 0.1, 2},
                                {-4000, 4000, 5, 1}, {-100, 100, 0.1, 2}, {-4, 4, 0.01, 2}};

    // LUT: label above like the sliders, the file name with its buttons below
    auto* lutBox = new QWidget;
    lutBox->setProperty("tourTarget", "color_lut"); // the in-app tours point at it (app/TourManager)
    auto* lutLay = new QVBoxLayout(lutBox);
    lutLay->setContentsMargins(0, 0, 0, 0);
    lutLay->setSpacing(1);
    auto* lutLabel = new QLabel(T("LUT"));
    lutLabel->setStyleSheet(QString("color: %1; font-size: 8pt;").arg(Theme::textDim.name()));
    lutLabel->setAlignment(Qt::AlignCenter);
    m_lutName = new ElidedLabel; // a long file name ends in "…" instead of widening Offset's column
    m_lutName->setMaximumWidth(110);
    // "None" always whole (it was squeezed to "No…" beside the button): only a file name gets cut
    m_lutName->setMinimumWidth(m_lutName->fontMetrics().horizontalAdvance(T("Keine")) + 2);
    auto* lutLoad = new QToolButton;
    lutLoad->setText(T("Laden…"));
    lutLoad->setToolTip(T("LUT (.cube, .3dl, .csp, Hald-CLUT) für diesen Clip laden, wird nach der Korrektur angewendet"));
    connect(lutLoad, &QToolButton::clicked, this, &ColorPanel::loadLut);
    m_lutClear = smallButton("✕", T("LUT entfernen"));
    connect(m_lutClear, &QToolButton::clicked, this, [this] { m_editor->setGradeLut(targets(), {}); });
    auto* lutRow = new QHBoxLayout;
    lutRow->setSpacing(4);
    lutRow->addWidget(m_lutName);
    lutRow->addWidget(lutLoad);
    lutRow->addWidget(m_lutClear);
    lutLay->addWidget(lutLabel);
    lutLay->addLayout(lutRow);

    // One grid: the wheels spread over the width (the first at the left edge, the last at the right one, even gaps
    // in the stretching columns between them), each wheel's sliders centered in its column below
    // A narrow page (a small window, the scopes beside it, a large font) puts them in two rows of two (layoutWheels)
    auto* grid = new QGridLayout;
    m_wheelGrid = grid;
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(10);
    for (int i = 0; i < m_wheels.size(); ++i) {
        QWidget* wheel = buildWheel(m_wheels[i]);
        wheel->setProperty("tourTarget", QString("color_wheel_%1").arg(i)); // tours: 0 Lift .. 3 Offset
        m_wheelBoxes << wheel;
        auto* underBox = new QWidget;
        m_wheelUnders << underBox;
        auto* under = new QHBoxLayout(underBox);
        under->setContentsMargins(0, 0, 0, 0);
        under->setSpacing(14);
        if (2 * i + 1 < m_sliders.size()) {
            for (const int k : {2 * i, 2 * i + 1}) {
                QWidget* slider = buildSlider(m_sliders[k], sliderNames[k], ranges[k][0], ranges[k][1], ranges[k][2],
                                              int(ranges[k][3]));
                slider->setProperty("tourTarget", QString("color_slider_%1").arg(k)); // tours: 2 saturation, 5 exposure
                under->addWidget(slider);
            }
        } else {
            under->addWidget(lutBox);
        }
    }
    layoutWheels(4);

    m_body = new QWidget;
    auto* outer = new QBoxLayout(QBoxLayout::LeftToRight, m_body); // windows below the palettes on a narrow page
    m_outer = outer;
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(10);
    auto* wheels = new QWidget;
    auto* bl = new QVBoxLayout(wheels);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->addLayout(grid);
    bl->addStretch(1);
    m_palettes = new QStackedWidget;
    m_palettes->addWidget(wheels);
    m_palettes->addWidget(buildCurves());
    m_palettes->addWidget(buildQualifier());
    m_palettes->addWidget(buildStills());
    outer->addWidget(m_palettes, 1);
    // The windows as a section of their own beside the wheels
    auto* line = new QFrame;
    line->setFrameShape(QFrame::VLine);
    line->setFixedWidth(1);
    m_windowsLine = line;
    line->setStyleSheet(QString("background: %1; border: none;").arg(Theme::border.name()));
    outer->addSpacing(6);
    outer->addWidget(line);
    m_windowsBox = buildWindows();
    outer->addWidget(m_windowsBox);

    // No clip: what to do, and the Color tour (like the empty graph on the Comp page)
    m_empty = new QWidget;
    auto* emptyLay = new QVBoxLayout(m_empty);
    emptyLay->addStretch(1);
    auto* emptyText = new QLabel(T("Kein Videoclip ausgewählt oder am Abspielkopf"));
    emptyText->setAlignment(Qt::AlignCenter);
    emptyText->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    emptyLay->addWidget(emptyText);
    m_tourButton = new QPushButton(T("Rundgang Farbe starten"));
    m_tourButton->setObjectName("color_tour_button");
    m_tourButton->setFocusPolicy(Qt::NoFocus);
    connect(m_tourButton, &QPushButton::clicked, this, &ColorPanel::tourRequested);
    emptyLay->addWidget(m_tourButton, 0, Qt::AlignHCenter);
    emptyLay->addStretch(1);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addLayout(head);
    // Scrolls when the page gives it little height: its controls (a window's fields, the wheels in long languages) never
    // set a minimum height for the whole window (it grew past the screen, the page bar went off it)
    auto* scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setWidget(m_body);
    scroll->setMinimumHeight(60);
    m_bodyScroll = scroll;
    lay->addWidget(scroll, 1);
    lay->addWidget(m_empty, 1);

    Project* project = m_editor->project();
    connect(project, &Project::timelineChanged, this, &ColorPanel::refresh);
    connect(m_editor->selection(), &Selection::changed, this, &ColorPanel::refresh);
    connect(project, &Project::stillsChanged, this, &ColorPanel::refreshStills);
    connect(project, &Project::loaded, this, [this] { // another project: its stills have their own ids
        m_stillIcons.clear();
        setComparedStill(0);
        m_stillList->clear();
        refreshStills();
    });
    refreshStills();
    setPalette(std::clamp(QSettings().value("color/palette", int(Wheels)).toInt(), 0, int(StillsPalette)));
}

void ColorPanel::layoutWheels(int columns)
{
    if (columns == m_wheelColumns || !m_wheelGrid) return;
    m_wheelColumns = columns;
    for (QWidget* w : m_wheelBoxes + m_wheelUnders) m_wheelGrid->removeWidget(w);
    for (int c = 0; c < 8; ++c) m_wheelGrid->setColumnStretch(c, 0);
    for (int i = 0; i < m_wheelBoxes.size(); ++i) {
        const int col = 2 * (i % columns), row = 2 * (i / columns);
        if (col) m_wheelGrid->setColumnStretch(col - 1, 1);
        m_wheelGrid->addWidget(m_wheelBoxes[i], row, col);
        m_wheelGrid->addWidget(m_wheelUnders[i], row + 1, col, Qt::AlignHCenter);
    }
}

void ColorPanel::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    fitWidth();
}

void ColorPanel::fitWidth()
{
    if (!m_wheelGrid || !m_bodyScroll) return;
    // The widths the wheels need in one row and in two (measured in that layout; until then the other one halved)
    if (m_wheelColumns == 4) m_fourWide = m_wheelGrid->minimumSize().width();
    if (m_wheelColumns == 2) m_twoWide = m_wheelGrid->minimumSize().width();
    const int four = m_fourWide, two = m_twoWide ? m_twoWide : four / 2 + 6;
    // Every palette: the widest page decides (the stack is as wide as its widest page)
    const auto others = [this] {
        int w = 0;
        for (int i = 1; i < m_palettes->count(); ++i) w = std::max(w, m_palettes->widget(i)->minimumSizeHint().width());
        return w;
    }();
    const int all = width() - 40, beside = all - m_windowsBox->width() - 10; // margins, line, spacing
    // Beside the windows if the wheels fit there (4 in a row, else 2 x 2), else the windows go below
    int columns = 4;
    bool below = false;
    if (beside >= std::max(four, others)) columns = 4;
    else if (beside >= std::max(two, others)) columns = 2;
    else {
        below = true;
        columns = all >= four ? 4 : 2;
    }
    layoutWheels(columns);
    const auto dir = below ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
    if (m_outer->direction() != dir) {
        m_outer->setDirection(dir);
        m_windowsLine->setVisible(!below);
        m_outer->setAlignment(m_windowsBox, below ? Qt::AlignLeft : Qt::Alignment());
    }
}

void ColorPanel::setPalette(int palette, bool remember)
{
    m_palette = std::clamp(palette, 0, int(StillsPalette));
    QSettings settings; // only a change is written (a tour must leave the settings as they were)
    if (remember && settings.value("color/palette", int(Wheels)).toInt() != m_palette) settings.setValue("color/palette", m_palette);
    m_palettes->setCurrentIndex(m_palette);
    if (m_palette == CurvesPalette && !m_histFrame.isNull()) m_histTimer.start(); // the frame shown meanwhile
    for (int i = 0; i < m_paletteButtons.size(); ++i) m_paletteButtons[i]->setChecked(i == m_palette);
    refresh();
}

ColorPanel::~ColorPanel()
{
    if (m_trackThread) {
        m_trackCancel->store(true);
        m_trackThread->wait();
    }
}

QToolButton* ColorPanel::smallButton(const QString& text, const QString& tip)
{
    auto* b = new QToolButton;
    b->setText(text);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::NoFocus);
    b->setStyleSheet(QString("QToolButton { color: %1; border: none; padding: 0 3px; }"
                             "QToolButton:disabled { color: %2; }").arg(Theme::textDim.name(), Theme::textFaint.name()));
    return b;
}

QWidget* ColorPanel::buildWheel(Wheel& w)
{
    auto* box = new QWidget;
    auto* lay = new QVBoxLayout(box);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(4);

    auto* head = new QHBoxLayout;
    auto* name = new QLabel(T(w.name));
    name->setStyleSheet(QString("color: %1;").arg(Theme::text.name()));
    auto* reset = smallButton("↺", T("%1 zurücksetzen").arg(T(w.name)));
    const QVector<AnimParam> params{w.p[0], w.p[1], w.p[2], w.p[3]};
    connect(reset, &QToolButton::clicked, this, [this, params, n = T(w.name)] {
        QVector<AnimParam> list;
        for (AnimParam p : params) list << mapped(p);
        m_editor->resetGrade(targets(), list, m_playhead, T("%1 zurücksetzen").arg(n));
    });
    head->addWidget(name);
    head->addStretch(1);
    head->addWidget(reset);
    lay->addLayout(head);

    w.wheel = new ColorWheel;
    lay->addWidget(w.wheel, 0, Qt::AlignHCenter);
    const int index = int(&w - m_wheels.data());
    w.wheel->onChange = [this, index](double u, double v) {
        const Wheel& w = m_wheels[index];
        const Clip* c = current();
        if (!c) return;
        // brightness part of the R/G/B values stays, only the balance follows the point
        const int t = localFrame(*c);
        const AnimParam p[3] = {mapped(w.p[1]), mapped(w.p[2]), mapped(w.p[3])};
        double d[3];
        for (int i = 0; i < 3; ++i) d[i] = Keys::valueAt(*c, p[i], t) - w.neutral;
        const double y = kWr * d[0] + kWg * d[1] + kWb * d[2];
        double rgb[3];
        uvToRgb(u * w.range, v * w.range, rgb);
        setValues({{p[0], w.neutral + y + rgb[0]}, {p[1], w.neutral + y + rgb[1]}, {p[2], w.neutral + y + rgb[2]}},
                  T(w.name), QString("color:wheel:%1:%2").arg(w.name).arg(m_layer));
    };
    w.wheel->onFinish = [this] { finish(); };

    // Master wheel: Y of this wheel (brightness of its range), one step of the field per pixel
    w.master = new MasterWheel;
    lay->addWidget(w.master, 0, Qt::AlignHCenter);
    w.master->onDrag = [this, index](double dx) {
        const Wheel& w = m_wheels[index];
        const Clip* c = current();
        if (!c) return;
        const EffectParam* ep = nullptr;
        EffectRegistry::paramFor(w.p[0], nullptr, &ep);
        const AnimParam p = mapped(w.p[0]);
        const double step = ep ? ep->step : 0.002;
        double v = Keys::valueAt(*c, p, localFrame(*c)) + dx * step;
        if (ep) v = std::clamp(v, ep->min, ep->max);
        setValues({{p, v}}, T(w.name), QString("color:master:%1:%2").arg(w.name).arg(m_layer));
    };
    w.master->onFinish = [this] { finish(); };
    w.master->onReset = [this, index] {
        const Wheel& w = m_wheels[index];
        m_editor->resetGrade(targets(), {mapped(w.p[0])}, m_playhead, T("%1 zurücksetzen").arg(T(w.name)));
    };

    // Number fields Y R G B (Y = master, like the wheel under the DaVinci color wheels)
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(3);
    grid->setVerticalSpacing(0);
    const char* labels[4] = {"Y", "R", "G", "B"};
    const QColor colors[4] = {Theme::text, QColor(0xe8, 0x5a, 0x5a), QColor(0x5a, 0xc8, 0x6a), QColor(0x5a, 0x8a, 0xe8)};
    const EffectParam* ep = nullptr;
    EffectRegistry::paramFor(w.p[0], nullptr, &ep);
    for (int i = 0; i < 4; ++i) {
        auto* l = new QLabel(labels[i]);
        l->setAlignment(Qt::AlignCenter);
        l->setStyleSheet(QString("color: %1; font-size: 8pt;").arg(colors[i].name()));
        const double min = ep ? ep->min : -1, max = ep ? ep->max : 1;
        auto* f = new ScrubField(min, max, ep ? ep->step : 0.01, ep ? ep->decimals : 2);
        f->setShrinkable(true); // narrow with a large font: four of them under a wheel
        f->setValue(w.neutral);
        const AnimParam p = w.p[i];
        f->onChange = [this, p, n = T(w.name)](double v) {
            setValues({{mapped(p), v}}, n, QString("color:%1").arg(int(mapped(p))));
        };
        f->onFinish = [this] { finish(); };
        w.field[i] = f;
        grid->addWidget(l, 0, i);
        grid->addWidget(f, 1, i);
    }
    lay->addLayout(grid);
    return box;
}

QWidget* ColorPanel::buildSlider(Slider& s, const QString& label, double min, double max, double step, int decimals)
{
    auto* box = new QWidget;
    auto* lay = new QVBoxLayout(box);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(1);
    auto* l = new QLabel(label);
    l->setStyleSheet(QString("color: %1; font-size: 8pt;").arg(Theme::textDim.name()));
    l->setAlignment(Qt::AlignCenter);
    s.field = new ScrubField(min, max, step, decimals);
    s.field->setShrinkable(true);
    const AnimParam p = s.p;
    s.field->onChange = [this, p, label](double v) {
        setValues({{mapped(p), v}}, label, QString("color:%1").arg(int(mapped(p))));
    };
    s.field->onFinish = [this] { finish(); };
    lay->addWidget(l);
    lay->addWidget(s.field, 0, Qt::AlignHCenter);
    // Double-click on the label = default value (like DaVinci)
    l->setToolTip(T("Doppelklick = zurücksetzen"));
    l->installEventFilter(this);
    l->setProperty("gradeParam", int(p));
    return box;
}

bool ColorPanel::copyGrade()
{
    const Clip* c = current();
    return c && m_editor->copyGrade(c->id);
}

bool ColorPanel::pasteGrade()
{
    const QVector<int> ids = targets();
    if (!m_editor->hasCopiedGrade() || ids.isEmpty()) return false;
    m_editor->pasteGrade(ids);
    return true;
}

bool ColorPanel::gradeFromPrevious(int back)
{
    const Clip* c = current();
    return c && m_editor->applyGradeFromPrevious(c->id, back);
}

QVector<int> ColorPanel::targets() const { return m_editor->effectTargets(m_playhead); }

const Clip* ColorPanel::current() const
{
    const Timeline& tl = m_editor->project()->timeline();
    const Clip* best = nullptr;
    for (int id : targets()) {
        const Clip* c = TimelineOps::findClip(tl, id);
        if (c && (!best || c->start < best->start)) best = c;
    }
    return best;
}

int ColorPanel::localFrame(const Clip& c) const
{
    return std::clamp(m_playhead - c.start, 0, std::max(0, c.length() - 1));
}

void ColorPanel::setValues(const QVector<QPair<AnimParam, double>>& values, const QString& text, const QString& key)
{
    m_editor->setGradeValues(targets(), values, m_playhead, text, key);
}

void ColorPanel::finish() { m_editor->project()->closeMerge(); }

void ColorPanel::loadLut()
{
    if (targets().isEmpty()) return;
    QSettings settings;
    QString dir = settings.value("color/lutDir").toString();
    if (dir.isEmpty() || !QFileInfo(dir).isDir()) dir = EffectFolders::lutDir(); // own LUTs folder
    const QString path = QFileDialog::getOpenFileName(this, T("LUT laden"), dir, EffectFolders::lutFileFilter());
    if (path.isEmpty()) return;
    settings.setValue("color/lutDir", QFileInfo(path).absolutePath());
    QString error;
    ColorGrade::revalidateLuts(); // the file as it is now (one just overwritten, one that failed before)
    if (!ColorGrade::loadLut(path, &error)) {
        QMessageBox::warning(this, T("LUT laden"), T("Die LUT-Datei lässt sich nicht lesen:\n%1").arg(error));
        return;
    }
    m_editor->setGradeLut(targets(), path);
}

void ColorPanel::setPlayhead(int frame)
{
    if (frame == m_playhead) return;
    m_playhead = frame;
    if (isVisible()) refresh();
}

void ColorPanel::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    // The palette tabs never cut to "W…s" (a narrow panel scrolls instead): a style sheet's min-width (touch mode)
    // is an explicit minimum that wins over the text's own size, so their minimum is set from it once polished
    for (QToolButton* b : m_paletteButtons) b->setMinimumWidth(b->sizeHint().width());
    m_overlay->setVisible(true);
    fitWidth();
    refresh();
}

void ColorPanel::hideEvent(QHideEvent* e)
{
    QWidget::hideEvent(e);
    m_overlay->setVisible(false);
    updateMask(); // the mask only while the Color page shows
}

bool ColorPanel::eventFilter(QObject* obj, QEvent* e)
{
    if (e->type() == QEvent::MouseButtonDblClick && obj->property("gradeParam").isValid()) {
        const auto p = mapped(AnimParam(obj->property("gradeParam").toInt()));
        m_editor->resetGrade(targets(), {p}, m_playhead, T("%1 zurücksetzen").arg(static_cast<QLabel*>(obj)->text()));
        return true;
    }
    return QWidget::eventFilter(obj, e);
}

void ColorPanel::refresh()
{
    const Clip* c = current();
    refreshStills();
    m_bodyScroll->setVisible(c);
    m_empty->setVisible(!c);
    for (QWidget* w : {static_cast<QWidget*>(m_enabled), static_cast<QWidget*>(m_keyPrev),
                       static_cast<QWidget*>(m_keyDiamond), static_cast<QWidget*>(m_keyNext)})
        w->setEnabled(c);
    if (!c) {
        static_cast<ElidedLabel*>(m_clipName)->setFullText(QString());
        m_overlay->setTarget(0, 0, m_playhead);
        updateMask();
        return;
    }
    if (m_layer == GradeQualifier::kLayer ? !GradeQualifier::exists(*c) : m_layer > 0 && !GradeWindows::exists(*c, m_layer))
        m_layer = 0; // window or qualifier deleted (or other clip)
    const int n = targets().size();
    {
        const QString cn = m_editor->project()->clipName(*c);
        static_cast<ElidedLabel*>(m_clipName)->setFullText(n > 1 ? T("%1 (+%2 weitere)").arg(cn).arg(n - 1) : cn);
    }
    const bool has = EffectRegistry::has(*c, ColorGrade::EffectId);
    const EffectInstance* inst = EffectRegistry::instance(*c, ColorGrade::EffectId);
    m_enabled->setChecked(!inst || inst->enabled);
    m_enabled->setEnabled(has);

    const int t = localFrame(*c);
    // without correction: default values
    auto value = [&](AnimParam p) { return Keys::valueAt(*c, mapped(p), t); };
    for (Wheel& w : m_wheels) {
        for (int i = 0; i < 4; ++i)
            if (!w.field[i]->hasFocus()) w.field[i]->setValue(value(w.p[i]));
        double d[3];
        for (int i = 0; i < 3; ++i) d[i] = value(w.p[i + 1]) - w.neutral;
        double u, v;
        rgbToUv(d, &u, &v);
        w.wheel->setPoint(std::clamp(u / w.range, -1.0, 1.0), std::clamp(v / w.range, -1.0, 1.0));
        w.master->setActive(std::abs(value(w.p[0]) - w.neutral) > 1e-9);
    }
    for (Slider& s : m_sliders)
        if (!s.field->hasFocus()) s.field->setValue(value(s.p));

    const QString lut = ColorGrade::lutPath(*c);
    const bool lutOk = lut.isEmpty() || QFileInfo::exists(lut);
    m_lutName->setFullText(lut.isEmpty() ? T("Keine") : EffectFolders::displayName(lut) + (lutOk ? QString() : T(" (fehlt)")));
    m_lutName->setToolTip(lut);
    m_lutName->setStyleSheet(QString("color: %1;").arg((lutOk ? Theme::text : Theme::warning).name()));
    m_lutClear->setEnabled(!lut.isEmpty());

    // Keyframe diamond: red = keyframe at the playhead, bright = correction animated
    const QVector<AnimParam> params = layerParams();
    const bool animated = std::any_of(params.begin(), params.end(), [&](AnimParam p) { return Keys::animated(*c, p); });
    const bool here = animated && Keys::keyAt(*c, mapped(AnimParam::GradeLiftY), t);
    m_keyDiamond->setText(here ? "◆" : "◇");
    m_keyDiamond->setStyleSheet(QString("QToolButton { color: %1; border: none; padding: 0 3px; font-size: 11pt; }")
                                    .arg((here ? Theme::warning : animated ? Theme::text : Theme::textDim).name()));
    bool before = false, after = false;
    for (int k : Keys::keyTimes(*c, params)) {
        if (k < 0 || k >= c->length()) continue;
        before |= k < t;
        after |= k > t;
    }
    m_keyPrev->setEnabled(before);
    m_keyNext->setEnabled(after);
    refreshWindows(c, t);
    refreshCurves(c);
    refreshQualifier(c, t);
    updateMask();
}

// ---- Windows ----

ViewerOverlay* ColorPanel::overlay() const { return m_overlay; }

void ColorPanel::setLayer(int layer)
{
    const Clip* c = current();
    if (layer == GradeQualifier::kLayer) {
        if (!c || !GradeQualifier::exists(*c)) layer = 0;
    } else if (layer < 0 || layer > GradeWindows::kMax || (layer > 0 && (!c || !GradeWindows::exists(*c, layer)))) {
        layer = 0;
    }
    m_layer = layer;
    refresh();
}

AnimParam ColorPanel::mapped(AnimParam p) const
{
    if (m_layer <= 0) return p;
    const QVector<AnimParam>& base = ColorGrade::animParams();
    const int i = int(base.indexOf(p));
    if (i < 0) return p;
    return m_layer == GradeQualifier::kLayer ? ColorGrade::qualifierParams()[i] : ColorGrade::windowParams(m_layer)[i];
}

QVector<AnimParam> ColorPanel::layerParams() const
{
    if (m_layer <= 0) return ColorGrade::animParams();
    if (m_layer == GradeQualifier::kLayer) {
        QVector<AnimParam> list = ColorGrade::qualifierParams();
        for (const QString& k : GradeQualifier::rangeKeys()) list << GradeQualifier::param(k);
        return list;
    }
    QVector<AnimParam> list = ColorGrade::windowParams(m_layer);
    for (const QString& k : GradeWindows::geometryKeys()) list << GradeWindows::param(m_layer, k);
    return list;
}

QWidget* ColorPanel::buildWindows()
{
    auto* box = new QWidget;
    box->setProperty("tourTarget", "color_windows");
    box->setFixedWidth(250);
    auto* lay = new QVBoxLayout(box);
    lay->setContentsMargins(8, 0, 0, 0);
    lay->setSpacing(6);
    const QString dim = QString("color: %1;").arg(Theme::textDim.name());

    // Head: title and the buttons for new windows (like the shape buttons of DaVinci's Window palette)
    auto* head = new QHBoxLayout;
    auto* title = new QLabel(T("Fenster"));
    title->setStyleSheet(QString("color: %1;").arg(Theme::text.name()));
    head->addWidget(title);
    head->addStretch(1);
    m_addRect = smallButton("▭", T("Rechteck-Fenster hinzufügen"));
    m_addEllipse = smallButton("◯", T("Ellipsen-Fenster hinzufügen"));
    connect(m_addRect, &QToolButton::clicked, this, [this] { addWindow("rectangle"); });
    connect(m_addEllipse, &QToolButton::clicked, this, [this] { addWindow("ellipse"); });
    head->addWidget(m_addRect);
    head->addWidget(m_addEllipse);
    lay->addLayout(head);

    // Layer: whole picture or one of the windows
    auto* layers = new QHBoxLayout;
    layers->setSpacing(3);
    for (int i = 0; i <= GradeWindows::kMax; ++i) {
        auto* b = new QToolButton;
        b->setText(i == 0 ? T("Bild") : QString::number(i));
        b->setToolTip(i == 0 ? T("Räder und Regler wirken auf das ganze Bild")
                             : T("Räder und Regler wirken in Fenster %1").arg(i));
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setMinimumWidth(i == 0 ? 44 : 26);
        connect(b, &QToolButton::clicked, this, [this, i] { setLayer(i); });
        m_layerButtons << b;
        layers->addWidget(b);
    }
    { // the qualifier's layer (index GradeQualifier::kLayer), shown while the clip has one
        auto* b = new QToolButton;
        b->setText(T("Q"));
        b->setToolTip(T("Räder und Regler wirken in der Auswahl des Qualifiers"));
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setMinimumWidth(26);
        connect(b, &QToolButton::clicked, this, [this] { setLayer(GradeQualifier::kLayer); });
        m_layerButtons << b;
        layers->addWidget(b);
    }
    layers->addStretch(1);
    lay->addLayout(layers);

    m_windowControls = new QWidget;
    auto* grid = new QGridLayout(m_windowControls);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(4);
    const auto label = [&](const QString& text) {
        auto* l = new QLabel(text);
        l->setStyleSheet(dim + " font-size: 8pt;");
        return l;
    };

    m_shape = new QComboBox;
    m_shape->addItem(T("Rechteck"), "rectangle");
    m_shape->addItem(T("Ellipse"), "ellipse");
    connect(m_shape, &QComboBox::activated, this, [this](int i) {
        m_editor->setGradeWindowOption(targets(), m_layer, "shape", m_shape->itemData(i).toString());
    });
    m_invert = new QCheckBox(T("Invertieren"));
    m_invert->setToolTip(T("Wirkt außerhalb des Fensters statt innen"));
    connect(m_invert, &QCheckBox::clicked, this,
            [this](bool on) { m_editor->setGradeWindowOption(targets(), m_layer, "invert", on); });
    grid->addWidget(m_shape, 0, 0, 1, 2);
    grid->addWidget(m_invert, 0, 2, 1, 2);

    // Number fields (two per row): label above the field
    const QPair<const char*, QString> fields[] = {
        {"x", T("Position X")}, {"y", T("Position Y")}, {"width", T("Breite")},       {"height", T("Höhe")},
        {"rotation", T("Drehung")}, {"softness", T("Weichheit")}, {"amount", T("Stärke")},
    };
    int row = 1, col = 0;
    for (const auto& [key, text] : fields) {
        const EffectParam* ep = nullptr;
        EffectRegistry::paramFor(GradeWindows::param(1, key), nullptr, &ep);
        auto* f = new ScrubField(ep ? ep->min : 0, ep ? ep->max : 100, ep ? ep->step : 0.1, ep ? ep->decimals : 2);
        f->setFixedWidth(70);
        const QString k = key;
        f->onChange = [this, k](double v) {
            m_editor->setGradeValues(targets(), {{GradeWindows::param(m_layer, k), v}}, m_playhead, T("Fenster"),
                                     QString("window:%1:%2").arg(k).arg(m_layer));
        };
        f->onFinish = [this] { finish(); };
        m_windowFields << WindowField{k, f};
        if (k == "amount") { // fill row: choice, amount, color
            row += col ? 2 : 0;
            col = 0;
            m_fill = new QComboBox;
            m_fill->addItem(T("Keine Füllung"), "none");
            m_fill->addItem(T("Unschärfe"), "blur");
            m_fill->addItem(T("Verpixeln"), "pixelate");
            m_fill->addItem(T("Farbe"), "color");
            m_fill->setToolTip(T("Füllt das Fenster, z. B. um ein Gesicht oder einen Namen zu verdecken"));
            connect(m_fill, &QComboBox::activated, this, [this](int i) {
                m_editor->setGradeWindowOption(targets(), m_layer, "fill", m_fill->itemData(i).toString());
            });
            grid->addWidget(label(T("Füllung")), row, 0, 1, 2);
            grid->addWidget(m_fill, row + 1, 0, 1, 2);
            grid->addWidget(label(text), row, 2);
            grid->addWidget(f, row + 1, 2);
            m_fillColor = new QToolButton;
            m_fillColor->setToolTip(T("Farbe der Füllung"));
            m_fillColor->setFixedSize(22, 18);
            connect(m_fillColor, &QToolButton::clicked, this, [this] {
                const Clip* c = current();
                if (!c || m_layer <= 0) return;
                const QColor col = QColorDialog::getColor(GradeWindows::fillAt(*c, m_layer, localFrame(*c)).color, this,
                                                          T("Farbe der Füllung"));
                if (col.isValid()) m_editor->setGradeWindowOption(targets(), m_layer, "color", col);
            });
            grid->addWidget(m_fillColor, row + 1, 3);
            row += 2;
            continue;
        }
        grid->addWidget(label(text), row, col);
        grid->addWidget(f, row + 1, col);
        if (++col == 2) {
            col = 0;
            row += 2;
        }
    }
    // Tracker like DaVinci's window tracker (position): from the playhead backwards / forwards
    m_trackBack = new QToolButton;
    m_trackBack->setText(T("◀ Verfolgen"));
    m_trackBack->setToolTip(T("Fenster folgt dem Objekt darunter, vom Abspielkopf rückwärts zum Clipanfang"));
    m_trackForward = new QToolButton;
    m_trackForward->setText(T("Verfolgen ▶"));
    m_trackForward->setToolTip(T("Fenster folgt dem Objekt darunter, vom Abspielkopf vorwärts zum Clipende"));
    m_trackStop = smallButton("■", T("Verfolgen anhalten"));
    m_trackBack->setObjectName("color_track_back"); // the tours point at them
    m_trackForward->setObjectName("color_track_forward");
    for (QToolButton* b : {m_trackBack, m_trackForward}) b->setFocusPolicy(Qt::NoFocus);
    connect(m_trackBack, &QToolButton::clicked, this, [this] { startTracking(false); });
    connect(m_trackForward, &QToolButton::clicked, this, [this] { startTracking(true); });
    connect(m_trackStop, &QToolButton::clicked, this, &ColorPanel::stopTracking);
    auto* trackRow = new QHBoxLayout;
    trackRow->setSpacing(4);
    trackRow->addWidget(m_trackBack);
    trackRow->addWidget(m_trackForward);
    trackRow->addWidget(m_trackStop);
    trackRow->addStretch(1);
    grid->addLayout(trackRow, row++, 0, 1, 4);
    m_trackStatus = new QLabel;
    m_trackStatus->setWordWrap(true);
    m_trackStatus->setStyleSheet(dim + " font-size: 8pt;");
    grid->addWidget(m_trackStatus, row++, 0, 1, 4);
    m_trackPoll.setInterval(200);
    connect(&m_trackPoll, &QTimer::timeout, this, [this] {
        const int total = m_trackTotal ? m_trackTotal->load() : 0;
        if (total > 0) m_trackStatus->setText(T("Verfolge … %1 %").arg(m_trackDone->load() * 100 / total));
    });

    auto* remove = new QToolButton;
    remove->setText(T("Fenster löschen"));
    remove->setFocusPolicy(Qt::NoFocus);
    connect(remove, &QToolButton::clicked, this, [this] {
        if (m_layer <= 0) return;
        const int n = m_layer;
        m_layer = 0;
        m_editor->removeGradeWindow(targets(), n);
    });
    grid->addWidget(remove, row, 0, 1, 4, Qt::AlignLeft);
    lay->addWidget(m_windowControls);

    auto* hint = new QLabel(T("Ein Fenster begrenzt Räder und Regler auf einen Bildteil, die Füllung verdeckt ihn. "
                              "In der Vorschau ziehen: verschieben, Griffe = Größe, oberer Punkt = drehen."));
    hint->setWordWrap(true);
    hint->setStyleSheet(dim + " font-size: 8pt;");
    hint->setObjectName("WindowHint");
    lay->addWidget(hint);
    lay->addStretch(1);
    return box;
}

void ColorPanel::nextLayer()
{
    const Clip* c = current();
    if (!c) return;
    QVector<int> list = GradeWindows::list(*c);
    if (GradeQualifier::exists(*c)) list << GradeQualifier::kLayer;
    const auto next = std::upper_bound(list.begin(), list.end(), m_layer);
    setLayer(next == list.end() ? 0 : *next);
}

void ColorPanel::setTourButtonAllowed(bool on)
{
    m_tourButton->setVisible(on);
}

void ColorPanel::addWindow(const QString& shape)
{
    const Clip* c = current();
    if (!c) return;
    const int n = GradeWindows::freeNumber(*c);
    if (!n) return; // button is disabled then
    m_layer = n;
    m_editor->addGradeWindow(targets(), n, shape);
    refresh();
}

void ColorPanel::refreshWindows(const Clip* c, int t)
{
    const QVector<int> existing = GradeWindows::list(*c);
    for (int i = 0; i < m_layerButtons.size(); ++i) {
        m_layerButtons[i]->setVisible(i == 0 || existing.contains(i)
                                      || (i == GradeQualifier::kLayer && GradeQualifier::exists(*c)));
        m_layerButtons[i]->setChecked(i == m_layer);
    }
    m_addRect->setEnabled(GradeWindows::freeNumber(*c) != 0);
    m_addEllipse->setEnabled(GradeWindows::freeNumber(*c) != 0);
    const bool window = m_layer > 0 && m_layer <= GradeWindows::kMax;
    m_title->setText(m_layer == GradeQualifier::kLayer ? T("Farbkorrektur – Qualifier")
                     : window                          ? T("Farbkorrektur – Fenster %1").arg(m_layer)
                                                       : T("Farbkorrektur"));
    m_windowControls->setVisible(window);
    if (QLabel* hint = findChild<QLabel*>("WindowHint")) hint->setVisible(!window);
    m_overlay->setTarget(c->id, window ? m_layer : 0, m_playhead);
    if (!window) return;

    const GradeWindows::Geometry g = GradeWindows::geometryAt(*c, m_layer, t);
    const GradeWindows::FillInfo fill = GradeWindows::fillAt(*c, m_layer, t);
    m_shape->setCurrentIndex(m_shape->findData(QString(GradeWindows::shapeId(g.shape))));
    m_invert->setChecked(g.invert);
    m_fill->setCurrentIndex(m_fill->findData(QString(GradeWindows::fillId(fill.fill))));
    for (const WindowField& f : m_windowFields) {
        if (f.key == "amount") {
            f.field->setEnabled(fill.fill == GradeWindows::Fill::Blur || fill.fill == GradeWindows::Fill::Pixelate);
            // Pixelate of a project from before 0.14 can be above 100 (old coarser scale, converted): show it as it is
            f.field->setRange(0, fill.fill == GradeWindows::Fill::Pixelate ? std::max(100.0, fill.amount) : 100.0);
        }
        if (!f.field->hasFocus()) f.field->setValue(Keys::valueAt(*c, GradeWindows::param(m_layer, f.key), t));
    }
    m_fillColor->setEnabled(fill.fill == GradeWindows::Fill::Color);
    m_trackBack->setEnabled(!tracking() && t > 0);
    m_trackForward->setEnabled(!tracking() && t < c->length() - 1);
    m_trackStop->setVisible(tracking());
    m_fillColor->setStyleSheet(QString("QToolButton { background: %1; border: 1px solid %2; }")
                                   .arg(fill.color.name(), Theme::well.name()));
}

void ColorPanel::startTracking(bool forward)
{
    const Clip* c = current();
    if (tracking() || !c || m_layer <= 0 || !GradeWindows::exists(*c, m_layer)) return;
    const Clip clip = *c;
    const int n = m_layer, from = localFrame(clip), to = forward ? clip.length() - 1 : 0;
    if (from == to) return;
    const ProjectFormat format = m_editor->project()->timelineFormat();
    m_trackCancel = std::make_shared<std::atomic<bool>>(false);
    m_trackDone = std::make_shared<std::atomic<int>>(0);
    m_trackTotal = std::make_shared<std::atomic<int>>(std::abs(to - from));
    auto result = std::make_shared<Tracker::Result>();
    auto cancel = m_trackCancel;
    auto done = m_trackDone, total = m_trackTotal;
    m_trackThread = QThread::create([=] {
        *result = Tracker::trackWindow(format, clip, n, from, to, [done, total](int d, int t) {
            done->store(d);
            total->store(t);
        }, cancel.get());
    });
    m_trackThread->setParent(this); // the destructor waits for it, then it goes with the panel
    const int generation = ++m_trackGeneration;
    connect(m_trackThread, &QThread::finished, this, [this, result, clip, clipId = clip.id, start = clip.start, n, from,
                                                      generation] {
        m_trackThread->wait();
        delete m_trackThread;
        m_trackThread = nullptr;
        m_trackPoll.stop();
        const ProjectFormat fmt = m_editor->project()->timelineFormat();
        QString text;
        if (generation != m_trackGeneration) {
            // discarded: the clip id may name another clip by now
        } else if (!result->error.isEmpty()) {
            text = result->error;
        } else if (const Clip* now = TimelineOps::findClip(m_editor->project()->timeline(), clipId);
                   !now || now->mediaPath != clip.mediaPath || now->in != clip.in || now->out != clip.out
                   || now->speed != clip.speed || now->reverse != clip.reverse || now->freeze != clip.freeze
                   || now->freezeFrame != clip.freezeFrame || now->ramp != clip.ramp) {
            // Slipped, head-trimmed or retimed while tracking (or another timeline shown): the path was measured on
            // the frames the clip showed then and would land shifted (the keys are stored as source frames)
            text = T("Verfolgung verworfen – Clip geändert");
        } else {
            // The clip may have moved meanwhile: the keyframes count from its start, so they still fit
            m_editor->setGradeWindowPath(clipId, n, from, result->points);
            const int last = result->points.isEmpty() ? from : result->points.last().frame;
            if (result->lost)
                text = T("Ab %1 nicht mehr gefunden – Fenster dort neu setzen und weiter verfolgen")
                           .arg(Timecode::format(start + result->lostAt, fmt.rate.timebase()));
            else if (result->canceled)
                text = T("Angehalten bei %1").arg(Timecode::format(start + last, fmt.rate.timebase()));
            else
                text = T("Verfolgt bis %1").arg(Timecode::format(start + last, fmt.rate.timebase()));
        }
        m_trackStatus->setText(text);
        refresh();
        emit trackingFinished();
    });
    m_trackStatus->setText(T("Verfolge …"));
    m_trackPoll.start();
    m_trackThread->start();
    refresh();
}

void ColorPanel::stopTracking()
{
    if (m_trackCancel) m_trackCancel->store(true);
}

void ColorPanel::discardTracking()
{
    if (!tracking()) return;
    stopTracking();
    ++m_trackGeneration;
}

// ---- Curves ----

QWidget* ColorPanel::buildCurves()
{
    auto* box = new QWidget;
    box->setProperty("tourTarget", "color_curves");
    auto* lay = new QVBoxLayout(box);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(6);
    auto* rowBox = new QWidget; // wraps into two rows on a narrow page
    auto* row = new FlowLayout(rowBox, 3, 3);
    row->setContentsMargins(0, 0, 0, 0);
    using K = GradeCurves::Kind;
    const struct {
        K kind;
        QString text, tip;
        QColor color;
    } kinds[] = {
        {K::Y, T("Y"), T("Master-Kurve: Helligkeit aller Kanäle"), Theme::text},
        {K::R, T("R"), T("Rot-Kurve"), QColor(0xe8, 0x5a, 0x5a)},
        {K::G, T("G"), T("Grün-Kurve"), QColor(0x5a, 0xc8, 0x6a)},
        {K::B, T("B"), T("Blau-Kurve"), QColor(0x5a, 0x8a, 0xe8)},
        {K::HueHue, T("Farbton/Farbton"), T("Dreht einzelne Farbtöne zu anderen (z. B. Grün zu Gelb)"), Theme::text},
        {K::HueSat, T("Farbton/Sättigung"), T("Sättigung je Farbton (z. B. nur Blau kräftiger)"), Theme::text},
        {K::LumSat, T("Helligkeit/Sättigung"), T("Sättigung je Helligkeit (z. B. Schatten entsättigen)"), Theme::text},
    };
    auto* group = new QButtonGroup(this);
    m_curveButtons.resize(GradeCurves::kCount);
    for (const auto& k : kinds) {
        auto* b = new QToolButton;
        b->setText(k.text);
        b->setToolTip(k.tip);
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);
        if (GradeCurves::tone(k.kind)) {
            b->setMinimumWidth(26);
            b->setStyleSheet(QString("QToolButton { color: %1; }").arg(k.color.name()));
        }
        group->addButton(b);
        const K kind = k.kind;
        connect(b, &QToolButton::clicked, this, [this, kind] {
            m_curve->setKind(kind);
            refresh();
        });
        m_curveButtons[int(k.kind)] = b;
        row->addWidget(b);
    }
    m_curveReset = smallButton("↺", T("Diese Kurve zurücksetzen"));
    connect(m_curveReset, &QToolButton::clicked, this,
            [this] { m_editor->setGradeCurve(targets(), m_curve->kind(), {}); });
    row->addWidget(m_curveReset);
    lay->addWidget(rowBox);
    m_curve = new CurveEditor;
    m_curve->setObjectName("color_curve");
    m_curveButtons[int(K::Y)]->setChecked(true);
    connect(m_curve, &CurveEditor::changed, this, [this](const QVector<QPointF>& points) {
        m_editor->setGradeCurve(targets(), m_curve->kind(), points,
                                QString("color:curve:%1").arg(GradeCurves::key(m_curve->kind())));
    });
    connect(m_curve, &CurveEditor::finished, this, [this] {
        finish();
        refresh();
    });
    lay->addWidget(m_curve, 1);
    m_histTimer.setSingleShot(true);
    m_histTimer.setInterval(150);
    connect(&m_histTimer, &QTimer::timeout, this, [this] {
        if (m_palette != CurvesPalette || !isVisible() || m_histFrame.isNull()) return;
        m_curve->setHistograms(CurveEditor::histograms(m_histFrame));
        m_histFrame = QImage();
    });
    m_curveNote = new QLabel(T("Kurven wirken auf das ganze Bild, nicht nur in der gewählten Ebene."));
    m_curveNote->setWordWrap(true);
    m_curveNote->setStyleSheet(QString("color: %1; font-size: 8pt;").arg(Theme::textDim.name()));
    lay->addWidget(m_curveNote);
    return box;
}

void ColorPanel::setFrame(const QImage& frame)
{
    m_lastFrame = frame;
    if (m_compareId && m_overlay->sideBySide()) m_overlay->setLiveFrame(frame);
    m_histFrame = frame;
    if (!m_histTimer.isActive()) m_histTimer.start();
}

void ColorPanel::refreshCurves(const Clip* c)
{
    const ColorGrade::Params p = ColorGrade::at(*c, localFrame(*c));
    for (int k = 0; k < GradeCurves::kCount; ++k) {
        // A bent curve: its button marked (the wheels page shows the same with its moved points)
        const bool bent = !GradeCurves::isIdentity(p.curves.points[k], GradeCurves::Kind(k));
        QFont f = m_curveButtons[k]->font();
        f.setUnderline(bent);
        m_curveButtons[k]->setFont(f);
    }
    const QVector<QPointF>& pts = p.curves.points[int(m_curve->kind())];
    m_curve->setPoints(pts);
    m_curveReset->setEnabled(!pts.isEmpty());
    m_curveNote->setVisible(m_layer != 0);
}

// ---- Qualifier ----

QWidget* ColorPanel::buildQualifier()
{
    auto* box = new QWidget;
    box->setProperty("tourTarget", "color_qualifier");
    auto* lay = new QVBoxLayout(box);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(6);
    const QString dim = QString("color: %1; font-size: 8pt;").arg(Theme::textDim.name());

    // Without a qualifier: what it does, and the buttons to start one
    m_qualEmpty = new QWidget;
    {
        auto* el = new QVBoxLayout(m_qualEmpty);
        el->setContentsMargins(0, 0, 0, 0);
        auto* text = new QLabel(T("Ein Qualifier wählt Farben aus – etwa Haut oder Himmel – und die Räder wirken dann "
                                  "nur auf diese Farben."));
        text->setWordWrap(true);
        text->setStyleSheet(dim);
        el->addWidget(text);
        auto* buttons = new QHBoxLayout;
        auto* pick = new QPushButton(T("Farbe im Bild aufnehmen …"));
        pick->setObjectName("color_qualifier_pick");
        pick->setFocusPolicy(Qt::NoFocus);
        connect(pick, &QPushButton::clicked, this, [this] {
            // The qualifier comes with the picked colour (applyQualifierColor): a cancelled pick leaves nothing
            if (const Clip* c = current()) emit qualifierPickRequested(c->id);
        });
        auto* add = new QPushButton(T("Hauttöne"));
        add->setToolTip(T("Qualifier mit einer Auswahl typischer Hauttöne anlegen"));
        add->setFocusPolicy(Qt::NoFocus);
        connect(add, &QPushButton::clicked, this, [this] { m_editor->addGradeQualifier(targets()); });
        buttons->addWidget(pick);
        buttons->addWidget(add);
        buttons->addStretch(1);
        el->addLayout(buttons);
        el->addStretch(1);
    }
    lay->addWidget(m_qualEmpty);

    m_qualControls = new QWidget;
    auto* cl = new QVBoxLayout(m_qualControls);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(6);
    auto* topBox = new QWidget;
    auto* top = new FlowLayout(topBox, 6, 3);
    top->setContentsMargins(0, 0, 0, 0);
    auto* pick = new QToolButton;
    pick->setText(T("Farbe aufnehmen …"));
    pick->setToolTip(T("Klick ins Bild des Clips: die Auswahl wird um diese Farbe gelegt"));
    pick->setFocusPolicy(Qt::NoFocus);
    connect(pick, &QToolButton::clicked, this, [this] {
        if (const Clip* c = current()) emit qualifierPickRequested(c->id);
    });
    m_qualMask = new QToolButton;
    m_qualMask->setText(T("Maske zeigen"));
    m_qualMask->setToolTip(T("Vorschau zeigt die Auswahl: weiß = ausgewählt, schwarz = nicht"));
    m_qualMask->setCheckable(true);
    m_qualMask->setFocusPolicy(Qt::NoFocus);
    m_qualMask->setObjectName("color_qualifier_mask");
    connect(m_qualMask, &QToolButton::toggled, this, [this](bool on) {
        m_showMask = on;
        updateMask();
    });
    m_qualInvert = new QCheckBox(T("Invertieren"));
    m_qualInvert->setToolTip(T("Wirkt auf alles außer der Auswahl"));
    connect(m_qualInvert, &QCheckBox::clicked, this, [this](bool on) { m_editor->setGradeQualifierInvert(targets(), on); });
    auto* correct = new QToolButton;
    correct->setText(T("Auswahl korrigieren ▶"));
    correct->setToolTip(T("Zu den Rädern: sie wirken dann nur in der Auswahl (Ebene „Q“)"));
    correct->setFocusPolicy(Qt::NoFocus);
    connect(correct, &QToolButton::clicked, this, [this] {
        m_layer = GradeQualifier::kLayer;
        setPalette(Wheels);
    });
    auto* remove = new QToolButton;
    remove->setText(T("Qualifier löschen"));
    remove->setFocusPolicy(Qt::NoFocus);
    connect(remove, &QToolButton::clicked, this, [this] {
        if (m_layer == GradeQualifier::kLayer) m_layer = 0;
        m_editor->removeGradeQualifier(targets());
    });
    top->addWidget(pick);
    top->addWidget(m_qualMask);
    top->addWidget(m_qualInvert);
    m_qualWindow = new QComboBox;
    m_qualWindow->setObjectName("color_qualifier_window");
    m_qualWindow->setToolTip(T("Die Auswahl nur innerhalb eines Fensters (z. B. nur die Haut im Gesicht)"));
    connect(m_qualWindow, &QComboBox::activated, this,
            [this](int i) { m_editor->setGradeQualifierWindow(targets(), m_qualWindow->itemData(i).toInt()); });
    top->addWidget(m_qualWindow);
    top->addWidget(correct);
    top->addWidget(remove);
    cl->addWidget(topBox);

    // Three ranges: a bar of what is selected, the numbers beside it
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(2);
    const QString rows[3] = {T("Farbton"), T("Sättigung"), T("Helligkeit")};
    const QString heads[3][3] = {{T("Mitte"), T("Breite"), T("Weich")},
                                 {T("Von"), T("Bis"), T("Weich")},
                                 {T("Von"), T("Bis"), T("Weich")}};
    for (int r = 0; r < 3; ++r) {
        auto* name = new QLabel(rows[r]);
        name->setStyleSheet(QString("color: %1;").arg(Theme::text.name()));
        grid->addWidget(name, 2 * r + 1, 0);
        m_qualBars[r] = new QualifierBar(QualifierBar::Kind(r));
        m_qualBars[r]->setObjectName(QString("color_qualifier_bar_%1").arg(r));
        m_qualBars[r]->onChange = [this, bar = m_qualBars[r]](const QString& key, double v) {
            // The bar shows the new range at once (the model answers through refresh as well)
            const Clip* c = current();
            if (!c) return;
            m_editor->setGradeValues(targets(), {{GradeQualifier::param(key), v}}, m_playhead, T("Qualifier"),
                                     QString("qualifier:bar:%1").arg(key));
            if (const Clip* now = current()) bar->setRange(GradeQualifier::rangeAt(*now, localFrame(*now)));
        };
        m_qualBars[r]->onFinish = [this] { finish(); };
        grid->addWidget(m_qualBars[r], 2 * r + 1, 1);
        for (int k = 0; k < 3; ++k) {
            const QString key = GradeQualifier::rangeKeys()[3 * r + k];
            const EffectParam* ep = nullptr;
            EffectRegistry::paramFor(GradeQualifier::param(key), nullptr, &ep);
            auto* l = new QLabel(heads[r][k]);
            l->setStyleSheet(dim);
            l->setAlignment(Qt::AlignCenter);
            auto* f = new ScrubField(ep ? ep->min : 0, ep ? ep->max : 100, ep ? ep->step : 0.2, ep ? ep->decimals : 1);
            f->setShrinkable(true);
            f->onChange = [this, key](double v) {
                m_editor->setGradeValues(targets(), {{GradeQualifier::param(key), v}}, m_playhead, T("Qualifier"),
                                         QString("qualifier:%1").arg(key));
            };
            f->onFinish = [this] { finish(); };
            m_qualFields << QualField{key, f};
            grid->addWidget(l, 2 * r, 2 + k);
            grid->addWidget(f, 2 * r + 1, 2 + k);
        }
    }
    grid->setColumnStretch(1, 1);
    cl->addLayout(grid);
    auto* hint = new QLabel(T("Die Auswahl richtet sich nach den Farben nach Rädern und Kurven. Weich = sanfter Rand "
                              "der Auswahl."));
    hint->setWordWrap(true);
    hint->setStyleSheet(dim);
    cl->addWidget(hint);
    cl->addStretch(1);
    lay->addWidget(m_qualControls, 1);
    return box;
}

void ColorPanel::refreshQualifier(const Clip* c, int t)
{
    const bool has = GradeQualifier::exists(*c);
    m_qualEmpty->setVisible(!has);
    m_qualControls->setVisible(has);
    if (!has) return;
    const GradeQualifier::Range r = GradeQualifier::rangeAt(*c, t);
    m_qualInvert->setChecked(r.invert);
    {
        QSignalBlocker block(m_qualWindow);
        const QVector<int> windows = GradeWindows::list(*c);
        QVector<int> shown; // rebuilt only when the windows changed (refresh runs on every playhead frame)
        for (int i = 1; i < m_qualWindow->count(); ++i) shown << m_qualWindow->itemData(i).toInt();
        if (shown != windows || m_qualWindow->count() == 0) {
            m_qualWindow->clear();
            m_qualWindow->addItem(T("Ganzes Bild"), 0);
            for (int n : windows) m_qualWindow->addItem(T("Nur in Fenster %1").arg(n), n);
        }
        const int want = std::max(0, m_qualWindow->findData(GradeQualifier::windowOf(*c)));
        if (m_qualWindow->currentIndex() != want) m_qualWindow->setCurrentIndex(want);
        m_qualWindow->setEnabled(m_qualWindow->count() > 1);
    }
    for (QualifierBar* b : m_qualBars) b->setRange(r);
    for (const QualField& f : m_qualFields)
        if (!f.field->hasFocus()) f.field->setValue(Keys::valueAt(*c, GradeQualifier::param(f.key), t));
}

void ColorPanel::updateMask()
{
    const Clip* c = isVisible() && m_showMask ? current() : nullptr;
    const int id = c && GradeQualifier::exists(*c) ? c->id : 0;
    if (id == m_maskSent) return;
    m_maskSent = id;
    emit qualifierMaskChanged(id);
}

void ColorPanel::applyQualifierColor(const QColor& color)
{
    const Clip* c = current();
    if (!c || !color.isValid()) return;
    if (!GradeQualifier::exists(*c)) m_editor->addGradeQualifier(targets());
    c = current();
    if (!c) return;
    GradeQualifier::Range r = GradeQualifier::around(color);
    r.invert = GradeQualifier::rangeAt(*c, localFrame(*c)).invert;
    m_editor->setGradeQualifierRange(targets(), r, m_playhead);
    setPalette(QualifierPalette);
}

// ---- Stills ----

QWidget* ColorPanel::buildStills()
{
    auto* box = new QWidget;
    box->setProperty("tourTarget", "color_stills");
    auto* lay = new QVBoxLayout(box);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(6);
    auto* rowBox = new QWidget; // wraps on a narrow page
    auto* row = new FlowLayout(rowBox, 6, 3);
    row->setContentsMargins(0, 0, 0, 0);
    auto* grab = new QToolButton;
    m_stillGrab = grab;
    grab->setText(T("Standbild aufnehmen"));
    grab->setToolTip(T("Das Bild im Viewer mit der Korrektur des Clips als Standbild ablegen"));
    grab->setObjectName("color_still_grab");
    grab->setFocusPolicy(Qt::NoFocus);
    connect(grab, &QToolButton::clicked, this, [this] {
        if (current()) emit stillGrabRequested();
    });
    m_stillCompare = new QToolButton;
    m_stillCompare->setText(T("Vergleichen"));
    m_stillCompare->setToolTip(T("Standbild links im Viewer, das aktuelle Bild rechts; die Linie lässt sich ziehen "
                                 "(Doppelklick auf ein Standbild = vergleichen)"));
    m_stillCompare->setCheckable(true);
    m_stillCompare->setObjectName("color_still_compare");
    m_stillCompare->setFocusPolicy(Qt::NoFocus);
    connect(m_stillCompare, &QToolButton::clicked, this,
            [this](bool on) { setComparedStill(on ? selectedStill() : 0); });
    m_stillSide = new QToolButton;
    m_stillSide->setText(T("Nebeneinander"));
    m_stillSide->setToolTip(T("Beim Vergleichen beide Bilder ganz nebeneinander statt der Wischlinie"));
    m_stillSide->setCheckable(true);
    m_stillSide->setObjectName("color_still_side");
    m_stillSide->setFocusPolicy(Qt::NoFocus);
    connect(m_stillSide, &QToolButton::toggled, this, [this](bool on) {
        m_overlay->setSideBySide(on);
        m_overlay->setLiveFrame(m_lastFrame);
    });
    m_stillApply = new QToolButton;
    m_stillApply->setText(T("Korrektur übernehmen"));
    m_stillApply->setToolTip(T("Die Korrektur des Standbilds auf die gewählten Clips übertragen"));
    m_stillApply->setObjectName("color_still_apply");
    m_stillApply->setFocusPolicy(Qt::NoFocus);
    connect(m_stillApply, &QToolButton::clicked, this, [this] {
        if (const int id = selectedStill()) m_editor->applyGradeStill(id, targets());
    });
    m_stillRemove = new QToolButton;
    m_stillRemove->setText(T("Löschen"));
    m_stillRemove->setFocusPolicy(Qt::NoFocus);
    connect(m_stillRemove, &QToolButton::clicked, this, [this] {
        if (const int id = selectedStill()) m_editor->removeGradeStill(id);
    });
    row->addWidget(grab);
    row->addWidget(m_stillCompare);
    row->addWidget(m_stillSide);
    row->addWidget(m_stillApply);
    row->addWidget(m_stillRemove);
    lay->addWidget(rowBox);

    m_stillList = new QListWidget;
    m_stillList->setObjectName("color_stills");
    m_stillList->setViewMode(QListView::IconMode);
    m_stillList->setFlow(QListView::LeftToRight);
    m_stillList->setWrapping(true);
    m_stillList->setResizeMode(QListView::Adjust);
    m_stillList->setMovement(QListView::Static);
    m_stillList->setIconSize(QSize(128, 72));
    m_stillList->setSpacing(4);
    m_stillList->setUniformItemSizes(true);
    m_stillList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_stillList->setStyleSheet(
        QString("QListWidget { background: %1; border: 1px solid %2; color: %3; font-size: 8pt; }"
                "QListWidget::item { border: 1px solid transparent; padding: 2px; }"
                "QListWidget::item:selected { background: %4; color: %5; border: 1px solid %6; }")
            .arg(Theme::lane.name(), Theme::border.name(), Theme::textDim.name(), Theme::control.name(),
                 Theme::text.name(), Theme::primary.name()));
    connect(m_stillList, &QListWidget::itemSelectionChanged, this, [this] {
        const int id = selectedStill();
        if (m_compareId && id && id != m_compareId) setComparedStill(id); // comparing follows the selection
        refreshStills();
    });
    connect(m_stillList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        const int id = item->data(Qt::UserRole).toInt();
        setComparedStill(m_compareId == id ? 0 : id);
    });
    lay->addWidget(m_stillList, 1);
    auto* hint = new QLabel(T("Standbilder werden im Projekt gespeichert. Vergleichen hilft, Clips aneinander "
                              "anzugleichen."));
    hint->setWordWrap(true);
    hint->setStyleSheet(QString("color: %1; font-size: 8pt;").arg(Theme::textDim.name()));
    lay->addWidget(hint);
    return box;
}

int ColorPanel::selectedStill() const
{
    const QList<QListWidgetItem*> sel = m_stillList->selectedItems();
    return sel.isEmpty() ? 0 : sel.first()->data(Qt::UserRole).toInt();
}

void ColorPanel::refreshStills()
{
    const QVector<GradeStill>& stills = m_editor->project()->stills();
    // Rebuilt only when the list changed (ids, labels and pictures), so the selection and scroll stay
    QStringList want, have;
    for (const GradeStill& s : stills) want << QString::number(s.id) + s.label + '#' + QString::number(qHash(s.image));
    for (int i = 0; i < m_stillList->count(); ++i)
        have << m_stillList->item(i)->data(Qt::UserRole).toString() + m_stillList->item(i)->text() + '#'
                    + m_stillList->item(i)->data(Qt::UserRole + 1).toString();
    // The pictures are decoded only for the stills palette (a project with many stills opens without that work)
    if (want != have && m_palette == StillsPalette) {
        const int keep = selectedStill();
        QSignalBlocker block(m_stillList);
        m_stillList->clear();
        for (const GradeStill& s : stills) {
            const size_t hash = qHash(s.image);
            auto icon = m_stillIcons.find(s.id);
            if (icon == m_stillIcons.end() || icon->first != hash)
                icon = m_stillIcons.insert(s.id, {hash, QIcon(QPixmap::fromImage(GradeStills::picture(s)))});
            auto* item = new QListWidgetItem(icon->second, s.label);
            item->setData(Qt::UserRole, s.id);
            item->setData(Qt::UserRole + 1, QString::number(hash));
            item->setToolTip(s.label);
            m_stillList->addItem(item);
            if (s.id == keep) item->setSelected(true);
        }
        if (!keep && m_stillList->count()) m_stillList->item(m_stillList->count() - 1)->setSelected(true);
    }
    if (m_compareId && std::none_of(stills.begin(), stills.end(), [&](const GradeStill& s) { return s.id == m_compareId; }))
        setComparedStill(0); // deleted (or undone)
    const bool any = selectedStill() != 0;
    m_stillCompare->setEnabled(any);
    m_stillCompare->setChecked(m_compareId != 0);
    m_stillApply->setEnabled(any && current());
    m_stillRemove->setEnabled(any);
    // The picture comes from the playhead, the grade from the shown clip: only when that clip is under the playhead
    const Clip* c = current();
    const bool under = c && m_playhead >= c->start && m_playhead < c->end();
    m_stillGrab->setEnabled(under);
    m_stillGrab->setToolTip(under ? T("Das Bild im Viewer mit der Korrektur des Clips als Standbild ablegen")
                                  : T("Den Abspielkopf auf den Clip stellen, um ein Standbild aufzunehmen"));
}

void ColorPanel::setComparedStill(int stillId)
{
    const QVector<GradeStill>& stills = m_editor->project()->stills();
    const auto it = std::find_if(stills.begin(), stills.end(), [&](const GradeStill& s) { return s.id == stillId; });
    const QImage picture = it == stills.end() ? QImage() : GradeStills::picture(*it);
    m_compareId = picture.isNull() ? 0 : stillId; // a still whose picture cannot be read compares with nothing
    m_overlay->setCompare(picture);
    if (m_stillCompare) m_stillCompare->setChecked(m_compareId != 0);
}

void ColorPanel::addStill(const QImage& picture, const QString& timecode)
{
    const Clip* c = current();
    if (!c || picture.isNull() || m_playhead < c->start || m_playhead >= c->end()) return; // grade and picture of one clip
    const QString label = m_editor->project()->clipName(*c) + "  " + timecode;
    setPalette(StillsPalette);
    if (const int id = m_editor->addGradeStill(c->id, picture, label)) {
        refreshStills();
        for (int i = 0; i < m_stillList->count(); ++i)
            m_stillList->item(i)->setSelected(m_stillList->item(i)->data(Qt::UserRole).toInt() == id);
    }
    setPalette(StillsPalette);
}
