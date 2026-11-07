// SPDX-License-Identifier: AGPL-3.0-only
// Inspector: title page.
#include "ui/Inspector.h"

#include "app/Theme.h"
#include "core/BrandKit.h"
#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/Subtitles.h"
#include "core/Timecode.h"
#include "core/TimelineOps.h"
#include "ui/ScrubField.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QDir>
#include <QComboBox>
#include <QFontComboBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QMenu>
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
#include <functional>
#include "ui/InspectorDetail.h"

using namespace InspectorDetail;

namespace {
// Every widget in grid rows [from, to) (also inside nested layouts): rows to show or hide together
QVector<QWidget*> rowWidgets(QGridLayout* grid, int from, int to)
{
    QVector<QWidget*> out;
    std::function<void(QLayoutItem*)> collect = [&](QLayoutItem* item) {
        if (!item) return;
        if (QWidget* w = item->widget()) out << w;
        if (QLayout* l = item->layout())
            for (int i = 0; i < l->count(); ++i) collect(l->itemAt(i));
    };
    for (int i = 0; i < grid->count(); ++i) {
        int row, col, rowSpan, colSpan;
        grid->getItemPosition(i, &row, &col, &rowSpan, &colSpan);
        if (row >= from && row < to) collect(grid->itemAt(i));
    }
    return out;
}
} // namespace

