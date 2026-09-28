#include "ui/Inspector.h"

#include "app/Theme.h"
#include "core/EffectRegistry.h"
#include "core/Editor.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

// Eine Zeile wie im DaVinci-Inspector: Name, Schieberegler, Zahlenfeld, Zurücksetzen
class ParamRow : public QWidget {
public:
    ParamRow(const QString& label, double min, double max, double def, int decimals, QWidget* parent = nullptr)
        : QWidget(parent), m_def(def)
    {
        auto* name = new QLabel(label);
        m_name = name;
        name->setFixedWidth(kNameW);
        name->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));

        m_slider = new QSlider(Qt::Horizontal);
        m_slider->setRange(0, kSteps);
        m_slider->setFocusPolicy(Qt::NoFocus); // Pfeiltasten bleiben bei der Timeline

        m_spin = new QDoubleSpinBox;
        m_spin->setDecimals(decimals);
        m_spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        m_spin->setAlignment(Qt::AlignRight);
        m_spin->setFixedWidth(72);
        m_spin->setKeyboardTracking(false);

        auto* reset = new QToolButton;
        reset->setText("↺");
        reset->setToolTip("Zurücksetzen");
        reset->setAutoRaise(true);

        setRange(min, max);

        auto* lay = new QHBoxLayout(this);
        m_layout = lay;
        lay->setContentsMargins(10, 1, 4, 1);
        lay->setSpacing(6);
        lay->addWidget(name);
        lay->addWidget(m_slider, 1);
        lay->addWidget(m_spin);
        lay->addWidget(reset);

        connect(m_slider, &QSlider::valueChanged, this, [this](int v) {
            const double val = m_min + (m_max - m_min) * v / kSteps;
            const QSignalBlocker b(m_spin);
            m_spin->setValue(val);
            if (onChange) onChange(m_spin->value());
        });
        connect(m_slider, &QSlider::sliderReleased, this, [this] { if (onFinish) onFinish(); });
        connect(m_spin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            syncSlider(v);
            if (onChange) onChange(v);
            if (onFinish) onFinish();
        });
        connect(m_spin, &QDoubleSpinBox::editingFinished, m_spin, [this] { m_spin->clearFocus(); });
        connect(reset, &QToolButton::clicked, this, [this] {
            setValue(m_def);
            if (onChange) onChange(m_def);
            if (onFinish) onFinish();
        });
    }

    void setRange(double min, double max)
    {
        m_min = min;
        m_max = max;
        const QSignalBlocker b(m_spin);
        m_spin->setRange(min, max);
        syncSlider(m_spin->value());
    }
    void setValue(double v)
    {
        const QSignalBlocker b(m_spin);
        m_spin->setValue(v);
        syncSlider(v);
    }
    // Text für den kleinsten Wert (z. B. "-∞" bei der Lautstärke)
    void setMinimumText(const QString& t) { m_spin->setSpecialValueText(t); }
    void setSuffix(const QString& s) { m_spin->setSuffix(s); }
    // Zusatzknopf rechts neben dem Namen (z. B. Kette für Zoom X/Y); Regler bleiben bündig
    void addExtra(QWidget* w)
    {
        m_layout->insertWidget(1, w);
        m_name->setFixedWidth(kNameW - w->sizeHint().width() - m_layout->spacing());
    }

    std::function<void(double)> onChange; // während des Ziehens
    std::function<void()> onFinish;       // Loslassen / Eingabe fertig

private:
    static constexpr int kSteps = 1000;
    static constexpr int kNameW = 78;
    void syncSlider(double v)
    {
        const QSignalBlocker b(m_slider);
        m_slider->setValue(int(std::lround((v - m_min) / std::max(1e-9, m_max - m_min) * kSteps)));
    }

    QHBoxLayout* m_layout;
    QLabel* m_name;
    QSlider* m_slider;
    QDoubleSpinBox* m_spin;
    double m_min = 0, m_max = 1, m_def;
};

