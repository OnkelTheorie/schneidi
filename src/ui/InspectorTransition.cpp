// Inspector: transition page.
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

void Inspector::changeTransition(const std::function<void(TransitionStyle&)>& fn, const QString& mergeKey)
{
    TrackKind kind;
    TimelineOps::TransitionSpan span;
    if (!selectedTransition(&kind, &span)) return;
    TransitionStyle st = span.style;
    fn(st);
    m_editor->setTransitionStyle(span.leftId, span.rightId, st, mergeKey);
}

void Inspector::buildTransitionPage(QVBoxLayout* page)
{
    Section s = addSection(page, TrackKind::Video, T("Übergang"), {});
    auto combo = [] {
        auto* c = new QComboBox;
        c->setFocusPolicy(Qt::ClickFocus);
        c->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        return c;
    };
    // Zeile mit Beschriftung; rows sammelt beide Widgets, um sie je nach Art ein-/auszublenden
    auto addRow = [&](const QString& label, QWidget* w, QVector<QWidget*>* rows = nullptr, int span = 2) {
        QLabel* l = rowLabel(label);
        s.grid->addWidget(l, s.rows, 0);
        s.grid->addWidget(w, s.rows++, 1, 1, span);
        if (rows) *rows << l << w;
    };
    // Farbfeld (QColorDialog) rechtsbündig in einem eigenen Widget
    auto swatchRow = [&](QToolButton*& swatch, const QString& text,
                         const std::function<QColor&(TransitionStyle&)>& field) {
        swatch = new QToolButton;
        swatch->setFixedSize(46, 18);
        swatch->setFocusPolicy(Qt::NoFocus);
        swatch->setToolTip(T("%1 wählen").arg(text));
        connect(swatch, &QToolButton::clicked, this, [this, text, field] {
            TrackKind kind;
            TimelineOps::TransitionSpan span;
            if (!selectedTransition(&kind, &span)) return;
            const QColor col = QColorDialog::getColor(field(span.style), this, T("%1 wählen").arg(text));
            if (col.isValid()) changeTransition([&](TransitionStyle& st) { field(st) = col; });
        });
        auto* box = new QWidget;
        auto* lay = new QHBoxLayout(box);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addStretch(1);
        lay->addWidget(swatch);
        return box;
    };

    m_transType = combo();
    for (const auto& i : kTransitionTypes) m_transType->addItem(T(i.name), int(i.type));
    m_transAudioType = combo(); // Audio: Pegelkurve des Crossfades
    for (const auto& i : kAudioCurves) m_transAudioType->addItem(i.name, int(i.curve));
    auto* typeBox = new QWidget;
    auto* typeLay = new QHBoxLayout(typeBox);
    typeLay->setContentsMargins(0, 0, 0, 0);
    typeLay->addWidget(m_transType);
    typeLay->addWidget(m_transAudioType);
    addRow(T("Art"), typeBox);

    // Verlaufsblende: Verlaufsbild und Umkehren
    m_transLuma = combo();
    m_transLuma->setToolTip(T("Verlaufsbild: dunkle Stellen wechseln zuerst. Eigene Bilder in den Ordner "
                              "„Transitions“ (Effects Library → „Effekte importieren…“)"));
    addRow(T("Verlauf"), m_transLuma, &m_lumaRows);
    connect(m_transLuma, &QComboBox::activated, this, [this] {
        const QString path = m_transLuma->currentData().toString();
        if (!path.isEmpty()) changeTransition([path](TransitionStyle& st) { st.luma = path; });
    });
    m_transInvert = new QCheckBox(T("Umkehren"));
    m_transInvert->setFocusPolicy(Qt::ClickFocus);
    m_transInvert->setToolTip(T("Helle Stellen wechseln zuerst"));
    addRow({}, m_transInvert, &m_lumaRows);
    connect(m_transInvert, &QCheckBox::clicked, this, [this](bool on) {
        changeTransition([on](TransitionStyle& st) { st.invert = on; });
    });

    m_transLen = new ScrubField(0.04, 600, 0.04, 2);
    m_transLen->setSuffix(" s");
    m_transLen->onChange = [this](double sec) {
        const int frames = std::max(1, int(std::lround(sec * m_editor->project()->fps())));
        m_editor->setTransitionLength(m_transKey.leftId, m_transKey.rightId, frames, "transLenInspector");
    };
    m_transLen->onFinish = [this] { m_editor->project()->closeMerge(); };
    addRow(T("Dauer"), m_transLen, nullptr, 1);

    m_transAlign = combo();
    m_transAlign->addItems({T("Mitte auf Schnitt"), T("Beginn am Schnitt"), T("Ende am Schnitt")}); // Index = TransitionAlign
    m_transAlign->setToolTip(T("Lage zum Schnitt (nur bei Überblendung zwischen zwei Clips)"));
    addRow(T("Ausrichtung"), m_transAlign);

    // Abblende über Farbe ("Dip to Color Dissolve"): Farbe dazwischen
    addRow(T("Farbe"), swatchRow(m_transColor, T("Abblende-Farbe"), [](TransitionStyle& st) -> QColor& { return st.color; }),
           &m_dipRows);

    // Wischblende: Weichheit der Kante, Rand (Breite + Farbe)
    m_transSoft = new ScrubField(0, 100, 0.5, 0);
    m_transSoft->setSuffix(" %");
    m_transSoft->setToolTip(T("Weiche Kante (nur zwischen zwei Clips)"));
    m_transSoft->onChange = [this](double v) {
        changeTransition([v](TransitionStyle& st) { st.softness = v; }, "transSoftInspector");
    };
    m_transSoft->onFinish = [this] { m_editor->project()->closeMerge(); };
    addRow(T("Weichheit"), m_transSoft, &m_softRows, 1);

    m_transBorder = new ScrubField(0, 200, 0.5, 0);
    m_transBorder->setSuffix(" px");
    m_transBorder->onChange = [this](double v) {
        changeTransition([v](TransitionStyle& st) { st.border = v; }, "transBorderInspector");
    };
    m_transBorder->onFinish = [this] { m_editor->project()->closeMerge(); };
    addRow(T("Rand"), m_transBorder, &m_wipeRows, 1);
    addRow(T("Randfarbe"),
           swatchRow(m_transBorderColor, T("Randfarbe"), [](TransitionStyle& st) -> QColor& { return st.borderColor; }),
           &m_wipeRows);

    connect(m_transType, &QComboBox::activated, this, [this] {
        const auto type = TransitionType(m_transType->currentData().toInt());
        changeTransition([type](TransitionStyle& st) {
            st.type = type;
            if (st.isLuma() && st.luma.isEmpty()) st.luma = EffectFolders::builtinTransitions().value(0).path;
        });
    });
    connect(m_transAudioType, &QComboBox::activated, this, [this] {
        const auto curve = AudioCurve(m_transAudioType->currentData().toInt());
        changeTransition([curve](TransitionStyle& st) { st.audio = curve; });
    });
    connect(m_transAlign, &QComboBox::activated, this, [this](int i) {
        changeTransition([i](TransitionStyle& st) { st.align = TransitionAlign(i); });
    });
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
    for (int id : {0, 1, kTitlePage, kEffectsPage}) m_tabs->button(id)->setVisible(!on);
    m_tabs->button(kTransitionPage)->setVisible(on);
    if (!on) return false;
    m_transKey = m_editor->selection()->transition();
    m_content->setVisible(true);
    m_empty->setVisible(false);
    const bool video = kind == TrackKind::Video;
    QString name = video ? T(transitionTypeInfo(span.style.type).name) : QString(audioCurveInfo(span.style.audio).name);
    if (video && span.style.isLuma()) name = EffectFolders::displayName(span.style.luma);
    m_clipName->setText(name);
    m_clipName->setToolTip({});
    m_tabs->button(kTransitionPage)->setChecked(true);
    m_pages->setCurrentIndex(kTransitionPage);

    m_transType->setVisible(video);
    m_transAudioType->setVisible(!video);
    m_transType->setCurrentIndex(m_transType->findData(int(span.style.type)));
    m_transAudioType->setCurrentIndex(m_transAudioType->findData(int(span.style.audio)));
    m_transAlign->setCurrentIndex(int(span.style.align));
    // Felder nur für die passende Art (wie DaVinci je nach Übergang andere Parameter zeigt)
    for (QWidget* w : m_dipRows) w->setVisible(video && span.style.type == TransitionType::DipToColor);
    for (QWidget* w : m_wipeRows) w->setVisible(video && span.style.isWipe());
    for (QWidget* w : m_lumaRows) w->setVisible(video && span.style.isLuma());
    for (QWidget* w : m_softRows) w->setVisible(video && (span.style.isWipe() || span.style.isLuma()));
    if (video && span.style.isLuma()) {
        // Auswahl: mitgeliefert, dann eigene Bilder (mit Unterordner); fehlendes Bild als eigener Eintrag
        const QSignalBlocker block(m_transLuma);
        m_transLuma->clear();
        for (const auto& e : EffectFolders::builtinTransitions()) m_transLuma->addItem(e.name, e.path);
        const auto user = EffectFolders::userTransitions();
        if (!user.isEmpty()) m_transLuma->insertSeparator(m_transLuma->count());
        for (const auto& e : user) m_transLuma->addItem(e.group.isEmpty() ? e.name : e.group + '/' + e.name, e.path);
        int i = m_transLuma->findData(span.style.luma);
        if (i < 0) {
            const bool missing = !EffectFolders::isBuiltin(span.style.luma) && !QFileInfo::exists(span.style.luma);
            m_transLuma->addItem(EffectFolders::displayName(span.style.luma) + (missing ? T(" (fehlt)") : QString()),
                                 span.style.luma);
            i = m_transLuma->count() - 1;
        }
        m_transLuma->setCurrentIndex(i);
        m_transLuma->setToolTip(EffectFolders::isBuiltin(span.style.luma)
                                    ? T("Verlaufsbild: dunkle Stellen wechseln zuerst. Eigene Bilder in den Ordner "
                                        "„Transitions“ (Effects Library → „Effekte importieren…“)")
                                    : QDir::toNativeSeparators(span.style.luma));
        m_transInvert->setChecked(span.style.invert);
    }
    auto swatchStyle = [](const QColor& c) {
        return QString("QToolButton { background: %1; border: 1px solid %2; border-radius: 2px; }")
            .arg(c.name(), Theme::border.name());
    };
    m_transColor->setStyleSheet(swatchStyle(span.style.color));
    m_transBorderColor->setStyleSheet(swatchStyle(span.style.borderColor));
    m_transSoft->setValue(span.style.softness);
    m_transSoft->setEnabled(span.isDissolve()); // ins/aus dem Leeren: harte Kante
    m_transBorder->setValue(span.style.border);
    m_transAlign->setEnabled(span.isDissolve()); // Ein-/Ausblenden liegt immer im Clip
    m_transLen->setValue(double(span.length()) / std::max(1, m_editor->project()->fps()));
    return true;
}
