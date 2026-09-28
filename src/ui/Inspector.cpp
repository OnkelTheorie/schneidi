#include "ui/Inspector.h"

#include "app/Theme.h"
#include "core/EffectRegistry.h"
#include "core/Editor.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "ui/ScrubField.h"

#include <QButtonGroup>
#include <QColorDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

namespace {

constexpr int kSliderSteps = 1000;

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
    inst.enabled = false; // erst über den roten Punkt (oder eine Einstellung) einschalten
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

// Roter Punkt wie in DaVinci: an = rot, aus = grau
QToolButton* makeDot()
{
    auto* dot = new QToolButton;
    dot->setCheckable(true);
    dot->setFixedSize(10, 10);
    dot->setFocusPolicy(Qt::NoFocus);
    dot->setToolTip("Bereich an/aus");
    dot->setStyleSheet("QToolButton { border: none; border-radius: 5px; background: #55555c; }"
                       "QToolButton:checked { background: #e8414a; }");
    return dot;
}

QLabel* axisLabel(const QString& t)
{
    auto* l = new QLabel(t);
    l->setStyleSheet(QString("color: %1; font-size: 8pt;").arg(Theme::textDim.name()));
    return l;
}

// Kettensymbol (Zoom X/Y gekoppelt): an = hell, aus = gedimmt
QIcon chainIcon()
{
    auto draw = [](const QColor& col) {
        QPixmap pm(32, 32);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(col, 3));
        p.translate(16, 16);
        p.rotate(-45);
        p.drawRoundedRect(QRectF(-13, -5, 15, 10), 5, 5);
        p.drawRoundedRect(QRectF(-2, -5, 15, 10), 5, 5);
        return pm;
    };
    QIcon icon;
    icon.addPixmap(draw(Theme::textDim), QIcon::Normal, QIcon::Off);
    icon.addPixmap(draw(Theme::text), QIcon::Normal, QIcon::On);
    return icon;
}

int sliderPos(double v, double min, double max)
{
    return int(std::lround((v - min) / std::max(1e-9, max - min) * kSliderSteps));
}

} // namespace

