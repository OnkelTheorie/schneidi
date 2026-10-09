// Inspector: title page.
#include "ui/Inspector.h"

#include "app/Theme.h"
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
#include "ui/InspectorDetail.h"

using namespace InspectorDetail;

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
        for (AnimParam p : {AnimParam::TitleSize, AnimParam::TitleColor, AnimParam::TitlePosX, AnimParam::TitlePosY})
            Keys::clear(c, p);
    }, {}, true);

    auto* edit = new QPlainTextEdit;
    edit->setFixedHeight(76);
    edit->setPlaceholderText("Text");
    edit->setTabChangesFocus(true);
    // Live: jede Änderung sofort in die Vorschau, Tippen = ein Undo-Schritt
    connect(edit, &QPlainTextEdit::textChanged, this, [this, edit, V] {
        const QString text = edit->toPlainText();
        apply(V, "titleText", T("Titeltext"), [text](Clip& c) { c.title.text = text; }, true);
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
        apply(V, {}, T("Schriftart"), [family](Clip& c) { c.title.font = family; }, true);
    });
    m_refreshers << [fontBox, current] {
        if (const Clip* c = current(); c && fontBox->currentFont().family() != c->title.fontFamily()) {
            const QSignalBlocker b(fontBox);
            fontBox->setCurrentFont(QFont(c->title.fontFamily()));
        }
    };
    txt.grid->addWidget(rowLabel(T("Schriftart")), txt.rows, 0);
    txt.grid->addWidget(fontBox, txt.rows++, 1);

    addSlider(txt, "titleSize", T("Größe"), 1, 400, TitleStyle().size, 0.5, 1,
              [](Clip& c) -> double& { return c.title.size; }, AnimParam::TitleSize);
    addColor(txt, T("Farbe"), T("Textfarbe"), [](Clip& c) -> QColor& { return c.title.color; }, true, {},
             AnimParam::TitleColor);

    // Stil + Ausrichtung in einer Zeile
    auto* bold = toggleButton();
    bold->setText("F");
    bold->setToolTip(T("Fett"));
    QFont bf = bold->font();
    bf.setBold(true);
    bold->setFont(bf);
    auto* italic = toggleButton();
    italic->setText("K");
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

    addXY(txt, "titlePos", "Position", -10000, 10000, 0, 1, 3, [](Clip& c) -> double& { return c.title.posX; },
          [](Clip& c) -> double& { return c.title.posY; }, nullptr, AnimParam::TitlePosX, AnimParam::TitlePosY);

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
    Section ol = addSection(page, V, T("Umrandung"), [](Clip& c) {
        const TitleStyle d;
        c.title.outlineColor = d.outlineColor;
        c.title.outlineWidth = d.outlineWidth;
    }, outlineOn, true);
    addColor(ol, T("Farbe"), T("Umrandungsfarbe"), [](Clip& c) -> QColor& { return c.title.outlineColor; }, true,
             [](Clip& c) { c.title.outlineOn = true; });
    autoOn(addSlider(ol, "titleOutline", T("Breite"), 0, 40, TitleStyle().outlineWidth, 0.1, 1,
                     [](Clip& c) -> double& { return c.title.outlineWidth; }),
           "titleOutline", T("Umrandung"), outlineOn);

    const Flag boxOn = [](Clip& c) -> bool& { return c.title.boxOn; };
    Section box = addSection(page, V, T("Hintergrund"), [](Clip& c) {
        const TitleStyle d;
        c.title.boxColor = d.boxColor;
        c.title.boxPad = d.boxPad;
    }, boxOn, true);
    addColor(box, T("Farbe"), T("Hintergrundfarbe"), [](Clip& c) -> QColor& { return c.title.boxColor; }, true,
             [](Clip& c) { c.title.boxOn = true; });
    autoOn(addSlider(box, "titleBoxPad", T("Abstand"), 0, 200, TitleStyle().boxPad, 0.5, 1,
                     [](Clip& c) -> double& { return c.title.boxPad; }),
           "titleBoxPad", T("Hintergrund"), boxOn);
    page->addStretch(1);
}

void Inspector::addColor(Section& s, const QString& label, const QString& text,
                         const std::function<QColor&(Clip&)>& color, bool alpha, const std::function<void(Clip&)>& also,
                         std::optional<AnimParam> anim)
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
        apply(kind, {}, text, [this, color, col, also, anim](Clip& clip) {
            if (anim) Keys::setValue(clip, *anim, localFrame(clip), Keys::fromColor(col)); // animiert -> Keyframe
            else color(clip) = col;
            if (also) also(clip);
        }, title);
    });
    m_refreshers << [=, this] {
        if (const Clip* c = primary(kind, title)) {
            Clip copy = *c;
            const QColor col = anim ? Keys::toColor(Keys::valueAt(copy, *anim, localFrame(copy))) : color(copy);
            const QString css = QString("QToolButton { background: %1; border: 1px solid %2; border-radius: 2px; }")
                                    .arg(col.name(QColor::HexArgb), Theme::border.name());
            if (swatch->styleSheet() != css) swatch->setStyleSheet(css); // beim Abspielen nur bei Änderung
        }
    };
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->addStretch(1);
    row->addWidget(swatch);
    s.grid->addWidget(rowLabel(label), s.rows, 0);
    s.grid->addLayout(row, s.rows, 1);
    if (anim) s.grid->addWidget(keyButtons(kind, title, {*anim}), s.rows, 3);
    ++s.rows;
}