namespace {

EffectInstance* findEffect(Clip& c, const QString& id)
{
    for (auto& e : c.effects)
        if (e.effectId == id) return &e;
    return nullptr;
}

EffectInstance& ensureEffect(Clip& c, const QString& id)
{
    if (EffectInstance* e = findEffect(c, id)) return *e;
    EffectInstance inst;
    inst.effectId = id;
    if (const EffectDescriptor* d = EffectRegistry::find(id))
        for (const auto& p : d->params) inst.params[p.key] = p.defaultValue;
    c.effects << inst;
    return c.effects.last();
}

QVariant effectParam(const Clip& c, const QString& effectId, const QString& key)
{
    for (const auto& e : c.effects)
        if (e.effectId == effectId && e.params.contains(key)) return e.params.value(key);
    if (const EffectDescriptor* d = EffectRegistry::find(effectId))
        for (const auto& p : d->params)
            if (p.key == key) return p.defaultValue;
    return {};
}

void setColorButton(QToolButton* b, const QColor& c)
{
    b->setStyleSheet(QString("QToolButton { background: %1; border: 1px solid #141417; min-width: 40px; }").arg(c.name()));
    b->setToolTip(c.name());
}

} // namespace

Inspector::Inspector(Editor* editor, QWidget* parent) : QWidget(parent), m_editor(editor)
{
    setObjectName("Panel");
    auto* title = new QLabel("Inspector");
    title->setObjectName("PanelTitle");

    m_clipName = new QLabel;
    m_clipName->setContentsMargins(10, 6, 10, 6);
    m_clipName->setStyleSheet("font-weight: 600;");
    m_empty = new QLabel("Kein Clip ausgewählt");
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));

    auto makePage = [](QWidget*& page) {
        page = new QWidget;
        auto* lay = new QVBoxLayout(page);
        lay->setContentsMargins(0, 4, 0, 4);
        lay->setSpacing(0);
        auto* scroll = new QScrollArea;
        scroll->setWidget(page);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        return std::make_pair(scroll, lay);
    };
    auto [videoScroll, videoLay] = makePage(m_videoPage);
    auto [audioScroll, audioLay] = makePage(m_audioPage);
    m_tabs = new QTabWidget;
    m_tabs->setDocumentMode(true);
    m_tabs->addTab(videoScroll, "Video");
    m_tabs->addTab(audioScroll, "Audio");

    // ---- Video: Transform ----
    const TrackKind V = TrackKind::Video;
    auto resetTransform = [this] {
        apply(TrackKind::Video, {}, "Transform zurücksetzen", [](Clip& c) {
            const ClipTransform d;
            c.transform.zoomX = d.zoomX;
            c.transform.zoomY = d.zoomY;
            c.transform.posX = d.posX;
            c.transform.posY = d.posY;
            c.transform.rotation = d.rotation;
        });
    };
    QVBoxLayout* tr = addSection(videoLay, "Transform", resetTransform);
    ParamRow* zoomX = addParam(tr, V, "zoomX", "Zoom X", 0.0, 5.0, 1.0, 3, [](Clip& c) -> double& { return c.transform.zoomX; });
    ParamRow* zoomY = addParam(tr, V, "zoomY", "Zoom Y", 0.0, 5.0, 1.0, 3, [](Clip& c) -> double& { return c.transform.zoomY; });
    // Zoom X/Y gekoppelt wie das Kettensymbol in DaVinci
    m_zoomLink = new QToolButton;
    m_zoomLink->setText("🔗");
    m_zoomLink->setToolTip("Zoom X/Y gekoppelt");
    m_zoomLink->setCheckable(true);
    m_zoomLink->setChecked(true);
    m_zoomLink->setAutoRaise(true);
    m_zoomLink->setFocusPolicy(Qt::NoFocus);
    m_zoomLink->setFixedSize(20, 20);
    connect(m_zoomLink, &QToolButton::toggled, this, [this](bool on) { m_zoomLinked = on; });
    zoomX->addExtra(m_zoomLink);
    zoomX->onChange = [this, zoomY](double v) {
        if (m_zoomLinked) zoomY->setValue(v);
        apply(TrackKind::Video, "zoom", "Zoom", [this, v](Clip& c) {
            c.transform.zoomX = v;
            if (m_zoomLinked) c.transform.zoomY = v;
        });
    };
    zoomY->onChange = [this, zoomX](double v) {
        if (m_zoomLinked) zoomX->setValue(v);
        apply(TrackKind::Video, "zoom", "Zoom", [this, v](Clip& c) {
            c.transform.zoomY = v;
            if (m_zoomLinked) c.transform.zoomX = v;
        });
    };
    addParam(tr, V, "posX", "Position X", -4000, 4000, 0, 1, [](Clip& c) -> double& { return c.transform.posX; });
    addParam(tr, V, "posY", "Position Y", -4000, 4000, 0, 1, [](Clip& c) -> double& { return c.transform.posY; });
    addParam(tr, V, "rotation", "Rotation", -360, 360, 0, 1, [](Clip& c) -> double& { return c.transform.rotation; })
        ->setSuffix("°");

    // ---- Video: Beschneiden ----
    QVBoxLayout* crop = addSection(videoLay, "Beschneiden", [this] {
        apply(TrackKind::Video, {}, "Beschneiden zurücksetzen", [](Clip& c) {
            c.transform.cropLeft = c.transform.cropRight = c.transform.cropTop = c.transform.cropBottom = 0;
        });
    });
    m_cropRows[0] = addParam(crop, V, "cropLeft", "Links", 0, 960, 0, 0, [](Clip& c) -> double& { return c.transform.cropLeft; });
    m_cropRows[1] = addParam(crop, V, "cropRight", "Rechts", 0, 960, 0, 0, [](Clip& c) -> double& { return c.transform.cropRight; });
    m_cropRows[2] = addParam(crop, V, "cropTop", "Oben", 0, 540, 0, 0, [](Clip& c) -> double& { return c.transform.cropTop; });
    m_cropRows[3] = addParam(crop, V, "cropBottom", "Unten", 0, 540, 0, 0, [](Clip& c) -> double& { return c.transform.cropBottom; });

    // ---- Video: Composite ----
    QVBoxLayout* comp = addSection(videoLay, "Composite", [this] {
        apply(TrackKind::Video, {}, "Deckkraft zurücksetzen", [](Clip& c) { c.transform.opacity = 100; });
    });
    addParam(comp, V, "opacity", "Deckkraft", 0, 100, 100, 1, [](Clip& c) -> double& { return c.transform.opacity; })
        ->setSuffix(" %");

    // ---- Video: Green Screen (Effekt aus der EffectRegistry) ----
    m_keyEnabled = new QCheckBox;
    m_keyEnabled->setToolTip("Green Screen an/aus");
    m_keyEnabled->setFocusPolicy(Qt::NoFocus);
    QVBoxLayout* key = addSection(videoLay, "Green Screen", [this] {
        apply(TrackKind::Video, {}, "Green Screen zurücksetzen", [](Clip& c) {
            if (EffectInstance* e = findEffect(c, "chromakey")) {
                const bool on = e->enabled;
                c.effects.removeIf([](const EffectInstance& x) { return x.effectId == "chromakey"; });
                if (on) ensureEffect(c, "chromakey");
            }
        });
    }, m_keyEnabled);
    connect(m_keyEnabled, &QCheckBox::toggled, this, [this](bool on) {
        apply(TrackKind::Video, {}, on ? "Green Screen an" : "Green Screen aus",
              [on](Clip& c) { ensureEffect(c, "chromakey").enabled = on; });
    });
    m_keyColor = new QToolButton;
    connect(m_keyColor, &QToolButton::clicked, this, [this] {
        const Clip* c = primary(TrackKind::Video);
        if (!c) return;
        // Der Farbdialog hat eine Pipette ("Bildschirmfarbe wählen") -> direkt im Viewer picken
        const QColor col = QColorDialog::getColor(effectParam(*c, "chromakey", "color").value<QColor>(), this,
                                                  "Key-Farbe wählen");
        if (!col.isValid()) return;
        apply(TrackKind::Video, {}, "Key-Farbe", [col](Clip& clip) {
            EffectInstance& e = ensureEffect(clip, "chromakey");
            e.params["color"] = col;
            e.enabled = true;
        });
    });
    auto* colorRow = new QHBoxLayout;
    colorRow->setContentsMargins(10, 2, 4, 2);
    auto* colorLabel = new QLabel("Farbe");
    colorLabel->setFixedWidth(78);
    colorLabel->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    colorRow->addWidget(colorLabel);
    colorRow->addWidget(m_keyColor);
    colorRow->addStretch(1);
    key->addLayout(colorRow);
    m_keyTolerance = new ParamRow("Toleranz", 0, 100, 30, 1);
    m_keyTolerance->setSuffix(" %");
    m_keyTolerance->onChange = [this](double v) {
        apply(TrackKind::Video, "keyTolerance", "Key-Toleranz", [v](Clip& c) {
            EffectInstance& e = ensureEffect(c, "chromakey");
            e.params["distance"] = v / 100.0;
            e.enabled = true;
        });
    };
    m_keyTolerance->onFinish = [this] { m_editor->project()->closeMerge(); };
    key->addWidget(m_keyTolerance);
    videoLay->addStretch(1);

    // ---- Audio ----
    const TrackKind A = TrackKind::Audio;
    QVBoxLayout* vol = addSection(audioLay, "Lautstärke", [this] {
        apply(TrackKind::Audio, {}, "Audio zurücksetzen", [](Clip& c) {
            c.volumeDb = 0;
            c.pan = 0;
        });
    });
    ParamRow* volRow = addParam(vol, A, "volume", "Lautstärke", kMinVolumeDb, kMaxVolumeDb, 0, 1,
                                [](Clip& c) -> double& { return c.volumeDb; });
    volRow->setSuffix(" dB");
    volRow->setMinimumText("-∞ dB");
    addParam(vol, A, "pan", "Pan", -100, 100, 0, 0, [](Clip& c) -> double& { return c.pan; });
    audioLay->addStretch(1);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(title);
    lay->addWidget(m_clipName);
    lay->addWidget(m_tabs, 1);
    lay->addWidget(m_empty, 1);

    connect(editor->selection(), &Selection::changed, this, &Inspector::refresh);
    connect(editor->project(), &Project::timelineChanged, this, &Inspector::refresh);
    refresh();
}