Inspector::Inspector(Editor* editor, QWidget* parent) : QWidget(parent), m_editor(editor)
{
    setObjectName("Panel");
    auto* title = new QLabel("Inspector");
    title->setObjectName("PanelTitle");

    m_empty = new QLabel("Kein Clip ausgewählt");
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));

    // ---- Tabs oben (Symbol über Text wie in DaVinci) ----
    auto* tabBar = new QWidget;
    tabBar->setObjectName("InspectorTabs");
    tabBar->setStyleSheet(QString("QWidget#InspectorTabs { background: %1; border-bottom: 1px solid %2; }"
                                  "QToolButton { color: %3; border: none; background: transparent; padding: 4px 14px; }"
                                  "QToolButton:checked { color: %4; }"
                                  "QToolButton:disabled { color: #4a4a52; }")
                              .arg(Theme::panel.name(), Theme::border.name(), Theme::textDim.name(), Theme::text.name()));
    auto* tabLay = new QHBoxLayout(tabBar);
    tabLay->setContentsMargins(4, 2, 4, 2);
    tabLay->setSpacing(0);
    m_tabs = new QButtonGroup(this);
    m_pages = new QStackedWidget;
    const struct { const char* icon; const char* text; } tabs[] = {{"▣", "Video"}, {"♫", "Audio"}};
    for (int i = 0; i < 2; ++i) {
        auto* b = new QToolButton;
        b->setText(QString("%1\n%2").arg(tabs[i].icon, tabs[i].text));
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);
        m_tabs->addButton(b, i);
        tabLay->addWidget(b);
    }
    tabLay->addStretch(1);
    connect(m_tabs, &QButtonGroup::idClicked, m_pages, &QStackedWidget::setCurrentIndex);

    auto makePage = [this]() {
        auto* page = new QWidget;
        auto* lay = new QVBoxLayout(page);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);
        auto* scroll = new QScrollArea;
        scroll->setWidget(page);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        m_pages->addWidget(scroll);
        return lay;
    };
    QVBoxLayout* videoLay = makePage();
    QVBoxLayout* audioLay = makePage();

    const TrackKind V = TrackKind::Video;

    // ---- Transform ----
    Section tr = addSection(videoLay, V, "Transform", [](Clip& c) {
        const ClipTransform d;
        c.transform.zoomX = d.zoomX;
        c.transform.zoomY = d.zoomY;
        c.transform.posX = d.posX;
        c.transform.posY = d.posY;
        c.transform.rotation = d.rotation;
        c.transform.transformOn = true;
    }, [](Clip& c) -> bool& { return c.transform.transformOn; });

    auto* link = new QToolButton;
    link->setIcon(chainIcon());
    link->setIconSize(QSize(16, 16));
    link->setStyleSheet("QToolButton { border: none; background: transparent; }");
    link->setToolTip("Zoom X/Y gekoppelt");
    link->setCheckable(true);
    link->setChecked(true);
    link->setAutoRaise(true);
    link->setFocusPolicy(Qt::NoFocus);
    link->setFixedSize(20, 20);
    connect(link, &QToolButton::toggled, this, [this](bool on) { m_zoomLinked = on; });
    const auto zoom = addXY(tr, "zoom", "Zoom", 0.0, 100.0, 1.0, 0.005, 3,
                            [](Clip& c) -> double& { return c.transform.zoomX; },
                            [](Clip& c) -> double& { return c.transform.zoomY; }, link);
    // gekoppelt: X und Y gemeinsam ändern
    zoom.first->edit->onChange = [this, zy = zoom.second](double v) {
        if (m_zoomLinked) zy->edit->setValue(v);
        apply(TrackKind::Video, "zoom", "Zoom", [this, v](Clip& c) {
            c.transform.zoomX = v;
            if (m_zoomLinked) c.transform.zoomY = v;
        });
    };
    zoom.second->edit->onChange = [this, zx = zoom.first](double v) {
        if (m_zoomLinked) zx->edit->setValue(v);
        apply(TrackKind::Video, "zoom", "Zoom", [this, v](Clip& c) {
            c.transform.zoomY = v;
            if (m_zoomLinked) c.transform.zoomX = v;
        });
    };
    addXY(tr, "pos", "Position", -10000, 10000, 0, 1, 3, [](Clip& c) -> double& { return c.transform.posX; },
          [](Clip& c) -> double& { return c.transform.posY; });
    addSlider(tr, "rotation", "Rotation", -360, 360, 0, 0.2, 3, [](Clip& c) -> double& { return c.transform.rotation; });

    // ---- Beschneiden ----
    Section crop = addSection(videoLay, V, "Beschneiden", [](Clip& c) {
        c.transform.cropLeft = c.transform.cropRight = c.transform.cropTop = c.transform.cropBottom = 0;
        c.transform.cropOn = true;
    }, [](Clip& c) -> bool& { return c.transform.cropOn; });
    m_crop[0] = addSlider(crop, "cropLeft", "Links", 0, 960, 0, 1, 3, [](Clip& c) -> double& { return c.transform.cropLeft; });
    m_crop[1] = addSlider(crop, "cropRight", "Rechts", 0, 960, 0, 1, 3, [](Clip& c) -> double& { return c.transform.cropRight; });
    m_crop[2] = addSlider(crop, "cropTop", "Oben", 0, 540, 0, 1, 3, [](Clip& c) -> double& { return c.transform.cropTop; });
    m_crop[3] = addSlider(crop, "cropBottom", "Unten", 0, 540, 0, 1, 3, [](Clip& c) -> double& { return c.transform.cropBottom; });

    // ---- Composite ----
    Section comp = addSection(videoLay, V, "Composite", [](Clip& c) {
        c.transform.opacity = 100;
        c.transform.compositeOn = true;
    }, [](Clip& c) -> bool& { return c.transform.compositeOn; });
    addSlider(comp, "opacity", "Deckkraft", 0, 100, 100, 0.5, 2, [](Clip& c) -> double& { return c.transform.opacity; });

    // ---- Green Screen (Effekt aus der EffectRegistry) ----
    Section key = addSection(videoLay, V, "Green Screen", [](Clip& c) {
        c.effects.removeIf([](const EffectInstance& x) { return x.effectId == "chromakey"; });
    }, [](Clip& c) -> bool& { return ensureEffect(c, "chromakey").enabled; });
    auto* swatch = new QToolButton;
    swatch->setFixedSize(46, 18);
    swatch->setFocusPolicy(Qt::NoFocus);
    swatch->setToolTip("Key-Farbe wählen – im Dialog gibt es eine Pipette zum Picken im Viewer");
    connect(swatch, &QToolButton::clicked, this, [this] {
        const Clip* c = primary(TrackKind::Video);
        if (!c) return;
        const QColor col = QColorDialog::getColor(effectParam(*c, "chromakey", "color").value<QColor>(), this,
                                                  "Key-Farbe wählen");
        if (!col.isValid()) return;
        apply(TrackKind::Video, {}, "Key-Farbe", [col](Clip& clip) {
            EffectInstance& e = ensureEffect(clip, "chromakey");
            e.params["color"] = col;
            e.enabled = true;
        });
    });
    m_refreshers << [this, swatch] {
        if (const Clip* c = primary(TrackKind::Video)) {
            const QColor col = effectParam(*c, "chromakey", "color").value<QColor>();
            swatch->setStyleSheet(QString("QToolButton { background: %1; border: 1px solid #141417; border-radius: 2px; }")
                                      .arg(col.name()));
        }
    };
    auto* colorBox = new QHBoxLayout;
    colorBox->setContentsMargins(0, 0, 0, 0);
    colorBox->addStretch(1);
    colorBox->addWidget(swatch);
    key.grid->addWidget(rowLabel("Farbe"), key.rows, 0);
    key.grid->addLayout(colorBox, key.rows, 1);
    ++key.rows;
    // Registry speichert 0..1, angezeigt wird Prozent; eigenes onChange schreibt in den Effekt
    Param* tol = addSlider(key, "keyTolerance", "Toleranz", 0, 100, 30, 0.5, 2, [](Clip& c) -> double& {
        static double shown;
        shown = effectParam(c, "chromakey", "distance").toDouble() * 100.0;
        return shown;
    });
    tol->edit->onChange = [this, tol](double v) {
        const QSignalBlocker b(tol->slider);
        tol->slider->setValue(sliderPos(v, tol->min, tol->max));
        apply(TrackKind::Video, "keyTolerance", "Key-Toleranz", [v](Clip& c) {
            EffectInstance& e = ensureEffect(c, "chromakey");
            e.params["distance"] = v / 100.0;
            e.enabled = true;
        });
    };
    videoLay->addStretch(1);

    // ---- Audio ----
    const TrackKind A = TrackKind::Audio;
    Section vol = addSection(audioLay, A, "Lautstärke", [](Clip& c) {
        c.volumeDb = 0;
        c.pan = 0;
    });
    Param* volume = addSlider(vol, "volume", "Lautstärke", kMinVolumeDb, kMaxVolumeDb, 0, 0.1, 2,
                              [](Clip& c) -> double& { return c.volumeDb; });
    volume->edit->setMinimumText("-∞");
    addSlider(vol, "pan", "Pan", -100, 100, 0, 1, 2, [](Clip& c) -> double& { return c.pan; });
    audioLay->addStretch(1);

    // ---- Gesamtaufbau ----
    m_clipName = new QLabel;
    m_clipName->setAlignment(Qt::AlignCenter);
    m_clipName->setContentsMargins(8, 4, 8, 4);
    m_clipName->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));

    m_content = new QWidget;
    auto* cl = new QVBoxLayout(m_content);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);
    cl->addWidget(tabBar);
    cl->addWidget(m_clipName);
    cl->addWidget(m_pages, 1);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(title);
    lay->addWidget(m_content, 1);
    lay->addWidget(m_empty, 1);

    m_tabs->button(0)->setChecked(true);
    connect(editor->selection(), &Selection::changed, this, &Inspector::refresh);
    connect(editor->project(), &Project::timelineChanged, this, &Inspector::refresh);
    refresh();
}