void Inspector::buildTitlePage(QVBoxLayout* page)
{
    const TrackKind V = TrackKind::Video;
    auto current = [this]() -> const Clip* { return primary(TrackKind::Video, true); };

    // ---- Text (like DaVinci "Text": text field, font, size, color, style, alignment, position) ----
    Section txt = addSection(page, V, T("Text"), [](Clip& c) {
        const TitleStyle d;
        TitleStyle& t = c.title; // the text itself stays
        t.font = d.font;
        t.size = d.size;
        t.color = d.color;
        t.bold = d.bold;
        t.italic = d.italic;
        t.align = d.align;
        t.posX = d.posX;
        t.posY = d.posY;
        t.colorRef = t.fontRef = 0; // the defaults are fixed values
        for (AnimParam p : {AnimParam::TitleSize, AnimParam::TitleColor, AnimParam::TitlePosX, AnimParam::TitlePosY})
            Keys::clear(c, p);
    }, {}, true);

    auto* edit = new QPlainTextEdit;
    edit->setFixedHeight(76);
    edit->setPlaceholderText(T("Text"));
    edit->setTabChangesFocus(true);
    // Live: every change immediately into the preview, typing = one undo step
    connect(edit, &QPlainTextEdit::textChanged, this, [this, edit, V] {
        const QString text = edit->toPlainText();
        apply(V, "titleText", T("Titeltext"), [text](Clip& c) { c.title.text = text; }, true);
    });
    m_refreshers << [edit, current] {
        const Clip* c = current();
        if (c) edit->setVisible(c->title.kind == TitleKind::Text); // counter/progress bar: no text of their own
        if (c && edit->toPlainText() != c->title.text) {
            const QSignalBlocker b(edit);
            edit->setPlainText(c->title.text);
        }
    };
    txt.grid->addWidget(edit, txt.rows++, 0, 1, 3);
    const int fontRow = txt.rows;

    auto* fontBox = new QFontComboBox;
    fontBox->setFocusPolicy(Qt::ClickFocus);
    fontBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); // otherwise the list blows the width
    fontBox->setMinimumContentsLength(8);
    fontBox->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    connect(fontBox, &QFontComboBox::currentFontChanged, this, [this, V](const QFont& f) {
        const QString family = f.family();
        apply(V, {}, T("Schriftart"), [family](Clip& c) {
            c.title.font = family;
            c.title.fontRef = 0; // chosen by hand: fixed
        }, true);
    });
    m_refreshers << [fontBox, current] {
        if (const Clip* c = current(); c && fontBox->currentFont().family() != c->title.fontFamily()) {
            const QSignalBlocker b(fontBox);
            fontBox->setCurrentFont(QFont(c->title.fontFamily()));
        }
    };
    auto* fontLine = new QHBoxLayout;
    fontLine->setContentsMargins(0, 0, 0, 0);
    fontLine->setSpacing(4);
    fontLine->addWidget(fontBox, 1);
    fontLine->addWidget(brandButton(true, T("Schriftart"), [](Clip& c) -> int& { return c.title.fontRef; }));
    txt.grid->addWidget(rowLabel(T("Schriftart")), txt.rows, 0);
    txt.grid->addLayout(fontLine, txt.rows++, 1);

    addSlider(txt, "titleSize", T("Größe"), 1, 400, TitleStyle().size, 0.5, 1,
              [](Clip& c) -> double& { return c.title.size; }, AnimParam::TitleSize);
    const int colorRow = txt.rows;
    addColor(txt, T("Farbe"), T("Textfarbe"), [](Clip& c) -> QColor& { return c.title.color; }, true, {},
             AnimParam::TitleColor, [](Clip& c) -> int& { return c.title.colorRef; });
    const int styleRow = txt.rows;

    // Style + alignment in one row
    auto* bold = toggleButton();
    bold->setText(T("F")); // first letter of "bold" in the UI language (B, G, N, Ж)
    bold->setToolTip(T("Fett"));
    QFont bf = bold->font();
    bf.setBold(true);
    bold->setFont(bf);
    auto* italic = toggleButton();
    italic->setText(T("K")); // first letter of "italic" (I, K, К)
    italic->setToolTip(T("Kursiv"));
    QFont itf = italic->font();
    itf.setItalic(true);
    italic->setFont(itf);
    connect(bold, &QToolButton::clicked, this, [this, V](bool on) {
        apply(V, {}, on ? T("Fett an") : T("Fett aus"), [on](Clip& c) { c.title.bold = on; }, true);
    });
    connect(italic, &QToolButton::clicked, this, [this, V](bool on) {
        apply(V, {}, on ? T("Kursiv an") : T("Kursiv aus"), [on](Clip& c) { c.title.italic = on; }, true);
    });
    auto* styleBox = new QHBoxLayout;
    styleBox->setContentsMargins(0, 0, 0, 0);
    styleBox->setSpacing(2);
    styleBox->addWidget(bold);
    styleBox->addWidget(italic);
    styleBox->addSpacing(12);
    auto* alignGroup = new QButtonGroup(this);
    const QString alignTips[] = {T("Linksbündig"), T("Zentriert"), T("Rechtsbündig")};
    for (int i = 0; i < 3; ++i) {
        auto* b = toggleButton();
        b->setIcon(alignIcon(i));
        b->setToolTip(alignTips[i]);
        alignGroup->addButton(b, i);
        styleBox->addWidget(b);
    }
    styleBox->addStretch(1);
    connect(alignGroup, &QButtonGroup::idClicked, this, [this, V](int id) {
        apply(V, {}, T("Ausrichtung"), [id](Clip& c) { c.title.align = id; }, true);
    });
    m_refreshers << [bold, italic, alignGroup, current] {
        if (const Clip* c = current()) {
            bold->setChecked(c->title.bold);
            italic->setChecked(c->title.italic);
            if (auto* b = alignGroup->button(c->title.align)) b->setChecked(true);
        }
    };
    txt.grid->addWidget(rowLabel(T("Stil")), txt.rows, 0);
    txt.grid->addLayout(styleBox, txt.rows++, 1);

    addXY(txt, "titlePos", T("Position"), -10000, 10000, 0, 1, 3, [](Clip& c) -> double& { return c.title.posX; },
          [](Clip& c) -> double& { return c.title.posY; }, nullptr, AnimParam::TitlePosX, AnimParam::TitlePosY);
    // A progress bar has no font, size, style or position (its place is the edge): only its colour stays here
    const QVector<QWidget*> textRows = rowWidgets(txt.grid, fontRow, colorRow) + rowWidgets(txt.grid, styleRow, txt.rows);

    // ---- Outline / background (red dot; changing a value switches it on) ----
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
    Section ol = addSection(page, V, T("Umrandung"), [](Clip& c) {
        const TitleStyle d;
        c.title.outlineColor = d.outlineColor;
        c.title.outlineWidth = d.outlineWidth;
        c.title.outlineRef = 0;
    }, outlineOn, true);
    addColor(ol, T("Farbe"), T("Umrandungsfarbe"), [](Clip& c) -> QColor& { return c.title.outlineColor; }, true,
             [](Clip& c) { c.title.outlineOn = true; }, {}, [](Clip& c) -> int& { return c.title.outlineRef; });
    autoOn(addSlider(ol, "titleOutline", T("Breite"), 0, 40, TitleStyle().outlineWidth, 0.1, 1,
                     [](Clip& c) -> double& { return c.title.outlineWidth; }),
           "titleOutline", T("Umrandung"), outlineOn);

    const Flag boxOn = [](Clip& c) -> bool& { return c.title.boxOn; };
    Section box = addSection(page, V, T("Hintergrund"), [](Clip& c) {
        const TitleStyle d;
        c.title.boxColor = d.boxColor;
        c.title.boxPad = d.boxPad;
        c.title.boxRef = 0;
    }, boxOn, true);
    addColor(box, T("Farbe"), T("Hintergrundfarbe"), [](Clip& c) -> QColor& { return c.title.boxColor; }, true,
             [](Clip& c) { c.title.boxOn = true; }, {}, [](Clip& c) -> int& { return c.title.boxRef; });
    const int padRow = box.rows;
    autoOn(addSlider(box, "titleBoxPad", T("Abstand"), 0, 200, TitleStyle().boxPad, 0.5, 1,
                     [](Clip& c) -> double& { return c.title.boxPad; }),
           "titleBoxPad", T("Hintergrund"), boxOn);
    const QVector<QWidget*> textOnly = textRows + rowWidgets(box.grid, padRow, box.rows);
    m_refreshers << [textOnly, current] {
        if (const Clip* c = current())
            for (QWidget* w : textOnly) w->setVisible(!c->title.isProgress());
    };

    buildLookSections(page);
    buildGeneratorSections(page, ol);
    page->addStretch(1);
}