void Inspector::setFrameSize(const QSize& size)
{
    m_frameSize = size;
    m_cropRows[0]->setRange(0, size.width() / 2.0);
    m_cropRows[1]->setRange(0, size.width() / 2.0);
    m_cropRows[2]->setRange(0, size.height() / 2.0);
    m_cropRows[3]->setRange(0, size.height() / 2.0);
    refresh();
}

QVBoxLayout* Inspector::addSection(QVBoxLayout* page, const QString& title, const std::function<void()>& reset,
                                   QCheckBox* enable)
{
    // Kopfzeile wie in DaVinci: aufklappbar, optional Ein/Aus, rechts Zurücksetzen
    auto* header = new QWidget;
    header->setObjectName("InspectorSection");
    header->setStyleSheet(QString("QWidget#InspectorSection { background: %1; border-top: 1px solid %2; }")
                              .arg(Theme::panelHeader.name(), Theme::border.name()));
    auto* toggle = new QToolButton;
    toggle->setText(title);
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle->setArrowType(Qt::DownArrow);
    toggle->setCheckable(true);
    toggle->setChecked(true);
    toggle->setAutoRaise(true);
    toggle->setStyleSheet("font-weight: 600;");
    auto* resetBtn = new QToolButton;
    resetBtn->setText("↺");
    resetBtn->setToolTip("Bereich zurücksetzen");
    resetBtn->setAutoRaise(true);
    connect(resetBtn, &QToolButton::clicked, this, reset);

    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(4, 2, 4, 2);
    if (enable) hl->addWidget(enable);
    hl->addWidget(toggle);
    hl->addStretch(1);
    hl->addWidget(resetBtn);

    auto* body = new QWidget;
    auto* bl = new QVBoxLayout(body);
    bl->setContentsMargins(0, 4, 0, 6);
    bl->setSpacing(0);
    connect(toggle, &QToolButton::toggled, body, [toggle, body](bool open) {
        body->setVisible(open);
        toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    });

    page->addWidget(header);
    page->addWidget(body);
    return bl;
}