Inspector::~Inspector()
{
    qDeleteAll(m_params);
}

void Inspector::setFrameSize(const QSize& size)
{
    m_frameSize = size;
    for (int i = 0; i < 4; ++i) {
        const double max = (i < 2 ? size.width() : size.height()) / 2.0;
        m_crop[i]->max = max;
        m_crop[i]->edit->setRange(0, max);
    }
    refresh();
}

QToolButton* Inspector::resetButton(const std::function<void()>& fn)
{
    auto* b = new QToolButton;
    b->setText("↺");
    b->setToolTip("Zurücksetzen");
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::NoFocus);
    b->setStyleSheet(QString("QToolButton { color: %1; }").arg(Theme::textDim.name()));
    connect(b, &QToolButton::clicked, this, fn);
    return b;
}

QLabel* Inspector::rowLabel(const QString& text)
{
    auto* l = new QLabel(text);
    l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    l->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    return l;
}

Inspector::Section Inspector::addSection(QVBoxLayout* page, TrackKind kind, const QString& title,
                                         const std::function<void(Clip&)>& reset, const Flag& enabled)
{
    auto* header = new QWidget;
    header->setObjectName("InspectorSection");
    header->setStyleSheet(QString("QWidget#InspectorSection { background: %1; border-top: 1px solid %2; }")
                              .arg(Theme::panelHeader.name(), Theme::border.name()));
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(10, 3, 4, 3);
    hl->setSpacing(8);

    if (enabled) {
        QToolButton* dot = makeDot();
        connect(dot, &QToolButton::clicked, this, [this, kind, title, enabled](bool on) {
            apply(kind, {}, title + (on ? " an" : " aus"), [enabled, on](Clip& c) { enabled(c) = on; });
        });
        m_refreshers << [this, dot, kind, enabled] {
            if (const Clip* c = primary(kind)) {
                Clip copy = *c;
                const QSignalBlocker b(dot);
                dot->setChecked(enabled(copy));
            }
        };
        hl->addWidget(dot);
    } else {
        hl->addSpacing(10);
    }

    auto* toggle = new QToolButton;
    toggle->setText(title);
    toggle->setCheckable(true);
    toggle->setChecked(true);
    toggle->setToolTip("Auf-/zuklappen");
    toggle->setFocusPolicy(Qt::NoFocus);
    toggle->setStyleSheet(QString("QToolButton, QToolButton:checked { color: %1; border: none; background: transparent; }")
                              .arg(Theme::text.name()));
    hl->addWidget(toggle);
    hl->addStretch(1);
    hl->addWidget(resetButton([this, kind, title, reset] { apply(kind, {}, title + " zurücksetzen", reset); }));

    auto* body = new QWidget;
    auto* grid = new QGridLayout(body);
    grid->setContentsMargins(10, 6, 4, 8);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(4);
    grid->setColumnMinimumWidth(0, 90);
    grid->setColumnStretch(1, 1);
    connect(toggle, &QToolButton::toggled, body, &QWidget::setVisible);

    page->addWidget(header);
    page->addWidget(body);
    return {grid, kind, 0};
}

