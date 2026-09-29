#include "ui/ColorPanel.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/ColorGrade.h"
#include "ui/ScrubField.h"

#include <QConicalGradient>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QRadialGradient>
#include <QSettings>
#include <QStandardPaths>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <optional>

namespace {

// Farbbalance eines Rads als Punkt (u, v) in der Fläche, wie ein Vektorskop (BT.709): rechts = Blau, oben = Rot.
// R/G/B-Änderungen daraus haben zusammen keine Helligkeit -> Balance ändert die Farbe, der Master die Helligkeit.
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

// Farbrad wie DaVinci: Punkt ziehen = Farbbalance (relativ zur Mausbewegung, Shift = fein), Doppelklick = Mitte.
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
        // Farbkreis: Farbton je Richtung wie ein Vektorskop, zur Mitte hin grau
        QConicalGradient hue(c, 0);
        for (int i = 0; i <= 12; ++i) {
            const double a = i / 12.0 * 2 * M_PI;
            double rgb[3];
            uvToRgb(std::cos(a) * 0.5, std::sin(a) * 0.5, rgb);
            auto ch = [](double x) { return std::clamp(int(128 + x * 190), 0, 255); };
            hue.setColorAt(i / 12.0, QColor(ch(rgb[0]), ch(rgb[1]), ch(rgb[2])));
        }
        p.setPen(QPen(QColor(0x10, 0x10, 0x12), 2));
        p.setBrush(hue);
        p.drawEllipse(r);
        QRadialGradient fade(c, r.width() / 2);
        fade.setColorAt(0, QColor(0x2a, 0x2a, 0x30, 255));
        fade.setColorAt(0.75, QColor(0x2a, 0x2a, 0x30, 120));
        fade.setColorAt(1, QColor(0x2a, 0x2a, 0x30, 40));
        p.setPen(Qt::NoPen);
        p.setBrush(fade);
        p.drawEllipse(r.adjusted(2, 2, -2, -2));
        // Fadenkreuz
        p.setPen(QPen(QColor(255, 255, 255, 40), 1));
        p.drawLine(QPointF(c.x(), r.top() + 6), QPointF(c.x(), r.bottom() - 6));
        p.drawLine(QPointF(r.left() + 6, c.y()), QPointF(r.right() - 6, c.y()));
        // Punkt
        const QPointF dot(c.x() + m_u * r.width() / 2, c.y() - m_v * r.height() / 2);
        const bool moved = std::abs(m_u) > 1e-6 || std::abs(m_v) > 1e-6;
        p.setPen(QPen(moved ? Theme::accent : Theme::text, 1.5));
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
        {"Lift", {P::GradeLiftY, P::GradeLiftR, P::GradeLiftG, P::GradeLiftB}, 0.0, 0.25},
        {"Gamma", {P::GradeGammaY, P::GradeGammaR, P::GradeGammaG, P::GradeGammaB}, 0.0, 0.25},
        {"Gain", {P::GradeGainY, P::GradeGainR, P::GradeGainG, P::GradeGainB}, 1.0, 0.5},
        {"Offset", {P::GradeOffsetY, P::GradeOffsetR, P::GradeOffsetG, P::GradeOffsetB}, 25.0, 25.0},
    };

    // Kopfzeile: Titel, Clipname, an/aus, Keyframes, alles zurücksetzen
    auto* title = new QLabel(T("Primaries – Farbräder"));
    title->setObjectName("PanelTitle");
    m_clipName = new QLabel;
    m_clipName->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    m_enabled = new QToolButton;
    m_enabled->setCheckable(true);
    m_enabled->setFixedSize(10, 10);
    m_enabled->setFocusPolicy(Qt::NoFocus);
    m_enabled->setToolTip(T("Farbkorrektur des Clips an/aus"));
    m_enabled->setStyleSheet("QToolButton { border: none; border-radius: 5px; background: #55555c; }"
                             "QToolButton:checked { background: #e8414a; }");
    connect(m_enabled, &QToolButton::clicked, this, [this](bool on) { m_editor->setGradeEnabled(targets(), on); });
    m_keyPrev = smallButton("◀", T("Voriger Keyframe"));
    m_keyDiamond = smallButton("◇", T("Keyframe für die ganze Korrektur setzen/entfernen"));
    m_keyNext = smallButton("▶", T("Nächster Keyframe"));
    auto jump = [this](bool forward) {
        const Clip* c = current();
        if (!c) return;
        const int t = localFrame(*c);
        std::optional<int> best;
        for (int k : Keys::keyTimes(*c, ColorGrade::animParams())) {
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
        const bool here = Keys::keyAt(*c, AnimParam::GradeLiftY, localFrame(*c));
        m_editor->setGradeKeyframe(targets(), m_playhead, !here);
    });
    auto* resetAll = smallButton("↺", T("Farbkorrektur zurücksetzen (alles, auch LUT und Keyframes)"));
    connect(resetAll, &QToolButton::clicked, this,
            [this] { m_editor->resetGrade(targets(), {}, m_playhead, T("Farbkorrektur zurücksetzen")); });

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

    // Räder
    auto* wheels = new QHBoxLayout;
    wheels->setSpacing(12);
    wheels->addStretch(1);
    for (Wheel& w : m_wheels) wheels->addWidget(buildWheel(w));
    wheels->addStretch(1);

    // Regler darunter (wie die Leiste unter den DaVinci-Rädern)
    m_sliders = {{P::GradeContrast}, {P::GradePivot}, {P::GradeSaturation},
                 {P::GradeTemp},     {P::GradeTint},  {P::GradeExposure}};
    auto* bar = new QHBoxLayout;
    bar->setSpacing(14);
    bar->addStretch(1);
    bar->addWidget(buildSlider(m_sliders[0], T("Kontrast"), 0, 2, 0.002, 3));
    bar->addWidget(buildSlider(m_sliders[1], "Pivot", 0, 1, 0.001, 3));
    bar->addWidget(buildSlider(m_sliders[2], T("Sättigung"), 0, 100, 0.1, 2));
    bar->addWidget(buildSlider(m_sliders[3], T("Temperatur"), -4000, 4000, 5, 1));
    bar->addWidget(buildSlider(m_sliders[4], T("Tönung"), -100, 100, 0.1, 2));
    bar->addWidget(buildSlider(m_sliders[5], T("Belichtung"), -4, 4, 0.01, 2));

    // LUT
    auto* lutLabel = new QLabel("LUT");
    lutLabel->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    m_lutName = new QLabel;
    m_lutName->setMinimumWidth(120);
    auto* lutLoad = new QToolButton;
    lutLoad->setText(T("Laden…"));
    lutLoad->setToolTip(T("3D-LUT (.cube) für diesen Clip laden, wird nach der Korrektur angewendet"));
    connect(lutLoad, &QToolButton::clicked, this, &ColorPanel::loadLut);
    m_lutClear = smallButton("✕", T("LUT entfernen"));
    connect(m_lutClear, &QToolButton::clicked, this, [this] { m_editor->setGradeLut(targets(), {}); });
    bar->addSpacing(20);
    bar->addWidget(lutLabel);
    bar->addWidget(m_lutName);
    bar->addWidget(lutLoad);
    bar->addWidget(m_lutClear);
    bar->addStretch(1);

    m_body = new QWidget;
    auto* bl = new QVBoxLayout(m_body);
    bl->setContentsMargins(8, 8, 8, 8);
    bl->setSpacing(10);
    bl->addLayout(wheels);
    bl->addLayout(bar);
    bl->addStretch(1);

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