void Inspector::buildLookSections(QVBoxLayout* page)
{
    const TrackKind V = TrackKind::Video;
    auto current = [this]() -> const Clip* { return primary(TrackKind::Video, true); };
    auto combo = [] {
        auto* c = new QComboBox;
        c->setFocusPolicy(Qt::ClickFocus);
        return c;
    };
    // Changing a value of a group switches it on (like outline and box)
    auto autoOn = [this, V](Param* p, const QString& key, const QString& text, const std::function<void(Clip&)>& on) {
        p->edit->onChange = [this, p, key, text, on, V](double v) {
            if (p->slider) { // X/Y fields have none
                const QSignalBlocker b(p->slider);
                p->slider->setValue(sliderPos(v, p->min, p->max));
            }
            apply(V, key, text, [p, v, on](Clip& c) {
                p->field(c) = v;
                on(c);
            }, true);
        };
    };
    // The gradient's first two stops as fields (3 and 4 stops through the CLI keep their places)
    auto stop = [](Clip& c, int i) -> QColor& {
        QVector<ColorStop>& s = c.title.look.stops;
        if (s.size() < 2) {
            const QVector<ColorStop> d = c.title.look.gradientStops(c.title.color);
            s = {d[0], d[1]};
        }
        return s[i].color;
    };
    QVector<std::pair<Section, std::function<bool(const TitleStyle&)>>> shown;

    // ---- Fill: one colour, gradient or a picture of the media pool ----
    Section fill = addSection(page, V, T("Füllung"), [](Clip& c) {
        const TitleLook d;
        TitleLook& l = c.title.look;
        l.fill = d.fill;
        l.stops = d.stops;
        l.gradAngle = d.gradAngle;
        l.texture = d.texture;
        l.texScale = d.texScale;
        l.texX = d.texX;
        l.texY = d.texY;
    }, {}, true);
    auto* kind = combo();
    for (const auto& f : kTitleFills) kind->addItem(T(f.name), int(f.fill));
    connect(kind, &QComboBox::activated, this, [this, V, kind] {
        const auto f = TitleFill(kind->currentData().toInt());
        apply(V, {}, T("Füllung"), [f](Clip& c) { c.title.look.fill = f; }, true);
    });
    fill.grid->addWidget(rowLabel(T("Art")), fill.rows, 0);
    fill.grid->addWidget(kind, fill.rows++, 1);
    const int gradRow = fill.rows;
    const auto toGradient = [](Clip& c) { c.title.look.fill = TitleFill::Gradient; };
    addColor(fill, T("Farbe oben"), T("Verlaufsfarbe"), [stop](Clip& c) -> QColor& { return stop(c, 0); }, true,
             toGradient);
    addColor(fill, T("Farbe unten"), T("Verlaufsfarbe"), [stop](Clip& c) -> QColor& { return stop(c, 1); }, true,
             toGradient);
    autoOn(addSlider(fill, "titleGradAngle", T("Winkel"), -180, 180, TitleLook().gradAngle, 1, 0,
                     [](Clip& c) -> double& { return c.title.look.gradAngle; }),
           "titleGradAngle", T("Verlaufswinkel"), toGradient);
    const int texRow = fill.rows;
    auto* picture = combo();
    picture->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    picture->setMinimumContentsLength(8);
    picture->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    picture->setToolTip(T("Ein Bild aus dem Media Pool füllt die Buchstaben"));
    connect(picture, &QComboBox::activated, this, [this, V, picture] {
        const QString path = picture->currentData().toString();
        apply(V, {}, T("Textur"), [path](Clip& c) {
            c.title.look.texture = path;
            if (!path.isEmpty()) c.title.look.fill = TitleFill::Texture;
        }, true);
    });
    fill.grid->addWidget(rowLabel(T("Bild")), fill.rows, 0);
    fill.grid->addWidget(picture, fill.rows++, 1);
    const auto toTexture = [](Clip& c) {
        if (!c.title.look.texture.isEmpty()) c.title.look.fill = TitleFill::Texture;
    };
    autoOn(addSlider(fill, "titleTexScale", T("Maßstab"), 0.05, 10, TitleLook().texScale, 0.01, 2,
                     [](Clip& c) -> double& { return c.title.look.texScale; }),
           "titleTexScale", T("Texturmaßstab"), toTexture);
    addXY(fill, "titleTexPos", T("Versatz"), -10000, 10000, 0, 1, 0, [](Clip& c) -> double& { return c.title.look.texX; },
          [](Clip& c) -> double& { return c.title.look.texY; });
    const QVector<QWidget*> gradWidgets = rowWidgets(fill.grid, gradRow, texRow);
    const QVector<QWidget*> texWidgets = rowWidgets(fill.grid, texRow, fill.rows);
    m_refreshers << [this, current, kind, picture, gradWidgets, texWidgets] {
        const Clip* c = current();
        if (!c) return;
        const TitleLook& l = c->title.look;
        const QSignalBlocker b1(kind), b2(picture);
        kind->setCurrentIndex(kind->findData(int(l.fill)));
        for (QWidget* w : gradWidgets) w->setVisible(l.fill == TitleFill::Gradient);
        for (QWidget* w : texWidgets) w->setVisible(l.fill == TitleFill::Texture);
        // Pictures of the media pool (and the title's own, also when it is not in the pool any more)
        picture->clear();
        picture->addItem(T("Kein Bild"), QString());
        for (const MediaInfo& m : m_editor->project()->media())
            if (m.isImage) picture->addItem(m.name.isEmpty() ? QFileInfo(m.path).fileName() : m.name, m.path);
        if (!l.texture.isEmpty() && picture->findData(l.texture) < 0)
            picture->addItem(QFileInfo(l.texture).fileName(), l.texture);
        picture->setCurrentIndex(std::max(0, picture->findData(l.texture)));
    };
    shown.append({fill, [](const TitleStyle& t) { return !t.isProgress(); }});

    // ---- Edge: 2.5D extrusion ----
    const Flag edgeOn = [](Clip& c) -> bool& { return c.title.look.edgeOn; };
    const auto edgeSwitch = [](Clip& c) { c.title.look.edgeOn = true; };
    Section edge = addSection(page, V, T("Kante"), [](Clip& c) {
        const TitleLook d;
        TitleLook& l = c.title.look;
        l.edgeDepth = d.edgeDepth;
        l.edgeAngle = d.edgeAngle;
        l.edgeColor = d.edgeColor;
        l.edgeLight = d.edgeLight;
    }, edgeOn, true);
    edge.header->setToolTip(T("3D-Kante: die Buchstaben bekommen eine Tiefe in eine Richtung"));
    autoOn(addSlider(edge, "titleEdgeDepth", T("Tiefe"), 0, 200, TitleLook().edgeDepth, 0.5, 1,
                     [](Clip& c) -> double& { return c.title.look.edgeDepth; }),
           "titleEdgeDepth", T("Kante"), edgeSwitch);
    autoOn(addSlider(edge, "titleEdgeAngle", T("Winkel"), -180, 180, TitleLook().edgeAngle, 1, 0,
                     [](Clip& c) -> double& { return c.title.look.edgeAngle; }),
           "titleEdgeAngle", T("Kante"), edgeSwitch);
    addColor(edge, T("Farbe"), T("Kantenfarbe"), [](Clip& c) -> QColor& { return c.title.look.edgeColor; }, false,
             edgeSwitch);
    auto* light = new QCheckBox(T("Licht von oben"));
    light->setFocusPolicy(Qt::ClickFocus);
    connect(light, &QCheckBox::clicked, this, [this, V](bool on) {
        apply(V, {}, T("Licht von oben"), [on](Clip& c) { c.title.look.edgeLight = on; }, true);
    });
    edge.grid->addWidget(light, edge.rows++, 1);
    m_refreshers << [current, light] {
        if (const Clip* c = current()) {
            const QSignalBlocker b(light);
            light->setChecked(c->title.look.edgeLight);
        }
    };
    shown.append({edge, [](const TitleStyle& t) { return !t.isProgress(); }});

    // ---- Shadow ----
    const Flag shadowOn = [](Clip& c) -> bool& { return c.title.look.shadowOn; };
    const auto shadowSwitch = [](Clip& c) { c.title.look.shadowOn = true; };
    Section shadow = addSection(page, V, T("Schatten"), [](Clip& c) {
        const TitleLook d;
        TitleLook& l = c.title.look;
        l.shadowX = d.shadowX;
        l.shadowY = d.shadowY;
        l.shadowSoft = d.shadowSoft;
        l.shadowColor = d.shadowColor;
        l.shadowOpacity = d.shadowOpacity;
    }, shadowOn, true);
    const auto [sx, sy] = addXY(shadow, "titleShadow", T("Versatz"), -500, 500, 6, 0.5, 1,
                                [](Clip& c) -> double& { return c.title.look.shadowX; },
                                [](Clip& c) -> double& { return c.title.look.shadowY; });
    autoOn(sx, "titleShadowX", T("Schatten"), shadowSwitch);
    autoOn(sy, "titleShadowY", T("Schatten"), shadowSwitch);
    autoOn(addSlider(shadow, "titleShadowSoft", T("Weichheit"), 0, 100, TitleLook().shadowSoft, 0.5, 1,
                     [](Clip& c) -> double& { return c.title.look.shadowSoft; }),
           "titleShadowSoft", T("Schatten"), shadowSwitch);
    autoOn(addSlider(shadow, "titleShadowOpacity", T("Deckkraft"), 0, 1, TitleLook().shadowOpacity, 0.01, 2,
                     [](Clip& c) -> double& { return c.title.look.shadowOpacity; }),
           "titleShadowOpacity", T("Schatten"), shadowSwitch);
    addColor(shadow, T("Farbe"), T("Schattenfarbe"), [](Clip& c) -> QColor& { return c.title.look.shadowColor; },
             false, shadowSwitch);
    shown.append({shadow, [](const TitleStyle& t) { return !t.isProgress(); }});

    // ---- Glow ----
    const Flag glowOn = [](Clip& c) -> bool& { return c.title.look.glowOn; };
    const auto glowSwitch = [](Clip& c) { c.title.look.glowOn = true; };
    Section glow = addSection(page, V, T("Glühen"), [](Clip& c) {
        const TitleLook d;
        TitleLook& l = c.title.look;
        l.glowRadius = d.glowRadius;
        l.glowColor = d.glowColor;
        l.glowStrength = d.glowStrength;
    }, glowOn, true);
    autoOn(addSlider(glow, "titleGlowRadius", T("Radius"), 0, 200, TitleLook().glowRadius, 0.5, 1,
                     [](Clip& c) -> double& { return c.title.look.glowRadius; }),
           "titleGlowRadius", T("Glühen"), glowSwitch);
    autoOn(addSlider(glow, "titleGlowStrength", T("Stärke"), 0, 1, TitleLook().glowStrength, 0.01, 2,
                     [](Clip& c) -> double& { return c.title.look.glowStrength; }),
           "titleGlowStrength", T("Glühen"), glowSwitch);
    addColor(glow, T("Farbe"), T("Glühfarbe"), [](Clip& c) -> QColor& { return c.title.look.glowColor; }, false,
             glowSwitch);
    shown.append({glow, [](const TitleStyle& t) { return !t.isProgress(); }});

    // A progress bar has none of these
    m_refreshers << [shown, current] {
        if (const Clip* c = current())
            for (const auto& [s, when] : shown) {
                const bool on = when(c->title);
                s.header->setVisible(on);
                if (!on) s.body->setVisible(false);
                else if (s.toggle) s.body->setVisible(s.toggle->isChecked());
            }
    };
}

