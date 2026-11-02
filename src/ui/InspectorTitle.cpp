// SPDX-License-Identifier: AGPL-3.0-only
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

    auto* fontBox = new QFontComboBox;
    fontBox->setFocusPolicy(Qt::ClickFocus);
    fontBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); // otherwise the list blows the width
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

    buildGeneratorSections(page, ol);
    page->addStretch(1);
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
    addSlider(cnt, "titleCountFrom", T("Von"), -1e9, 1e9, TitleStyle().countFrom, 1, 2,
              [](Clip& c) -> double& { return c.title.countFrom; });
    addSlider(cnt, "titleCountTo", T("Bis"), -1e9, 1e9, TitleStyle().countTo, 1, 2,
              [](Clip& c) -> double& { return c.title.countTo; });
    auto* places = combo();
    for (int i = 0; i <= 3; ++i) places->addItem(QString::number(i), i);
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
            places->setCurrentIndex(places->findData(std::clamp(c->title.decimals, 0, 3)));
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
            if (anim) Keys::setValue(clip, *anim, localFrame(clip), Keys::fromColor(col)); // animated -> keyframe
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
            if (swatch->styleSheet() != css) swatch->setStyleSheet(css); // while playing only on change
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