ParamRow* Inspector::addParam(QVBoxLayout* section, TrackKind kind, const QString& key, const QString& label,
                              double min, double max, double def, int decimals,
                              const std::function<double&(Clip&)>& field)
{
    auto* row = new ParamRow(label, min, max, def, decimals);
    row->onChange = [this, kind, key, label, field](double v) {
        apply(kind, key, label, [field, v](Clip& c) { field(c) = v; });
    };
    row->onFinish = [this] { m_editor->project()->closeMerge(); };
    section->addWidget(row);
    m_bindings.append({kind, row, field});
    return row;
}

QVector<int> Inspector::selectedIds(TrackKind kind) const
{
    QVector<int> ids;
    const Timeline& tl = m_editor->project()->timeline();
    for (int id : m_editor->selection()->ids()) {
        TrackRef ref;
        if (TimelineOps::findClip(tl, id, &ref) && ref.kind == kind) ids << id;
    }
    return ids;
}

const Clip* Inspector::primary(TrackKind kind) const
{
    // Angezeigt wird der früheste ausgewählte Clip der Art
    const Clip* best = nullptr;
    const Timeline& tl = m_editor->project()->timeline();
    for (int id : selectedIds(kind)) {
        const Clip* c = TimelineOps::findClip(tl, id);
        if (c && (!best || c->start < best->start)) best = c;
    }
    return best;
}

