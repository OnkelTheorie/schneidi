// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/ColorPanel.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/GradeWindows.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "core/Timecode.h"
#include "engine/ColorGrade.h"
#include "engine/Tracker.h"
#include "ui/ElidedLabel.h"
#include "ui/ScrubField.h"
#include "ui/WindowOverlay.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QConicalGradient>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QRadialGradient>
#include <QSettings>
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

ColorPanel::ColorPanel(Editor* editor, QWidget* parent) : QWidget(parent), m_editor(editor)
{
    setObjectName("Panel");
    using P = AnimParam;
    m_wheels = {
        {N_("Lift"), {P::GradeLiftY, P::GradeLiftR, P::GradeLiftG, P::GradeLiftB}, 0.0, 0.25},
        {N_("Gamma"), {P::GradeGammaY, P::GradeGammaR, P::GradeGammaG, P::GradeGammaB}, 0.0, 0.25},
        {N_("Gain"), {P::GradeGainY, P::GradeGainR, P::GradeGainG, P::GradeGainB}, 1.0, 0.5},
        {N_("Offset"), {P::GradeOffsetY, P::GradeOffsetR, P::GradeOffsetG, P::GradeOffsetB}, 25.0, 25.0},
    };

    m_overlay = new WindowOverlay(m_editor, [this] { return targets(); }, this);

    // Header row: title, clip name, on/off, keyframes, reset all
    auto* title = new QLabel(T("Primaries – Farbräder"));
    title->setObjectName("PanelTitle");
    m_title = title;
    m_clipName = new QLabel;
    m_clipName->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    m_enabled = new QToolButton;
    m_enabled->setCheckable(true);
    m_enabled->setFixedSize(10, 10);
    m_enabled->setFocusPolicy(Qt::NoFocus);
    m_enabled->setToolTip(T("Farbkorrektur des Clips an/aus"));
    m_enabled->setStyleSheet(QString("QToolButton { border: none; border-radius: 5px; background: %1; }"
                                     "QToolButton:checked { background: %2; }")
                                 .arg(Theme::controlOff.name(), Theme::warning.name()));
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
        if (m_layer > 0) // window: only its correction, the shape stays
            m_editor->resetGrade(targets(), ColorGrade::windowParams(m_layer), m_playhead, T("Fenster-Korrektur zurücksetzen"));
        else
            m_editor->resetGrade(targets(), {}, m_playhead, T("Farbkorrektur zurücksetzen"));
    });

    auto* head = new QHBoxLayout;
    head->setContentsMargins(0, 0, 8, 0);
    head->setSpacing(6);
    head->addWidget(title);
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
    const QString sliderNames[] = {T("Kontrast"), T("Pivot"), T("Sättigung"), T("Temperatur"), T("Tönung"), T("Belichtung")};
    const double ranges[][4] = {{0, 2, 0.002, 3}, {0, 1, 0.001, 3}, {0, 100, 0.1, 2},
                                {-4000, 4000, 5, 1}, {-100, 100, 0.1, 2}, {-4, 4, 0.01, 2}};

    // LUT: label above like the sliders, the file name with its buttons below
    auto* lutBox = new QWidget;
    auto* lutLay = new QVBoxLayout(lutBox);
    lutLay->setContentsMargins(0, 0, 0, 0);
    lutLay->setSpacing(1);
    auto* lutLabel = new QLabel(T("LUT"));
    lutLabel->setStyleSheet(QString("color: %1; font-size: 8pt;").arg(Theme::textDim.name()));
    lutLabel->setAlignment(Qt::AlignCenter);
    m_lutName = new ElidedLabel; // a long file name ends in "…" instead of widening Offset's column
    m_lutName->setMaximumWidth(110);
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
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(10);
    for (int i = 0; i < m_wheels.size(); ++i) {
        const int col = 2 * i;
        if (i) grid->setColumnStretch(col - 1, 1);
        grid->addWidget(buildWheel(m_wheels[i]), 0, col);
        auto* under = new QHBoxLayout;
        under->setSpacing(14);
        if (2 * i + 1 < m_sliders.size()) {
            under->addWidget(buildSlider(m_sliders[2 * i], sliderNames[2 * i], ranges[2 * i][0], ranges[2 * i][1],
                                         ranges[2 * i][2], int(ranges[2 * i][3])));
            under->addWidget(buildSlider(m_sliders[2 * i + 1], sliderNames[2 * i + 1], ranges[2 * i + 1][0],
                                         ranges[2 * i + 1][1], ranges[2 * i + 1][2], int(ranges[2 * i + 1][3])));
        } else {
            under->addWidget(lutBox);
        }
        grid->addLayout(under, 1, col, Qt::AlignHCenter);
    }

    m_body = new QWidget;
    auto* outer = new QHBoxLayout(m_body);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(10);
    auto* bl = new QVBoxLayout;
    bl->addLayout(grid);
    bl->addStretch(1);
    outer->addLayout(bl, 1);
    // The windows as a section of their own beside the wheels
    auto* line = new QFrame;
    line->setFrameShape(QFrame::VLine);
    line->setFixedWidth(1);
    line->setStyleSheet(QString("background: %1; border: none;").arg(Theme::border.name()));
    outer->addSpacing(6);
    outer->addWidget(line);
    outer->addWidget(buildWindows());

    m_empty = new QLabel(T("Kein Videoclip ausgewählt oder am Playhead"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addLayout(head);
    lay->addWidget(m_body, 1);
    lay->addWidget(m_empty, 1);

    Project* project = m_editor->project();
    connect(project, &Project::timelineChanged, this, &ColorPanel::refresh);
    connect(m_editor->selection(), &Selection::changed, this, &ColorPanel::refresh);
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
        f->setFixedWidth(46);
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
    s.field->setFixedWidth(64);
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
    m_overlay->setVisible(true);
    refresh();
}

void ColorPanel::hideEvent(QHideEvent* e)
{
    QWidget::hideEvent(e);
    m_overlay->setVisible(false);
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
    m_body->setVisible(c);
    m_empty->setVisible(!c);
    for (QWidget* w : {static_cast<QWidget*>(m_enabled), static_cast<QWidget*>(m_keyPrev),
                       static_cast<QWidget*>(m_keyDiamond), static_cast<QWidget*>(m_keyNext)})
        w->setEnabled(c);
    if (!c) {
        m_clipName->clear();
        m_overlay->setTarget(0, 0, m_playhead);
        return;
    }
    if (m_layer > 0 && !GradeWindows::exists(*c, m_layer)) m_layer = 0; // window deleted (or other clip)
    const int n = targets().size();
    {
        const QString cn = m_editor->project()->clipName(*c);
        m_clipName->setText(n > 1 ? T("%1 (+%2 weitere)").arg(cn).arg(n - 1) : cn);
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
}

// ---- Windows ----

ViewerOverlay* ColorPanel::overlay() const { return m_overlay; }

void ColorPanel::setLayer(int layer)
{
    const Clip* c = current();
    if (layer < 0 || layer > GradeWindows::kMax || (layer > 0 && (!c || !GradeWindows::exists(*c, layer)))) layer = 0;
    m_layer = layer;
    refresh();
}

AnimParam ColorPanel::mapped(AnimParam p) const
{
    if (m_layer <= 0) return p;
    const QVector<AnimParam>& base = ColorGrade::animParams();
    const int i = int(base.indexOf(p));
    return i >= 0 ? ColorGrade::windowParams(m_layer)[i] : p;
}

QVector<AnimParam> ColorPanel::layerParams() const
{
    if (m_layer <= 0) return ColorGrade::animParams();
    QVector<AnimParam> list = ColorGrade::windowParams(m_layer);
    for (const QString& k : GradeWindows::geometryKeys()) list << GradeWindows::param(m_layer, k);
    return list;
}

QWidget* ColorPanel::buildWindows()
{
    auto* box = new QWidget;
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
    m_trackBack->setToolTip(T("Fenster folgt dem Objekt darunter, vom Playhead rückwärts zum Clipanfang"));
    m_trackForward = new QToolButton;
    m_trackForward->setText(T("Verfolgen ▶"));
    m_trackForward->setToolTip(T("Fenster folgt dem Objekt darunter, vom Playhead vorwärts zum Clipende"));
    m_trackStop = smallButton("■", T("Verfolgen anhalten"));
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
                              "Im Viewer ziehen: verschieben, Griffe = Größe, oberer Punkt = drehen."));
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
    const QVector<int> list = GradeWindows::list(*c);
    const auto next = std::upper_bound(list.begin(), list.end(), m_layer);
    setLayer(next == list.end() ? 0 : *next);
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
        m_layerButtons[i]->setVisible(i == 0 || existing.contains(i));
        m_layerButtons[i]->setChecked(i == m_layer);
    }
    m_addRect->setEnabled(GradeWindows::freeNumber(*c) != 0);
    m_addEllipse->setEnabled(GradeWindows::freeNumber(*c) != 0);
    m_title->setText(m_layer > 0 ? T("Primaries – Fenster %1").arg(m_layer) : T("Primaries – Farbräder"));
    m_windowControls->setVisible(m_layer > 0);
    if (QLabel* hint = findChild<QLabel*>("WindowHint")) hint->setVisible(m_layer == 0);
    m_overlay->setTarget(c->id, m_layer, m_playhead);
    if (m_layer <= 0) return;

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
    const ProjectFormat format = m_editor->project()->format();
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
    connect(m_trackThread, &QThread::finished, this, [this, result, clipId = clip.id, start = clip.start, n, from,
                                                      generation] {
        m_trackThread->wait();
        delete m_trackThread;
        m_trackThread = nullptr;
        m_trackPoll.stop();
        const ProjectFormat& fmt = m_editor->project()->format();
        QString text;
        if (generation != m_trackGeneration) {
            // discarded: the clip id may name another clip by now
        } else if (!result->error.isEmpty()) {
            text = result->error;
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
