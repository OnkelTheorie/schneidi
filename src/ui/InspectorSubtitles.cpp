// SPDX-License-Identifier: AGPL-3.0-only
// Inspector: subtitle page.
#include "ui/Inspector.h"
#include "ui/LazyFontBox.h"

#include "app/Theme.h"
#include "core/Captions.h"
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
#include "ui/ElidedLabel.h"

using namespace InspectorDetail;

// ---------- Subtitles ----------

const SubtitleCue* Inspector::selectedCue(int* track) const
{
    const Selection* sel = m_editor->selection();
    if (sel->isEmpty() || !m_editor->clipIdsOf(sel->ids()).isEmpty()) return nullptr; // clips take precedence
    const Timeline& tl = m_editor->project()->timeline();
    for (int t = 0; t < tl.subtitles.size(); ++t)
        for (const SubtitleCue& c : tl.subtitles[t].cues)
            if (sel->contains(c.id)) {
                if (track) *track = t;
                return &c;
            }
    return nullptr;
}

void Inspector::changeSubtitleStyle(const QString& text, const std::function<void(TitleStyle&)>& fn, const QString& mergeKey)
{
    int track = -1;
    if (selectedCue(&track)) m_editor->setSubtitleStyle(track, text, fn, mergeKey);
}

void Inspector::editSubtitle(int cueId)
{
    if (!m_editor->isSubtitle(cueId)) return;
    if (!m_editor->selection()->contains(cueId) || m_editor->selection()->ids().size() != 1) m_editor->selection()->set({cueId});
    refresh();
    if (m_subText) {
        m_subText->setFocus();
        m_subText->selectAll();
    }
}