void Inspector::apply(TrackKind kind, const QString& key, const QString& text, const std::function<void(Clip&)>& fn)
{
    // key leer = eigener Undo-Schritt, sonst werden Reglerbewegungen zusammengefasst
    m_editor->modifyClips(selectedIds(kind), text, fn, key.isEmpty() ? QString() : "inspector:" + key);
}

void Inspector::refresh()
{
    const Clip* v = primary(TrackKind::Video);
    const Clip* a = primary(TrackKind::Audio);
    const bool any = v || a;
    m_tabs->setVisible(any);
    m_clipName->setVisible(any);
    m_empty->setVisible(!any);
    if (!any) return;

    const Clip* shown = v ? v : a;
    const int count = m_editor->selection()->ids().size();
    QString name = QFileInfo(shown->mediaPath).fileName();
    if (count > (v && a ? 2 : 1)) name += QString("  (+%1)").arg(count - 1);
    m_clipName->setText(name);
    m_clipName->setToolTip(shown->mediaPath);

    m_tabs->setTabVisible(0, v != nullptr);
    m_tabs->setTabVisible(1, a != nullptr);

    for (const Binding& b : m_bindings) {
        const Clip* c = b.kind == TrackKind::Video ? v : a;
        if (!c) continue;
        Clip copy = *c;
        b.row->setValue(b.field(copy));
    }
    if (v) {
        const EffectInstance* key = nullptr;
        for (const auto& e : v->effects)
            if (e.effectId == "chromakey") key = &e;
        const QSignalBlocker blk(m_keyEnabled);
        m_keyEnabled->setChecked(key && key->enabled);
        setColorButton(m_keyColor, effectParam(*v, "chromakey", "color").value<QColor>());
        m_keyTolerance->setValue(effectParam(*v, "chromakey", "distance").toDouble() * 100.0);
    }
}