Inspector::Param* Inspector::makeParam(TrackKind kind, const QString& key, const QString& text, double min,
                                       double max, double def, double step, int decimals, const Field& field)
{
    auto* p = new Param{kind, field, new ScrubField(min, max, step, decimals), nullptr, min, max};
    p->edit->setValue(def);
    p->edit->onChange = [this, p, kind, key, text, field](double v) {
        if (p->slider) {
            const QSignalBlocker b(p->slider);
            p->slider->setValue(sliderPos(v, p->min, p->max));
        }
        apply(kind, key, text, [field, v](Clip& c) { field(c) = v; });
    };
    p->edit->onFinish = [this] { m_editor->project()->closeMerge(); };
    m_params << p;
    return p;
}

Inspector::Param* Inspector::addSlider(Section& s, const QString& key, const QString& label, double min, double max,
                                       double def, double step, int decimals, const Field& field)
{
    Param* p = makeParam(s.kind, key, label, min, max, def, step, decimals, field);
    auto* slider = new QSlider(Qt::Horizontal);
    slider->setRange(0, kSliderSteps);
    slider->setFocusPolicy(Qt::NoFocus); // Pfeiltasten bleiben bei der Timeline
    slider->setValue(sliderPos(def, min, max));
    p->slider = slider;
    connect(slider, &QSlider::valueChanged, this, [p](int v) {
        p->edit->setValue(p->min + (p->max - p->min) * v / kSliderSteps);
        const QSignalBlocker b(p->slider); // Rückkopplung vermeiden
        if (p->edit->onChange) p->edit->onChange(p->edit->value());
    });
    connect(slider, &QSlider::sliderReleased, this, [this] { m_editor->project()->closeMerge(); });

    auto* box = new QHBoxLayout;
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(8);
    box->addWidget(slider, 1);
    box->addWidget(p->edit);
    s.grid->addWidget(rowLabel(label), s.rows, 0);
    s.grid->addLayout(box, s.rows, 1);
    s.grid->addWidget(resetButton([p, def] {
        p->edit->setValue(def);
        if (p->edit->onChange) p->edit->onChange(def);
        if (p->edit->onFinish) p->edit->onFinish();
    }), s.rows, 2);
    ++s.rows;
    return p;
}