void Inspector::buildSubtitlePage(QVBoxLayout* page)
{
    const TrackKind V = TrackKind::Video;
    auto cueNow = [this]() { return selectedCue(); };

    // ---- Subtitle: text + times (type in timecode like DaVinci) ----
    Section cue = addSection(page, V, T("Untertitel"), {});
    m_subText = new QPlainTextEdit;
    m_subText->setFixedHeight(76);
    m_subText->setPlaceholderText(T("Untertiteltext"));
    m_subText->setTabChangesFocus(true);
    connect(m_subText, &QPlainTextEdit::textChanged, this, [this] {
        if (const SubtitleCue* c = selectedCue())
            m_editor->setSubtitleText(c->id, m_subText->toPlainText(), QString("subText:%1").arg(c->id));
    });
    m_subRefreshers << [this, cueNow] {
        const SubtitleCue* c = cueNow();
        if (c && m_subText->toPlainText() != c->text) {
            const QSignalBlocker b(m_subText);
            m_subText->setPlainText(c->text);
        }
    };
    cue.grid->addWidget(m_subText, cue.rows++, 0, 1, 3);

    // Start / End / Duration: Enter commits, invalid input shows the old value again
    enum { Start, End, Duration };
    const QString labels[] = {T("Start"), T("Ende"), T("Dauer")};
    for (int which : {Start, End, Duration}) {
        auto* edit = new QLineEdit;
        edit->setFocusPolicy(Qt::ClickFocus);
        edit->setAlignment(Qt::AlignRight);
        edit->setMaximumWidth(110);
        connect(edit, &QLineEdit::editingFinished, this, [this, edit, which] {
            const SubtitleCue* c = selectedCue();
            if (!c) return;
            const int f = Timecode::parse(edit->text(), m_editor->project()->fps());
            if (f >= 0) {
                int start = c->start, end = c->end;
                if (which == Start) { // Start moves, duration stays (like DaVinci)
                    end += f - start;
                    start = f;
                } else if (which == End) {
                    end = f;
                } else {
                    end = start + std::max(1, f);
                }
                m_editor->setSubtitleTiming(c->id, start, end);
            }
            refresh(); // invalid/limited -> show the actual value
        });
        m_subRefreshers << [this, edit, which, cueNow] {
            const SubtitleCue* c = cueNow();
            if (!c || edit->hasFocus()) return;
            const int v = which == Start ? c->start : which == End ? c->end : c->length();
            edit->setText(Timecode::format(v, m_editor->project()->fps()));
        };
        cue.grid->addWidget(rowLabel(labels[which]), cue.rows, 0);
        cue.grid->addWidget(edit, cue.rows++, 1, Qt::AlignLeft);
    }
    auto* add = new QPushButton(T("Neuer Untertitel am Abspielkopf"));
    add->setFocusPolicy(Qt::NoFocus);
    connect(add, &QPushButton::clicked, this, [this] {
        int track = -1;
        if (!selectedCue(&track)) return;
        if (const int id = m_editor->addSubtitle(m_playhead, track, QString())) editSubtitle(id);
    });
    cue.grid->addWidget(add, cue.rows++, 0, 1, 4, Qt::AlignLeft); // the whole row: long in some languages

    // ---- Word captions (shorts style): how the track shows its words ----
    Section wc = addSection(page, V, T("Wort-Untertitel"), {});
    auto captionsNow = [this]() -> std::optional<CaptionStyle> {
        int track = -1;
        if (!selectedCue(&track)) return std::nullopt;
        return m_editor->project()->timeline().subtitles[track].captions;
    };
    auto changeCaptions = [this](const std::function<void(CaptionStyle&)>& fn, const QString& mergeKey = {}) {
        int track = -1;
        if (!selectedCue(&track)) return;
        CaptionStyle c = m_editor->project()->timeline().subtitles[track].captions;
        fn(c);
        m_editor->setSubtitleCaptions(track, c, mergeKey);
    };
    auto* mode = new QComboBox;
    mode->setFocusPolicy(Qt::ClickFocus);
    for (const CaptionModeInfo& m : kCaptionModes) mode->addItem(T(m.name), int(m.mode));
    mode->setToolTip(T("Karaoke: der gesprochene Satz bleibt stehen, das gerade gesprochene Wort leuchtet auf. Wort "
                       "für Wort: immer nur das gesprochene Wort, es springt kurz auf. Die Wortzeiten kommen aus dem "
                       "Transkript."));
    connect(mode, qOverload<int>(&QComboBox::activated), this, [mode, changeCaptions](int i) {
        const auto m = CaptionMode(mode->itemData(i).toInt());
        changeCaptions([m](CaptionStyle& c) { c.mode = m; });
    });
    m_subRefreshers << [mode, captionsNow] {
        if (auto c = captionsNow()) {
            const QSignalBlocker b(mode);
            mode->setCurrentIndex(std::max(0, mode->findData(int(c->mode))));
        }
    };
    wc.grid->addWidget(rowLabel(T("Anzeige")), wc.rows, 0);
    wc.grid->addWidget(mode, wc.rows++, 1);

    auto* highlight = new QToolButton;
    highlight->setFixedSize(46, 18);
    highlight->setFocusPolicy(Qt::NoFocus);
    highlight->setToolTip(T("%1 wählen").arg(T("Hervorhebung")));
    connect(highlight, &QToolButton::clicked, this, [this, captionsNow, changeCaptions] {
        auto c = captionsNow();
        if (!c) return;
        const QColor col = QColorDialog::getColor(c->highlight, this, T("%1 wählen").arg(T("Hervorhebung")));
        if (col.isValid()) changeCaptions([col](CaptionStyle& x) { x.highlight = col; });
    });
    m_subRefreshers << [highlight, captionsNow] {
        if (auto c = captionsNow())
            highlight->setStyleSheet(QString("QToolButton { background: %1; border: 1px solid %2; border-radius: 2px; }")
                                         .arg(c->highlight.name(QColor::HexRgb), Theme::border.name()));
    };
    wc.grid->addWidget(rowLabel(T("Hervorhebung")), wc.rows, 0);
    wc.grid->addWidget(highlight, wc.rows++, 1, Qt::AlignLeft);

    // Number fields (dragging = one undo step)
    auto number = [&](const QString& key, const QString& label, double min, double max, double step, int decimals,
                      const std::function<double(const CaptionStyle&)>& get,
                      const std::function<void(CaptionStyle&, double)>& set) {
        auto* edit = new ScrubField(min, max, step, decimals);
        edit->onChange = [changeCaptions, set, key](double v) {
            changeCaptions([&](CaptionStyle& c) { set(c, v); }, key);
        };
        edit->onFinish = [this] { m_editor->project()->closeMerge(); };
        m_subRefreshers << [edit, get, captionsNow] {
            if (auto c = captionsNow()) edit->setValue(get(*c));
        };
        wc.grid->addWidget(rowLabel(label), wc.rows, 0);
        wc.grid->addWidget(edit, wc.rows++, 1, Qt::AlignLeft);
    };
    number("capScale", T("Größe des Worts"), 0.5, 3, 0.05, 2, [](const CaptionStyle& c) { return c.highlightScale; },
           [](CaptionStyle& c, double v) { c.highlightScale = v; });
    number("capWords", T("Wörter pro Zeile"), 0, 20, 1, 0, [](const CaptionStyle& c) { return double(c.maxWordsPerLine); },
           [](CaptionStyle& c, double v) { c.maxWordsPerLine = int(std::lround(v)); });
    number("capPop", T("Aufspringen (ms)"), 0, 1000, 10, 0, [](const CaptionStyle& c) { return double(c.popMs); },
           [](CaptionStyle& c, double v) { c.popMs = int(std::lround(v)); });

    auto* upper = new QCheckBox(T("Großbuchstaben"));
    auto* shadow = new QCheckBox(T("Schatten"));
    for (QCheckBox* box : {upper, shadow}) box->setFocusPolicy(Qt::NoFocus);
    connect(upper, &QCheckBox::clicked, this, [changeCaptions](bool on) { changeCaptions([on](CaptionStyle& c) { c.uppercase = on; }); });
    connect(shadow, &QCheckBox::clicked, this, [changeCaptions](bool on) { changeCaptions([on](CaptionStyle& c) { c.shadow = on; }); });
    m_subRefreshers << [upper, shadow, captionsNow] {
        if (auto c = captionsNow()) {
            upper->setChecked(c->uppercase);
            shadow->setChecked(c->shadow);
        }
    };
    // One below the other: side by side they set the panel's minimum width (long in some languages)
    wc.grid->addWidget(upper, wc.rows++, 1);
    wc.grid->addWidget(shadow, wc.rows++, 1);

    // ---- Track style (applies to all subtitles of the track) ----
    Section st = addSection(page, V, T("Spurstil"), {});
    st.headerLayout->addWidget(resetButton([this] {
        // a word caption track (karaoke/word) goes back to the caption style it was made with, not to plain lines
        int track = -1;
        if (!selectedCue(&track)) return;
        const QSize format = m_editor->project()->timelineFormat().size();
        const CaptionMode mode = m_editor->project()->timeline().subtitles[track].captions.mode;
        const TitleStyle def = mode == CaptionMode::Lines ? Subtitles::defaultStyle(format)
                                                           : Captions::defaultStyle(format, mode);
        changeSubtitleStyle(T("Spurstil zurücksetzen"), [def](TitleStyle& t) { t = def; });
    }));
    auto current = [this]() -> std::optional<TitleStyle> {
        int track = -1;
        if (!selectedCue(&track)) return std::nullopt;
        return m_editor->project()->timeline().subtitles[track].style;
    };

    auto* fontBox = new LazyFontBox; // the real QFontComboBox on the first showing (start time)
    connect(fontBox, &LazyFontBox::currentFontChanged, this, [this](const QFont& f) {
        const QString family = f.family();
        changeSubtitleStyle(T("Schriftart"), [family](TitleStyle& t) { t.font = family; });
    });
    m_subRefreshers << [fontBox, current] {
        if (const auto t = current(); t && fontBox->currentFont().family() != t->fontFamily()) {
            const QSignalBlocker b(fontBox);
            fontBox->setCurrentFont(QFont(t->fontFamily()));
        }
    };
    st.grid->addWidget(rowLabel(T("Schriftart")), st.rows, 0);
    st.grid->addWidget(fontBox, st.rows++, 1);

    // Slider + number field (dragging = one undo step)
    auto slider = [&](Section& s, const QString& key, const QString& label, double min, double max, double step,
                      int decimals, const std::function<double&(TitleStyle&)>& field,
                      const std::function<void(TitleStyle&)>& also = {}) {
        auto* edit = new ScrubField(min, max, step, decimals);
        auto* sl = new QSlider(Qt::Horizontal);
        sl->setRange(0, kSliderSteps);
        sl->setFocusPolicy(Qt::NoFocus);
        auto set = [this, key, label, field, also](double v) {
            changeSubtitleStyle(label, [=](TitleStyle& t) {
                field(t) = v;
                if (also) also(t);
            }, key);
        };
        edit->onChange = [sl, set, min, max](double v) {
            const QSignalBlocker b(sl);
            sl->setValue(sliderPos(v, min, max));
            set(v);
        };
        edit->onFinish = [this] { m_editor->project()->closeMerge(); };
        connect(sl, &QSlider::valueChanged, this, [edit, set, min, max](int v) {
            const double val = min + (max - min) * v / kSliderSteps;
            edit->setValue(val);
            set(val);
        });
        connect(sl, &QSlider::sliderReleased, this, [this] { m_editor->project()->closeMerge(); });
        m_subRefreshers << [edit, sl, field, current, min, max] {
            if (auto t = current()) {
                const double v = field(*t);
                edit->setValue(v);
                const QSignalBlocker b(sl);
                sl->setValue(sliderPos(v, min, max));
            }
        };
        auto* box = new QHBoxLayout;
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(8);
        box->addWidget(sl, 1);
        box->addWidget(edit);
        s.grid->addWidget(rowLabel(label), s.rows, 0);
        s.grid->addLayout(box, s.rows++, 1);
    };
    // Color field (QColorDialog with opacity)
    auto color = [&](Section& s, const QString& label, const QString& text, const std::function<QColor&(TitleStyle&)>& field,
                     const std::function<void(TitleStyle&)>& also = {}) {
        auto* swatch = new QToolButton;
        swatch->setFixedSize(46, 18);
        swatch->setFocusPolicy(Qt::NoFocus);
        swatch->setToolTip(T("%1 wählen").arg(text));
        connect(swatch, &QToolButton::clicked, this, [this, text, field, also, current] {
            auto t = current();
            if (!t) return;
            const QColor col = QColorDialog::getColor(field(*t), this, T("%1 wählen").arg(text), QColorDialog::ShowAlphaChannel);
            if (col.isValid())
                changeSubtitleStyle(text, [&](TitleStyle& st) {
                    field(st) = col;
                    if (also) also(st);
                });
        });
        m_subRefreshers << [swatch, field, current] {
            if (auto t = current())
                swatch->setStyleSheet(QString("QToolButton { background: %1; border: 1px solid %2; border-radius: 2px; }")
                                          .arg(field(*t).name(QColor::HexRgb), Theme::border.name()));
        };
        s.grid->addWidget(rowLabel(label), s.rows, 0);
        s.grid->addWidget(swatch, s.rows++, 1, Qt::AlignLeft);
    };

    slider(st, "subSize", T("Größe"), 4, 300, 0.5, 1, [](TitleStyle& t) -> double& { return t.size; });
    color(st, T("Farbe"), T("Textfarbe"), [](TitleStyle& t) -> QColor& { return t.color; });

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
    connect(bold, &QToolButton::clicked, this, [this](bool on) {
        changeSubtitleStyle(on ? T("Fett an") : T("Fett aus"), [on](TitleStyle& t) { t.bold = on; });
    });
    connect(italic, &QToolButton::clicked, this, [this](bool on) {
        changeSubtitleStyle(on ? T("Kursiv an") : T("Kursiv aus"), [on](TitleStyle& t) { t.italic = on; });
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
    connect(alignGroup, &QButtonGroup::idClicked, this, [this](int id) {
        changeSubtitleStyle(T("Ausrichtung"), [id](TitleStyle& t) { t.align = id; });
    });
    m_subRefreshers << [bold, italic, alignGroup, current] {
        if (auto t = current()) {
            bold->setChecked(t->bold);
            italic->setChecked(t->italic);
            if (auto* b = alignGroup->button(t->align)) b->setChecked(true);
        }
    };
    st.grid->addWidget(rowLabel(T("Stil")), st.rows, 0);
    st.grid->addLayout(styleBox, st.rows++, 1);
    slider(st, "subPosX", T("Position X"), -2000, 2000, 1, 0, [](TitleStyle& t) -> double& { return t.posX; });
    slider(st, "subPosY", T("Abstand unten"), 0, 2000, 1, 0, [](TitleStyle& t) -> double& { return t.posY; });

    // Outline / background: dot switches, changing a value switches on
    auto dotSection = [&](const QString& title, const std::function<bool&(TitleStyle&)>& on) {
        Section s = addSection(page, V, title, {});
        QToolButton* dot = makeDot();
        s.headerLayout->insertWidget(0, dot);
        connect(dot, &QToolButton::clicked, this, [this, title, on](bool enabled) {
            changeSubtitleStyle((enabled ? T("%1 an") : T("%1 aus")).arg(title), [&](TitleStyle& t) { on(t) = enabled; });
        });
        m_subRefreshers << [dot, on, current] {
            if (auto t = current()) {
                const QSignalBlocker b(dot);
                dot->setChecked(on(*t));
            }
        };
        return s;
    };
    const auto outlineOn = [](TitleStyle& t) -> bool& { return t.outlineOn; };
    Section ol = dotSection(T("Umrandung"), outlineOn);
    color(ol, T("Farbe"), T("Umrandungsfarbe"), [](TitleStyle& t) -> QColor& { return t.outlineColor; },
          [](TitleStyle& t) { t.outlineOn = true; });
    slider(ol, "subOutline", T("Breite"), 0, 20, 0.1, 1, [](TitleStyle& t) -> double& { return t.outlineWidth; },
           [](TitleStyle& t) { t.outlineOn = true; });
    const auto boxOn = [](TitleStyle& t) -> bool& { return t.boxOn; };
    Section box = dotSection(T("Hintergrund"), boxOn);
    color(box, T("Farbe"), T("Hintergrundfarbe"), [](TitleStyle& t) -> QColor& { return t.boxColor; },
          [](TitleStyle& t) { t.boxOn = true; });
    slider(box, "subBoxPad", T("Abstand"), 0, 100, 0.5, 1, [](TitleStyle& t) -> double& { return t.boxPad; },
           [](TitleStyle& t) { t.boxOn = true; });
    page->addStretch(1);
}

bool Inspector::refreshSubtitle()
{
    int track = -1;
    const SubtitleCue* c = selectedCue(&track);
    if (!c) return false;
    for (int id : {0, 1, kTitlePage, kTransitionPage, kEffectsPage}) m_tabs->button(id)->setVisible(false);
    m_tabs->button(kSubtitlePage)->setVisible(true);
    m_tabs->button(kSubtitlePage)->setEnabled(true);
    m_content->setVisible(true);
    m_empty->setVisible(false);
    const int count = m_editor->selection()->ids().size();
    QString name = QString("ST%1 · %2").arg(track + 1).arg(c->text.section('\n', 0, 0).trimmed());
    if (count > 1) name += QString("  (+%1)").arg(count - 1);
    m_clipName->setFullText(name);
    m_clipName->setToolTip(subtitleTrackDisplayName(m_editor->project()->timeline().subtitles[track], track));
    m_tabs->button(kSubtitlePage)->setChecked(true);
    m_pages->setCurrentIndex(kSubtitlePage);
    for (const auto& fn : m_subRefreshers) fn();
    return true;
}