// Counter and progress bar (TitleKind): their own sections, shown only for that kind; the progress bar has no
// outline (its section hides)
void Inspector::buildGeneratorSections(QVBoxLayout* page, const Section& outline)
{
    const TrackKind V = TrackKind::Video;
    auto current = [this]() -> const Clip* { return primary(TrackKind::Video, true); };
    auto combo = [] {
        auto* c = new QComboBox;
        c->setFocusPolicy(Qt::ClickFocus);
        return c;
    };
    auto lineEdit = [this, V, current](Section& s, const QString& label, const QString& undo,
                                       const std::function<QString&(Clip&)>& field) {
        auto* e = new QLineEdit;
        connect(e, &QLineEdit::textEdited, this, [this, V, undo, field](const QString& text) {
            apply(V, "title" + undo, undo, [field, text](Clip& c) { field(c) = text; }, true);
        });
        m_refreshers << [e, current, field] {
            if (const Clip* c = current()) {
                Clip copy = *c;
                if (e->text() != field(copy)) {
                    const QSignalBlocker b(e);
                    e->setText(field(copy));
                }
            }
        };
        s.grid->addWidget(rowLabel(label), s.rows, 0);
        s.grid->addWidget(e, s.rows++, 1);
    };
    auto showFor = [this, current](const Section& s, std::function<bool(const TitleStyle&)> when) {
        m_refreshers << [s, current, when] {
            if (const Clip* c = current()) {
                const bool on = when(c->title);
                s.header->setVisible(on);
                if (!on) s.body->setVisible(false);
                else if (s.toggle) s.body->setVisible(s.toggle->isChecked());
            }
        };
    };

    // ---- Counter ----
    Section cnt = addSection(page, V, T("Zähler"), [](Clip& c) {
        const TitleStyle d;
        c.title.countFrom = d.countFrom;
        c.title.countTo = d.countTo;
        c.title.decimals = d.decimals;
        c.title.grouping = d.grouping;
        c.title.countEase = d.countEase;
    }, {}, true);
    // Number fields only (drag or type): a slider over +-1e9 moved the count by millions per step, also when the
    // mouse wheel scrolled the Inspector across it
    for (Param* p : {addSlider(cnt, "titleCountFrom", T("Von"), -1e9, 1e9, TitleStyle().countFrom, 1, 2,
                               [](Clip& c) -> double& { return c.title.countFrom; }),
                     addSlider(cnt, "titleCountTo", T("Bis"), -1e9, 1e9, TitleStyle().countTo, 1, 2,
                               [](Clip& c) -> double& { return c.title.countTo; })}) {
        QSizePolicy keep = p->slider->sizePolicy(); // the field stays where the other rows have theirs
        keep.setRetainSizeWhenHidden(true);
        p->slider->setSizePolicy(keep);
        p->slider->hide();
    }
    auto* places = combo();
    for (int i = 0; i <= 6; ++i) places->addItem(QString::number(i), i); // like the file and the CLI (0..6)
    connect(places, &QComboBox::activated, this, [this, V, places] {
        const int n = places->currentData().toInt();
        apply(V, {}, T("Nachkommastellen"), [n](Clip& c) { c.title.decimals = n; }, true);
    });
    cnt.grid->addWidget(rowLabel(T("Nachkommastellen")), cnt.rows, 0);
    cnt.grid->addWidget(places, cnt.rows++, 1);
    lineEdit(cnt, T("Davor"), T("Text vor der Zahl"), [](Clip& c) -> QString& { return c.title.prefix; });
    lineEdit(cnt, T("Danach"), T("Text nach der Zahl"), [](Clip& c) -> QString& { return c.title.suffix; });
    auto* ease = combo();
    for (const auto& e : kMotionEases) ease->addItem(T(e.name), int(e.ease));
    connect(ease, &QComboBox::activated, this, [this, V, ease] {
        const auto e = MotionEase(ease->currentData().toInt());
        apply(V, {}, T("Tempo"), [e](Clip& c) { c.title.countEase = e; }, true);
    });
    cnt.grid->addWidget(rowLabel(T("Tempo")), cnt.rows, 0);
    cnt.grid->addWidget(ease, cnt.rows++, 1);
    auto* grouping = new QCheckBox(T("Tausendertrennzeichen"));
    grouping->setFocusPolicy(Qt::ClickFocus);
    connect(grouping, &QCheckBox::clicked, this, [this, V](bool on) {
        apply(V, {}, T("Tausendertrennzeichen"), [on](Clip& c) { c.title.grouping = on; }, true);
    });
    cnt.grid->addWidget(grouping, cnt.rows++, 1);
    // Separators: stored with the counter (a project renders the same in every UI language)
    auto* numbers = combo();
    for (const auto& n : kNumberStyles)
        numbers->addItem(QStringLiteral("1") + QChar(n.group == u'\u00a0' ? u' ' : n.group) + "234" + QChar(n.point) + "5",
                         int(n.style));
    connect(numbers, &QComboBox::activated, this, [this, V, numbers] {
        const auto n = NumberStyle(numbers->currentData().toInt());
        apply(V, {}, T("Trennzeichen"), [n](Clip& c) { c.title.numbers = n; }, true);
    });
    cnt.grid->addWidget(rowLabel(T("Trennzeichen")), cnt.rows, 0);
    cnt.grid->addWidget(numbers, cnt.rows++, 1);
    m_refreshers << [current, places, ease, grouping, numbers] {
        if (const Clip* c = current()) {
            const QSignalBlocker b1(places), b2(ease), b3(grouping), b4(numbers);
            places->setCurrentIndex(places->findData(std::clamp(c->title.decimals, 0, 6)));
            ease->setCurrentIndex(ease->findData(int(c->title.countEase)));
            grouping->setChecked(c->title.grouping);
            numbers->setCurrentIndex(numbers->findData(int(c->title.numbers)));
        }
    };
    showFor(cnt, [](const TitleStyle& t) { return t.isCounter(); });

    // ---- Progress bar ----
    Section bar = addSection(page, V, T("Fortschrittsbalken"), [](Clip& c) {
        const TitleStyle d;
        c.title.span = d.span;
        c.title.barTop = d.barTop;
        c.title.barMargin = d.barMargin;
        c.title.barRound = d.barRound;
    }, {}, true);
    auto* span = combo();
    span->addItem(T("Ganze Timeline"), int(ProgressSpan::Timeline));
    span->addItem(T("Länge des Clips"), int(ProgressSpan::Clip));
    span->setToolTip(T("Ganze Timeline: füllt sich vom Anfang bis zum Ende der Timeline, egal wo der Clip liegt"));
    connect(span, &QComboBox::activated, this, [this, V, span] {
        const auto v = ProgressSpan(span->currentData().toInt());
        apply(V, {}, T("Fortschritt über"), [v](Clip& c) { c.title.span = v; }, true);
    });
    bar.grid->addWidget(rowLabel(T("Fortschritt über")), bar.rows, 0);
    bar.grid->addWidget(span, bar.rows++, 1);
    auto* edge = combo();
    edge->addItem(T("Oben"), true);
    edge->addItem(T("Unten"), false);
    edge->setToolTip(T("Unten verdecken TikTok, Reels und Shorts den Balken mit ihrer eigenen Leiste"));
    connect(edge, &QComboBox::activated, this, [this, V, edge] {
        const bool top = edge->currentData().toBool();
        apply(V, {}, T("Bildrand"), [top](Clip& c) { c.title.barTop = top; }, true);
    });
    bar.grid->addWidget(rowLabel(T("Bildrand")), bar.rows, 0);
    bar.grid->addWidget(edge, bar.rows++, 1);
    addSlider(bar, "titleBarThickness", T("Dicke"), 1, 200, TitleStyle().barThickness, 0.5, 0,
              [](Clip& c) -> double& { return c.title.barThickness; });
    addSlider(bar, "titleBarMargin", T("Abstand zum Rand"), 0, 1000, TitleStyle().barMargin, 0.5, 0,
              [](Clip& c) -> double& { return c.title.barMargin; });
    auto* round = new QCheckBox(T("Runde Enden"));
    round->setFocusPolicy(Qt::ClickFocus);
    connect(round, &QCheckBox::clicked, this, [this, V](bool on) {
        apply(V, {}, T("Runde Enden"), [on](Clip& c) { c.title.barRound = on; }, true);
    });
    bar.grid->addWidget(round, bar.rows++, 1);
    m_refreshers << [current, span, edge, round] {
        if (const Clip* c = current()) {
            const QSignalBlocker b1(span), b2(edge), b3(round);
            span->setCurrentIndex(span->findData(int(c->title.span)));
            edge->setCurrentIndex(c->title.barTop ? 0 : 1);
            round->setChecked(c->title.barRound);
        }
    };
    showFor(bar, [](const TitleStyle& t) { return t.isProgress(); });
    showFor(outline, [](const TitleStyle& t) { return !t.isProgress(); });
}

