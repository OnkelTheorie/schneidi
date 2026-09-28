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
#include <QComboBox>
#include <QFontComboBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

namespace {

constexpr int kSliderSteps = 1000;
constexpr int kTitlePage = 2;
constexpr int kTransitionPage = 3; // nur bei ausgewähltem Übergang, dann einziger Tab

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

// Ausrichtungs-Symbol: Zeilen unterschiedlicher Länge, links/mittig/rechts
QIcon alignIcon(int align)
{
    auto draw = [align](const QColor& col) {
        QPixmap pm(32, 32);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setPen(QPen(col, 3));
        const int widths[] = {24, 16, 24, 12};
        for (int i = 0; i < 4; ++i) {
            const int w = widths[i];
            const int x = align == 0 ? 4 : align == 2 ? 28 - w : 16 - w / 2;
            p.drawLine(x, 6 + i * 7, x + w, 6 + i * 7);
        }
        return pm;
    };
    QIcon icon;
    icon.addPixmap(draw(Theme::textDim), QIcon::Normal, QIcon::Off);
    icon.addPixmap(draw(Theme::text), QIcon::Normal, QIcon::On);
    return icon;
}

// Umschaltknopf (Fett, Kursiv, Ausrichtung)
QToolButton* toggleButton()
{
    auto* b = new QToolButton;
    b->setCheckable(true);
    b->setFixedSize(26, 22);
    b->setIconSize(QSize(16, 16));
    b->setFocusPolicy(Qt::NoFocus);
    b->setStyleSheet(QString("QToolButton { color: %1; border: 1px solid %2; border-radius: 2px; background: transparent; }"
                             "QToolButton:checked { color: %3; background: %4; }")
                         .arg(Theme::textDim.name(), Theme::border.name(), Theme::text.name(), Theme::panelHeader.name()));
    return b;
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
    // Seiten-Index = Button-ID; "Titel" (nur bei Titelclips, wie DaVinci) steht vorne
    const struct { int id; const char* icon; const char* text; } tabs[] = {
        {kTitlePage, "T", "Titel"}, {kTransitionPage, "⧓", "Übergang"}, {0, "▣", "Video"}, {1, "♫", "Audio"}};
    for (const auto& t : tabs) {
        auto* b = new QToolButton;
        b->setText(QString("%1\n%2").arg(t.icon, t.text));
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);
        m_tabs->addButton(b, t.id);
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
    buildTitlePage(makePage());
    buildTransitionPage(makePage());

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

void Inspector::buildTitlePage(QVBoxLayout* page)
{
    const TrackKind V = TrackKind::Video;
    auto current = [this]() -> const Clip* { return primary(TrackKind::Video, true); };

    // ---- Text (wie DaVinci "Text": Textfeld, Schrift, Größe, Farbe, Stil, Ausrichtung, Position) ----
    Section txt = addSection(page, V, "Text", [](Clip& c) {
        const TitleStyle d;
        TitleStyle& t = c.title; // der Text selbst bleibt
        t.font = d.font;
        t.size = d.size;
        t.color = d.color;
        t.bold = d.bold;
        t.italic = d.italic;
        t.align = d.align;
        t.posX = d.posX;
        t.posY = d.posY;
    }, {}, true);

    auto* edit = new QPlainTextEdit;
    edit->setFixedHeight(76);
    edit->setPlaceholderText("Text");
    edit->setTabChangesFocus(true);
    // Live: jede Änderung sofort in die Vorschau, Tippen = ein Undo-Schritt
    connect(edit, &QPlainTextEdit::textChanged, this, [this, edit, V] {
        const QString text = edit->toPlainText();
        apply(V, "titleText", "Titeltext", [text](Clip& c) { c.title.text = text; }, true);
    });
    m_refreshers << [edit, current] {
        const Clip* c = current();
        if (c && edit->toPlainText() != c->title.text) {
            const QSignalBlocker b(edit);
            edit->setPlainText(c->title.text);
        }
    };
    txt.grid->addWidget(edit, txt.rows++, 0, 1, 3);

    auto* fontBox = new QFontComboBox;
    fontBox->setFocusPolicy(Qt::ClickFocus);
    fontBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); // sonst sprengt die Liste die Breite
    fontBox->setMinimumContentsLength(8);
    fontBox->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    connect(fontBox, &QFontComboBox::currentFontChanged, this, [this, V](const QFont& f) {
        const QString family = f.family();
        apply(V, {}, "Schriftart", [family](Clip& c) { c.title.font = family; }, true);
    });
    m_refreshers << [fontBox, current] {
        if (const Clip* c = current(); c && fontBox->currentFont().family() != c->title.font) {
            const QSignalBlocker b(fontBox);
            fontBox->setCurrentFont(QFont(c->title.font));
        }
    };
    txt.grid->addWidget(rowLabel("Schriftart"), txt.rows, 0);
    txt.grid->addWidget(fontBox, txt.rows++, 1);

    addSlider(txt, "titleSize", "Größe", 1, 400, TitleStyle().size, 0.5, 1,
              [](Clip& c) -> double& { return c.title.size; });
    addColor(txt, "Farbe", "Textfarbe", [](Clip& c) -> QColor& { return c.title.color; }, true);

    // Stil + Ausrichtung in einer Zeile
    auto* bold = toggleButton();
    bold->setText("F");
    bold->setToolTip("Fett");
    QFont bf = bold->font();
    bf.setBold(true);
    bold->setFont(bf);
    auto* italic = toggleButton();
    italic->setText("K");
    italic->setToolTip("Kursiv");
    QFont itf = italic->font();
    itf.setItalic(true);
    italic->setFont(itf);
    connect(bold, &QToolButton::clicked, this, [this, V](bool on) {
        apply(V, {}, on ? "Fett an" : "Fett aus", [on](Clip& c) { c.title.bold = on; }, true);
    });
    connect(italic, &QToolButton::clicked, this, [this, V](bool on) {
        apply(V, {}, on ? "Kursiv an" : "Kursiv aus", [on](Clip& c) { c.title.italic = on; }, true);
    });
    auto* styleBox = new QHBoxLayout;
    styleBox->setContentsMargins(0, 0, 0, 0);
    styleBox->setSpacing(2);
    styleBox->addWidget(bold);
    styleBox->addWidget(italic);
    styleBox->addSpacing(12);
    auto* alignGroup = new QButtonGroup(this);
    const char* alignTips[] = {"Linksbündig", "Zentriert", "Rechtsbündig"};
    for (int i = 0; i < 3; ++i) {
        auto* b = toggleButton();
        b->setIcon(alignIcon(i));
        b->setToolTip(alignTips[i]);
        alignGroup->addButton(b, i);
        styleBox->addWidget(b);
    }
    styleBox->addStretch(1);
    connect(alignGroup, &QButtonGroup::idClicked, this, [this, V](int id) {
        apply(V, {}, "Ausrichtung", [id](Clip& c) { c.title.align = id; }, true);
    });
    m_refreshers << [bold, italic, alignGroup, current] {
        if (const Clip* c = current()) {
            bold->setChecked(c->title.bold);
            italic->setChecked(c->title.italic);
            if (auto* b = alignGroup->button(c->title.align)) b->setChecked(true);
        }
    };
    txt.grid->addWidget(rowLabel("Stil"), txt.rows, 0);
    txt.grid->addLayout(styleBox, txt.rows++, 1);

    addXY(txt, "titlePos", "Position", -10000, 10000, 0, 1, 3, [](Clip& c) -> double& { return c.title.posX; },
          [](Clip& c) -> double& { return c.title.posY; });

    // ---- Umrandung / Hintergrund (roter Punkt; Wert ändern schaltet ein) ----
    auto autoOn = [this, V](Param* p, const QString& key, const QString& text, const Flag& on) {
        p->edit->onChange = [this, p, key, text, on, V](double v) {
            const QSignalBlocker b(p->slider);
            p->slider->setValue(sliderPos(v, p->min, p->max));
            apply(V, key, text, [p, v, on](Clip& c) {
                p->field(c) = v;
                on(c) = true;
            }, true);
        };
    };
    const Flag outlineOn = [](Clip& c) -> bool& { return c.title.outlineOn; };
    Section ol = addSection(page, V, "Umrandung", [](Clip& c) {
        const TitleStyle d;
        c.title.outlineColor = d.outlineColor;
        c.title.outlineWidth = d.outlineWidth;
    }, outlineOn, true);
    addColor(ol, "Farbe", "Umrandungsfarbe", [](Clip& c) -> QColor& { return c.title.outlineColor; }, true,
             [](Clip& c) { c.title.outlineOn = true; });
    autoOn(addSlider(ol, "titleOutline", "Breite", 0, 40, TitleStyle().outlineWidth, 0.1, 1,
                     [](Clip& c) -> double& { return c.title.outlineWidth; }),
           "titleOutline", "Umrandung", outlineOn);

    const Flag boxOn = [](Clip& c) -> bool& { return c.title.boxOn; };
    Section box = addSection(page, V, "Hintergrund", [](Clip& c) {
        const TitleStyle d;
        c.title.boxColor = d.boxColor;
        c.title.boxPad = d.boxPad;
    }, boxOn, true);
    addColor(box, "Farbe", "Hintergrundfarbe", [](Clip& c) -> QColor& { return c.title.boxColor; }, true,
             [](Clip& c) { c.title.boxOn = true; });
    autoOn(addSlider(box, "titleBoxPad", "Abstand", 0, 200, TitleStyle().boxPad, 0.5, 1,
                     [](Clip& c) -> double& { return c.title.boxPad; }),
           "titleBoxPad", "Hintergrund", boxOn);
    page->addStretch(1);
}

void Inspector::addColor(Section& s, const QString& label, const QString& text,
                         const std::function<QColor&(Clip&)>& color, bool alpha, const std::function<void(Clip&)>& also)
{
    auto* swatch = new QToolButton;
    swatch->setFixedSize(46, 18);
    swatch->setFocusPolicy(Qt::NoFocus);
    swatch->setToolTip(text + " wählen");
    const TrackKind kind = s.kind;
    const bool title = s.title;
    connect(swatch, &QToolButton::clicked, this, [=] {
        const Clip* c = primary(kind, title);
        if (!c) return;
        Clip copy = *c;
        const QColor col = QColorDialog::getColor(color(copy), this, text + " wählen",
                                                  alpha ? QColorDialog::ShowAlphaChannel : QColorDialog::ColorDialogOptions());
        if (!col.isValid()) return;
        apply(kind, {}, text, [color, col, also](Clip& clip) {
            color(clip) = col;
            if (also) also(clip);
        }, title);
    });
    m_refreshers << [=] {
        if (const Clip* c = primary(kind, title)) {
            Clip copy = *c;
            swatch->setStyleSheet(QString("QToolButton { background: %1; border: 1px solid #141417; border-radius: 2px; }")
                                      .arg(color(copy).name(QColor::HexArgb)));
        }
    };
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->addStretch(1);
    row->addWidget(swatch);
    s.grid->addWidget(rowLabel(label), s.rows, 0);
    s.grid->addLayout(row, s.rows, 1);
    ++s.rows;
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
                                         const std::function<void(Clip&)>& reset, const Flag& enabled, bool titleOnly)
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
        connect(dot, &QToolButton::clicked, this, [this, kind, title, enabled, titleOnly](bool on) {
            apply(kind, {}, title + (on ? " an" : " aus"), [enabled, on](Clip& c) { enabled(c) = on; }, titleOnly);
        });
        m_refreshers << [this, dot, kind, enabled, titleOnly] {
            if (const Clip* c = primary(kind, titleOnly)) {
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
    if (reset)
        hl->addWidget(resetButton([this, kind, title, reset, titleOnly] {
            apply(kind, {}, title + " zurücksetzen", reset, titleOnly);
        }));

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
    return {grid, kind, 0, titleOnly};
}

Inspector::Param* Inspector::makeParam(const Section& s, const QString& key, const QString& text, double min,
                                       double max, double def, double step, int decimals, const Field& field)
{
    auto* p = new Param{s.kind, s.title, field, new ScrubField(min, max, step, decimals), nullptr, min, max};
    p->edit->setValue(def);
    p->edit->onChange = [this, p, key, text, field](double v) {
        if (p->slider) {
            const QSignalBlocker b(p->slider);
            p->slider->setValue(sliderPos(v, p->min, p->max));
        }
        apply(p->kind, key, text, [field, v](Clip& c) { field(c) = v; }, p->title);
    };
    p->edit->onFinish = [this] { m_editor->project()->closeMerge(); };
    m_params << p;
    return p;
}

Inspector::Param* Inspector::addSlider(Section& s, const QString& key, const QString& label, double min, double max,
                                       double def, double step, int decimals, const Field& field)
{
    Param* p = makeParam(s, key, label, min, max, def, step, decimals, field);
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
    Param* px = makeParam(s, key + "X", label + " X", min, max, def, step, decimals, x);
    Param* py = makeParam(s, key + "Y", label + " Y", min, max, def, step, decimals, y);
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
    s.grid->addWidget(resetButton([this, kind = s.kind, title = s.title, label, x, y, def] {
        apply(kind, {}, label + " zurücksetzen", [x, y, def](Clip& c) {
            x(c) = def;
            y(c) = def;
        }, title);
    }), s.rows, 2);
    ++s.rows;
    return {px, py};
}

QVector<int> Inspector::selectedIds(TrackKind kind, bool title) const
{
    QVector<int> ids;
    const Timeline& tl = m_editor->project()->timeline();
    for (int id : m_editor->selection()->ids()) {
        TrackRef ref;
        const Clip* c = TimelineOps::findClip(tl, id, &ref);
        if (c && ref.kind == kind && (!title || c->isTitle())) ids << id;
    }
    return ids;
}

const Clip* Inspector::primary(TrackKind kind, bool title) const
{
    // Angezeigt wird der früheste ausgewählte Clip der Art
    const Clip* best = nullptr;
    const Timeline& tl = m_editor->project()->timeline();
    for (int id : selectedIds(kind, title)) {
        const Clip* c = TimelineOps::findClip(tl, id);
        if (c && (!best || c->start < best->start)) best = c;
    }
    return best;
}

void Inspector::apply(TrackKind kind, const QString& key, const QString& text, const std::function<void(Clip&)>& fn,
                      bool title)
{
    // key leer = eigener Undo-Schritt, sonst werden Ziehbewegungen zusammengefasst
    m_editor->modifyClips(selectedIds(kind, title), text, fn, key.isEmpty() ? QString() : "inspector:" + key);
}

void Inspector::buildTransitionPage(QVBoxLayout* page)
{
    Section s = addSection(page, TrackKind::Video, "Übergang", {});
    auto styleFromUi = [this] {
        TransitionStyle st;
        st.type = TransitionType(m_transType->currentData().toInt());
        st.align = TransitionAlign(m_transAlign->currentIndex());
        return st;
    };
    auto combo = [] {
        auto* c = new QComboBox;
        c->setFocusPolicy(Qt::ClickFocus);
        c->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        return c;
    };

    m_transType = combo();
    for (const auto& i : kTransitionTypes) m_transType->addItem(i.name, int(i.type));
    m_transAudioType = new QLabel("Cross Fade +3 dB"); // Audio kennt nur den Crossfade
    auto* typeBox = new QWidget;
    auto* typeLay = new QHBoxLayout(typeBox);
    typeLay->setContentsMargins(0, 0, 0, 0);
    typeLay->addWidget(m_transType);
    typeLay->addWidget(m_transAudioType);
    s.grid->addWidget(rowLabel("Art"), s.rows, 0);
    s.grid->addWidget(typeBox, s.rows++, 1, 1, 2);

    m_transLen = new ScrubField(0.04, 600, 0.04, 2);
    m_transLen->setSuffix(" s");
    m_transLen->onChange = [this](double sec) {
        const int frames = std::max(1, int(std::lround(sec * m_editor->project()->fps())));
        m_editor->setTransitionLength(m_transKey.leftId, m_transKey.rightId, frames, "transLenInspector");
    };
    s.grid->addWidget(rowLabel("Dauer"), s.rows, 0);
    s.grid->addWidget(m_transLen, s.rows++, 1);

    m_transAlign = combo();
    m_transAlign->addItems({"Mitte auf Schnitt", "Beginn am Schnitt", "Ende am Schnitt"}); // Index = TransitionAlign
    m_transAlign->setToolTip("Lage zum Schnitt (nur bei Überblendung zwischen zwei Clips)");
    s.grid->addWidget(rowLabel("Ausrichtung"), s.rows, 0);
    s.grid->addWidget(m_transAlign, s.rows++, 1, 1, 2);

    auto changed = [this, styleFromUi] {
        m_editor->setTransitionStyle(m_transKey.leftId, m_transKey.rightId, styleFromUi());
    };
    connect(m_transType, &QComboBox::activated, this, changed);
    connect(m_transAlign, &QComboBox::activated, this, changed);
    page->addStretch(1);
}

bool Inspector::selectedTransition(TrackKind* kind, TimelineOps::TransitionSpan* span) const
{
    const TransitionKey key = m_editor->selection()->transition();
    if (key.isNull()) return false;
    TrackRef ref;
    if (!TimelineOps::findClip(m_editor->project()->timeline(), key.leftId ? key.leftId : key.rightId, &ref))
        return false;
    for (const auto& s : m_editor->transitions(ref))
        if (s.leftId == key.leftId && s.rightId == key.rightId) {
            *kind = ref.kind;
            *span = s;
            return true;
        }
    return false;
}

bool Inspector::refreshTransition()
{
    TrackKind kind;
    TimelineOps::TransitionSpan span;
    const bool on = selectedTransition(&kind, &span);
    // Übergang ausgewählt: nur dieser Tab (wie DaVinci), sonst ist er ausgeblendet
    for (int id : {0, 1, kTitlePage}) m_tabs->button(id)->setVisible(!on);
    m_tabs->button(kTransitionPage)->setVisible(on);
    if (!on) return false;
    m_transKey = m_editor->selection()->transition();
    m_content->setVisible(true);
    m_empty->setVisible(false);
    const bool video = kind == TrackKind::Video;
    m_clipName->setText(video ? transitionTypeInfo(span.style.type).name : "Cross Fade +3 dB");
    m_clipName->setToolTip({});
    m_tabs->button(kTransitionPage)->setChecked(true);
    m_pages->setCurrentIndex(kTransitionPage);

    m_transType->setVisible(video);
    m_transAudioType->setVisible(!video);
    m_transType->setCurrentIndex(m_transType->findData(int(span.style.type)));
    m_transAlign->setCurrentIndex(int(span.style.align));
    m_transAlign->setEnabled(span.isDissolve()); // Ein-/Ausblenden liegt immer im Clip
    m_transLen->setValue(double(span.length()) / std::max(1, m_editor->project()->fps()));
    return true;
}

void Inspector::refresh()
{
    if (refreshTransition()) return;
    const Clip* v = primary(TrackKind::Video);
    const Clip* a = primary(TrackKind::Audio);
    const bool any = v || a;
    m_content->setVisible(any);
    m_empty->setVisible(!any);
    if (!any) return;

    const Clip* shown = v ? v : a;
    const int count = m_editor->selection()->ids().size();
    QString name = shown->displayName();
    if (count > (v && a ? 2 : 1)) name += QString("  (+%1)").arg(count - 1);
    m_clipName->setText(name);
    m_clipName->setToolTip(shown->mediaPath);

    // Tabs ohne passenden Clip ausgrauen wie in DaVinci; Titel-Tab nur bei Titelclips
    const Clip* t = primary(TrackKind::Video, true);
    m_tabs->button(0)->setEnabled(v);
    m_tabs->button(1)->setEnabled(a);
    m_tabs->button(kTitlePage)->setEnabled(t);
    m_tabs->button(kTitlePage)->setVisible(t);
    int page = m_pages->currentIndex();
    if (t && shown->id != m_lastShownId) page = kTitlePage; // neu ausgewählter Titel -> Tab "Titel"
    m_lastShownId = shown->id;
    if ((page == 0 && !v) || (page == 1 && !a) || (page == kTitlePage && !t)) page = v ? 0 : 1;
    m_tabs->button(page)->setChecked(true);
    m_pages->setCurrentIndex(page);

    for (Param* p : m_params) {
        const Clip* c = p->title ? t : p->kind == TrackKind::Video ? v : a;
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