QPair<Inspector::Param*, Inspector::Param*> Inspector::addXY(Section& s, const QString& key, const QString& label,
                                                             double min, double max, double def, double step,
                                                             int decimals, const Field& x, const Field& y,
                                                             QToolButton* link)
{
    Param* px = makeParam(s.kind, key + "X", label + " X", min, max, def, step, decimals, x);
    Param* py = makeParam(s.kind, key + "Y", label + " Y", min, max, def, step, decimals, y);
    auto* box = new QHBoxLayout;
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(4);
    box->addStretch(1);
    box->addWidget(axisLabel("X"));
    box->addWidget(px->edit);
    if (link) box->addWidget(link);
    else box->addSpacing(20 + box->spacing()); // Felder bündig mit der Zoom-Zeile
    box->addWidget(axisLabel("Y"));
    box->addWidget(py->edit);
    s.grid->addWidget(rowLabel(label), s.rows, 0);
    s.grid->addLayout(box, s.rows, 1);
    s.grid->addWidget(resetButton([this, kind = s.kind, label, x, y, def] {
        apply(kind, {}, label + " zurücksetzen", [x, y, def](Clip& c) {
            x(c) = def;
            y(c) = def;
        });
    }), s.rows, 2);
    ++s.rows;
    return {px, py};
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
    // key leer = eigener Undo-Schritt, sonst werden Ziehbewegungen zusammengefasst
    m_editor->modifyClips(selectedIds(kind), text, fn, key.isEmpty() ? QString() : "inspector:" + key);
}

void Inspector::refresh()
{
    const Clip* v = primary(TrackKind::Video);
    const Clip* a = primary(TrackKind::Audio);
    const bool any = v || a;
    m_content->setVisible(any);
    m_empty->setVisible(!any);
    if (!any) return;

    const Clip* shown = v ? v : a;
    const int count = m_editor->selection()->ids().size();
    QString name = QFileInfo(shown->mediaPath).fileName();
    if (count > (v && a ? 2 : 1)) name += QString("  (+%1)").arg(count - 1);
    m_clipName->setText(name);
    m_clipName->setToolTip(shown->mediaPath);

    // Tabs ohne passenden Clip ausgrauen wie in DaVinci
    m_tabs->button(0)->setEnabled(v);
    m_tabs->button(1)->setEnabled(a);
    int page = m_pages->currentIndex();
    if ((page == 0 && !v) || (page == 1 && !a)) page = v ? 0 : 1;
    m_tabs->button(page)->setChecked(true);
    m_pages->setCurrentIndex(page);

    for (Param* p : m_params) {
        const Clip* c = p->kind == TrackKind::Video ? v : a;
        if (!c) continue;
        Clip copy = *c;
        const double val = p->field(copy);
        p->edit->setValue(val);
        if (p->slider) {
            const QSignalBlocker b(p->slider);
            p->slider->setValue(sliderPos(val, p->min, p->max));
        }
    }
    for (const auto& fn : m_refreshers) fn();
}