void Inspector::addColor(Section& s, const QString& label, const QString& text,
                         const std::function<QColor&(Clip&)>& color, bool alpha, const std::function<void(Clip&)>& also,
                         std::optional<AnimParam> anim, const std::function<int&(Clip&)>& ref)
{
    auto* swatch = new QToolButton;
    swatch->setFixedSize(46, 18);
    swatch->setFocusPolicy(Qt::NoFocus);
    swatch->setToolTip(T("%1 wählen").arg(text));
    const TrackKind kind = s.kind;
    const bool title = s.title;
    connect(swatch, &QToolButton::clicked, this, [=, this] {
        const Clip* c = primary(kind, title);
        if (!c) return;
        Clip copy = *c;
        const QColor cur = anim ? Keys::toColor(Keys::valueAt(copy, *anim, localFrame(copy))) : color(copy);
        const QColor col = QColorDialog::getColor(cur, this, T("%1 wählen").arg(text),
                                                  alpha ? QColorDialog::ShowAlphaChannel : QColorDialog::ColorDialogOptions());
        if (!col.isValid()) return;
        apply(kind, {}, text, [this, color, col, also, anim, ref](Clip& clip) {
            if (anim) Keys::setValue(clip, *anim, localFrame(clip), Keys::fromColor(col)); // animated -> keyframe
            else color(clip) = col;
            if (ref) ref(clip) = 0; // chosen by hand: fixed
            if (also) also(clip);
        }, title);
    });
    m_refreshers << [=, this] {
        if (const Clip* c = primary(kind, title)) {
            Clip copy = *c;
            const QColor col = anim ? Keys::toColor(Keys::valueAt(copy, *anim, localFrame(copy))) : color(copy);
            const QString css = QString("QToolButton { background: %1; border: 1px solid %2; border-radius: 2px; }")
                                    .arg(col.name(QColor::HexArgb), Theme::border.name());
            if (swatch->styleSheet() != css) swatch->setStyleSheet(css); // while playing only on change
        }
    };
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->addStretch(1);
    row->addWidget(swatch);
    if (ref) row->addWidget(brandButton(false, text, ref, also));
    s.grid->addWidget(rowLabel(label), s.rows, 0);
    s.grid->addLayout(row, s.rows, 1);
    if (anim) s.grid->addWidget(keyButtons(kind, title, {*anim}), s.rows, 3);
    ++s.rows;
}