QToolButton* ColorPanel::smallButton(const QString& text, const QString& tip)
{
    auto* b = new QToolButton;
    b->setText(text);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::NoFocus);
    b->setStyleSheet(QString("QToolButton { color: %1; border: none; padding: 0 3px; }"
                             "QToolButton:disabled { color: #4a4a50; }").arg(Theme::textDim.name()));
    return b;
}

QWidget* ColorPanel::buildWheel(Wheel& w)
{
    auto* box = new QWidget;
    auto* lay = new QVBoxLayout(box);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(4);

    auto* head = new QHBoxLayout;
    auto* name = new QLabel(w.name); // wie DaVinci auch im deutschen Resolve englisch
    name->setStyleSheet(QString("color: %1;").arg(Theme::text.name()));
    auto* reset = smallButton("↺", T("%1 zurücksetzen").arg(w.name));
    const QVector<AnimParam> params{w.p[0], w.p[1], w.p[2], w.p[3]};
    connect(reset, &QToolButton::clicked, this, [this, params, n = w.name] {
        m_editor->resetGrade(targets(), params, m_playhead, T("%1 zurücksetzen").arg(n));
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
        // Helligkeitsanteil der R/G/B-Werte bleibt, nur die Balance folgt dem Punkt
        const int t = localFrame(*c);
        double d[3];
        for (int i = 0; i < 3; ++i) d[i] = Keys::valueAt(*c, w.p[i + 1], t) - w.neutral;
        const double y = kWr * d[0] + kWg * d[1] + kWb * d[2];
        double rgb[3];
        uvToRgb(u * w.range, v * w.range, rgb);
        setValues({{w.p[1], w.neutral + y + rgb[0]}, {w.p[2], w.neutral + y + rgb[1]}, {w.p[3], w.neutral + y + rgb[2]}},
                  w.name, "color:wheel:" + w.name);
    };
    w.wheel->onFinish = [this] { finish(); };

    // Zahlenfelder Y R G B (Y = Master, wie das Rad unter den DaVinci-Farbrädern)
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
        f->onChange = [this, p, n = w.name](double v) { setValues({{p, v}}, n, QString("color:%1").arg(int(p))); };
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
    s.field->onChange = [this, p, label](double v) { setValues({{p, v}}, label, QString("color:%1").arg(int(p))); };
    s.field->onFinish = [this] { finish(); };
    lay->addWidget(l);
    lay->addWidget(s.field, 0, Qt::AlignHCenter);
    // Doppelklick auf die Beschriftung = Standardwert (wie DaVinci)
    l->setToolTip(T("Doppelklick = zurücksetzen"));
    l->installEventFilter(this);
    l->setProperty("gradeParam", int(p));
    return box;
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
    if (dir.isEmpty() || !QFileInfo(dir).isDir()) dir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    const QString path = QFileDialog::getOpenFileName(this, T("LUT laden"), dir, T("LUT-Dateien (*.cube)"));
    if (path.isEmpty()) return;
    settings.setValue("color/lutDir", QFileInfo(path).absolutePath());
    QString error;
    if (!ColorGrade::loadCube(path, &error)) {
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
    refresh();
}

bool ColorPanel::eventFilter(QObject* obj, QEvent* e)
{
    if (e->type() == QEvent::MouseButtonDblClick && obj->property("gradeParam").isValid()) {
        const auto p = AnimParam(obj->property("gradeParam").toInt());
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
        return;
    }
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
    auto value = [&](AnimParam p) { return Keys::valueAt(*c, p, t); }; // ohne Korrektur: Standardwerte
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
    m_lutName->setText(lut.isEmpty() ? T("keine") : QFileInfo(lut).fileName() + (lutOk ? QString() : T(" (fehlt)")));
    m_lutName->setToolTip(lut);
    m_lutName->setStyleSheet(QString("color: %1;").arg(lutOk ? Theme::text.name() : "#e8414a"));
    m_lutClear->setEnabled(!lut.isEmpty());

    // Keyframe-Raute: rot = Keyframe am Playhead, hell = Korrektur animiert
    const QVector<AnimParam>& params = ColorGrade::animParams();
    const bool animated = std::any_of(params.begin(), params.end(), [&](AnimParam p) { return Keys::animated(*c, p); });
    const bool here = animated && Keys::keyAt(*c, AnimParam::GradeLiftY, t);
    m_keyDiamond->setText(here ? "◆" : "◇");
    m_keyDiamond->setStyleSheet(QString("QToolButton { color: %1; border: none; padding: 0 3px; font-size: 11pt; }")
                                    .arg(here ? "#e8414a" : animated ? Theme::text.name() : Theme::textDim.name()));
    bool before = false, after = false;
    for (int k : Keys::keyTimes(*c, params)) {
        if (k < 0 || k >= c->length()) continue;
        before |= k < t;
        after |= k > t;
    }
    m_keyPrev->setEnabled(before);
    m_keyNext->setEnabled(after);
}