QToolButton* Inspector::brandButton(bool font, const QString& text, const std::function<int&(Clip&)>& ref,
                                    const std::function<void(Clip&)>& also)
{
    const TrackKind V = TrackKind::Video;
    auto* button = new QToolButton;
    button->setFocusPolicy(Qt::NoFocus);
    button->setPopupMode(QToolButton::InstantPopup);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setFixedHeight(18);
    button->setMinimumWidth(30);
    auto* menu = new QMenu(button);
    button->setMenu(menu);
    connect(menu, &QMenu::aboutToShow, this, [=, this] {
        menu->clear();
        const Clip* c = primary(V, true);
        if (!c) return;
        Clip copy = *c;
        const int cur = ref(copy);
        const BrandKit kit = m_editor->project()->effectiveBrand();
        const int count = font ? BrandKit::kMaxFonts : BrandKit::kMaxColors;
        for (int n = 0; n <= count; ++n) {
            QString name = n == 0 ? T("Fest") : font ? T("Brand-Schrift %1").arg(n) : T("Brand-Farbe %1").arg(n);
            QIcon icon;
            if (n > 0 && !font) {
                if (const auto col = kit.color(n)) {
                    QPixmap px(14, 14);
                    px.fill(*col);
                    icon = QIcon(px);
                }
            }
            if (n > 0 && font && kit.font(n)) name += QString(" (%1)").arg(*kit.font(n));
            if (n > 0 && !(font ? kit.font(n).has_value() : kit.color(n).has_value())) name += " " + T("(nicht gesetzt)");
            QAction* a = menu->addAction(icon, name);
            a->setCheckable(true);
            a->setChecked(n == cur);
            connect(a, &QAction::triggered, this, [=, this] {
                apply(V, {}, text, [=](Clip& clip) {
                    ref(clip) = n;
                    // The kit's value now (missing: the value stays); a brand text colour ends its animation
                    Brand::resolve(clip, kit);
                    if (also && n > 0) also(clip);
                }, true);
            });
            if (n == 0) menu->addSeparator();
        }
    });
    m_refreshers << [=, this] {
        if (const Clip* c = primary(V, true)) {
            Clip copy = *c;
            const int n = ref(copy);
            const QString label = n ? QString("B%1").arg(n) : QStringLiteral("B");
            if (button->text() != label) button->setText(label);
            const QString tip = n ? (font ? T("Brand-Schrift %1").arg(n) : T("Brand-Farbe %1").arg(n))
                                  : T("Fester Wert (nicht aus dem Brand-Kit)");
            if (button->toolTip() != tip) button->setToolTip(tip);
            const QString css = n ? QString("QToolButton { color: %1; font-weight: bold; }").arg(Theme::primary.name())
                                  : QString("QToolButton { color: %1; }").arg(Theme::textDim.name());
            if (button->styleSheet() != css) button->setStyleSheet(css);
        }
    };
    return button;
}
