// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/studio/StudioPage.h"

#include "app/InputBindings.h"
#include "app/Theme.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Editor.h"
#include "ui/ElidedLabel.h"
#include "ui/studio/StudioMixer.h"
#include "ui/studio/StudioTools.h"
#include "ui/studio/StudioTracks.h"

#include <QAction>
#include <QButtonGroup>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSettings>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
// Pen smoothing levels: frames to each side of the moving average
struct Smoothing {
    const char* name;
    int frames;
};
constexpr Smoothing kSmoothing[] = {{N_("Aus"), 0}, {N_("Leicht"), 2}, {N_("Mittel"), 5}, {N_("Stark"), 12}};
constexpr int kDefaultSmoothing = 1;
} // namespace

StudioPage::StudioPage(Editor* editor, Engine* engine, MediaCache* cache, QWidget* parent)
    : QWidget(parent), m_editor(editor)
{
    setObjectName("studio_page");
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    m_root = new QSplitter(Qt::Horizontal);
    m_left = new QSplitter(Qt::Vertical);
    m_side = new QSplitter(Qt::Vertical);

    // Tracks with their tool row
    auto* tracksBox = new QWidget;
    tracksBox->setObjectName("Panel");
    auto* tracksLay = new QVBoxLayout(tracksBox);
    tracksLay->setContentsMargins(0, 0, 0, 0);
    tracksLay->setSpacing(0);
    auto* bar = new QWidget;
    bar->setObjectName("PanelTitle");
    bar->setAttribute(Qt::WA_StyledBackground);
    bar->setStyleSheet(QString("QWidget#PanelTitle { background: %1; }").arg(Theme::panelHeader.name()));
    // Two rows: the tools (and Fit) on top, what the pen/points write and the smoothing below - one row was wider
    // than a 1024 px window with a large font
    auto* barRows = new QVBoxLayout(bar);
    barRows->setContentsMargins(4, 2, 4, 2);
    barRows->setSpacing(2);
    auto* barLay = new QHBoxLayout;
    barLay->setSpacing(4);
    barRows->addLayout(barLay);
    auto* optionsLay = new QHBoxLayout;
    optionsLay->setSpacing(4);
    barRows->addLayout(optionsLay);
    // The title gives way first in a narrow row (a long language, a large font): the tools stay whole
    auto* title = new ElidedLabel;
    title->setFullText(T("Tonspuren"));
    title->setObjectName("PanelTitle");
    barLay->addWidget(title, 1);
    barLay->addSpacing(8);
    auto tool = [&](const QString& text, const QString& name, const QString& tip) {
        auto* b = new QToolButton;
        b->setText(text);
        b->setObjectName(name);
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolTip(tip);
        b->setStyleSheet(QString("QToolButton { background: %1; color: %2; padding: 1px 8px; border: none; }"
                                 "QToolButton:checked { background: %3; color: %4; }")
                             .arg(Theme::control.name(), Theme::text.name(), Theme::primary.name(), Theme::onPrimary.name()));
        barLay->addWidget(b);
        return b;
    };
    m_select = tool(T("Auswahl"), "studio_tool_select", T("Klick setzt den Abspielkopf und wählt den Clip"));
    m_points = tool(T("Punkte"), "studio_tool_points",
                    T("Klick auf die Lautstärke-Linie setzt einen Punkt, ziehen verschiebt ihn, Rechtsklick löscht, "
                      "Doppelklick oder Alt+Klick setzt ihn auf 0 dB"));
    m_pen = tool(T("Stift"), "studio_tool_pen",
                 T("Lautstärke zeichnen: über die Spur ziehen – oben lauter, unten leiser (ein Strich = ein Schritt "
                   "zum Rückgängigmachen)"));
    m_pen->setProperty("tourTarget", "studio_pen");
    // The keys (the window's tool actions pick these tools on this page; a changed binding shows here)
    for (auto [b, id] : {std::pair{m_select, "tool_select"}, std::pair{m_pen, "tool_draw"}})
        if (QAction* a = InputBindings::instance().action(id); a && !a->shortcut().isEmpty())
            b->setToolTip(T("%1 (Taste %2)").arg(b->toolTip(), a->shortcut().toString(QKeySequence::NativeText)));
    auto* group = new QButtonGroup(this);
    group->addButton(m_select);
    group->addButton(m_points);
    group->addButton(m_pen);
    // Points (rubber band) is the everyday way to shape a level; the pen is for quick rides
    m_points->setChecked(true);
    auto* fit = new QToolButton;
    fit->setObjectName("studio_fit");
    fit->setText(T("Einpassen"));
    fit->setFocusPolicy(Qt::NoFocus);
    fit->setToolTip(T("Das ganze Projekt in die Breite einpassen"));
    barLay->addSpacing(8);
    barLay->addWidget(fit);
    // Write mode (touch): playing, a moved fader or pan knob writes the track's automation
    auto* write = tool(T("Schreiben"), "studio_write",
                       T("Schreibmodus: beim Abspielen schreiben Fader und Pan-Regler die Automation ihrer Spur; beim "
                         "Loslassen gleitet der Wert zur Kurve zurück. Ein Durchlauf = ein Schritt zum Rückgängigmachen"));
    barLay = optionsLay; // the second row (tool() adds there now)
    // What the pen writes: the track's automation (a curve over the timeline's time, also between clips; it stays
    // when clips move) or the volume keyframes of the clips under the stroke (they move with their clip)
    auto* penClip = tool(T("Clip"), "studio_pen_clip",
                         T("Der Stift zeichnet Lautstärke-Keyframes der Clips unter dem Strich (sie wandern mit dem Clip)"));
    auto* penTrack = tool(T("Spur"), "studio_pen_track",
                          T("Der Stift zeichnet die Automation der Spur: eine Kurve über die Zeit der Timeline, auch "
                            "zwischen den Clips (der Fader verschiebt die ganze Kurve)"));
    auto* targetGroup = new QButtonGroup(this);
    targetGroup->addButton(penClip);
    targetGroup->addButton(penTrack);
    // Which curve of the track (track mode): its level or its pan
    barLay->addSpacing(6);
    auto* autoVolume = tool(T("Pegel"), "studio_auto_volume", T("Punkte und Stift bearbeiten die Lautstärke-Automation der Spur"));
    auto* autoPan = tool(T("Pan"), "studio_auto_pan",
                         T("Punkte und Stift bearbeiten die Pan-Automation der Spur (oben rechts, Mitte Mitte, unten links)"));
    auto* autoGroup = new QButtonGroup(this);
    autoGroup->addButton(autoVolume);
    autoGroup->addButton(autoPan);
    autoVolume->setChecked(true);
    barLay->addSpacing(6);
    // The label gives way in a narrow row (the box keeps its tooltip)
    auto* smoothLabel = new ElidedLabel;
    smoothLabel->setFullText(T("Glätten"));
    barLay->addWidget(smoothLabel);
    m_smoothing = new QComboBox;
    m_smoothing->setObjectName("studio_smoothing");
    m_smoothing->setToolTip(T("Wie stark ein gezeichneter Strich geglättet wird"));
    for (const Smoothing& s : kSmoothing) m_smoothing->addItem(T(s.name), s.frames);
    barLay->addWidget(m_smoothing);
    barLay->addStretch(1);
    // The tools drawer under the lanes: closed until wanted (the lanes keep the height)
    m_toolsButton = tool(T("Werkzeuge"), "studio_tools_toggle",
                         T("Werkzeuge ein- und ausklappen: Stimme säubern, Musik absenken, Lautheit auf Ziel, Aufnahme, "
                           "Effektklänge"));
    tracksLay->addWidget(bar);
    m_tracks = new StudioTracks(editor, cache);
    tracksLay->addWidget(m_tracks, 1);

    connect(m_select, &QToolButton::toggled, this, [this](bool on) {
        if (on) m_tracks->setTool(StudioTracks::Tool::Select);
    });
    connect(m_points, &QToolButton::toggled, this, [this](bool on) {
        if (on) m_tracks->setTool(StudioTracks::Tool::Points);
    });
    connect(m_pen, &QToolButton::toggled, this, [this](bool on) {
        if (on) m_tracks->setTool(StudioTracks::Tool::Pen);
    });
    connect(m_tracks, &StudioTracks::toolChanged, this, [this](StudioTracks::Tool t) {
        (t == StudioTracks::Tool::Pen ? m_pen : t == StudioTracks::Tool::Points ? m_points : m_select)->setChecked(true);
    });
    m_tracks->setTool(StudioTracks::Tool::Points);
    // The pen's target is the user's choice too (default: the track)
    const bool clipTarget = QSettings().value("studio/penTarget").toString() == QLatin1String("clip");
    (clipTarget ? penClip : penTrack)->setChecked(true);
    m_tracks->setPenTarget(clipTarget ? StudioTracks::PenTarget::Clip : StudioTracks::PenTarget::Track);
    autoVolume->setEnabled(!clipTarget);
    autoPan->setEnabled(!clipTarget);
    connect(penClip, &QToolButton::toggled, this, [this, autoVolume, autoPan](bool on) {
        autoVolume->setEnabled(!on); // clip keyframes are volume only
        autoPan->setEnabled(!on);
        if (!on) return;
        m_tracks->setPenTarget(StudioTracks::PenTarget::Clip);
        QSettings().setValue("studio/penTarget", "clip");
    });
    connect(autoPan, &QToolButton::toggled, this, [this](bool on) {
        m_tracks->setAutoTarget(on ? AutoTarget::Pan : AutoTarget::Volume);
    });
    connect(penTrack, &QToolButton::toggled, this, [this](bool on) {
        if (!on) return;
        m_tracks->setPenTarget(StudioTracks::PenTarget::Track);
        QSettings().setValue("studio/penTarget", "track");
    });
    // The smoothing is the user's choice, kept between sessions
    const int saved = QSettings().value("studio/smoothing", kDefaultSmoothing).toInt();
    m_smoothing->setCurrentIndex(std::clamp(saved, 0, int(std::size(kSmoothing)) - 1));
    m_tracks->setSmoothing(m_smoothing->currentData().toInt());
    connect(m_smoothing, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_tracks->setSmoothing(m_smoothing->itemData(i).toInt());
        QSettings().setValue("studio/smoothing", i);
    });
    connect(fit, &QToolButton::clicked, m_tracks, &StudioTracks::fit);

    m_left->addWidget(tracksBox);
    m_tools = new StudioTools(editor);
    m_left->addWidget(m_tools);
    m_left->setStretchFactor(0, 1);
    m_left->setChildrenCollapsible(false);
    m_tools->hide();
    connect(m_toolsButton, &QToolButton::toggled, this, &StudioPage::setToolsOpen);
    connect(m_tools, &StudioTools::closeRequested, this, [this] { setToolsOpen(false); });

    // The right column: the viewer comes on top (index 0) when the page shows, then the loudness and the mixer
    m_mixer = new StudioMixer(editor, engine);
    connect(write, &QToolButton::toggled, m_mixer, &StudioMixer::setWriteMode); // off by default
    m_sideBox = new QWidget;
    m_sideBox->setObjectName("studio_side");
    auto* sideLay = new QVBoxLayout(m_sideBox);
    sideLay->setContentsMargins(0, 0, 0, 0);
    sideLay->setSpacing(0);
    sideLay->addWidget(m_mixer->loudness());
    sideLay->addWidget(m_mixer, 1);
    m_side->addWidget(m_sideBox);
    m_side->setChildrenCollapsible(false);

    m_root->addWidget(m_left);
    m_root->addWidget(m_side);
    m_root->setStretchFactor(0, 1);
    m_root->setStretchFactor(1, 0);
    m_root->setChildrenCollapsible(false);
    lay->addWidget(m_root);
}

bool StudioPage::toolsOpen() const { return !m_tools->isHidden(); }

void StudioPage::setToolsOpen(bool open)
{
    if (m_toolsButton->isChecked() != open) {
        const QSignalBlocker block(m_toolsButton);
        m_toolsButton->setChecked(open);
    }
    if (open == toolsOpen()) return;
    m_tools->setVisible(open);
    if (!open) return;
    // As tall as the tools need (their list), at most 45 % of the lanes' height; a size dragged before stays
    QList<int> sizes = m_left->sizes();
    const int total = sizes.value(0) + sizes.value(1);
    if (total <= 0) return; // not laid out yet (the page's first showing sets the sizes)
    if (sizes.value(1) <= 0 || sizes.value(1) > total * 0.6) {
        const int want = std::max(m_tools->sizeHint().height(), m_tools->minimumSizeHint().height()) + 40;
        const int h = std::clamp(want, std::min(160, total / 2), std::max(1, int(total * 0.45)));
        m_left->setSizes({std::max(1, total - h), h});
    }
}

int StudioPage::defaultSideWidth(int pageWidth) const
{
    // A few strips beside the master (more tracks scroll), a fifth of the page at least (the viewer), at most 42 %
    const int tracks = std::clamp(int(m_editor->project()->timeline().audio.size()), 3, 5);
    const int strips = m_mixer->preferredWidth(tracks) + 2;
    const int want = std::max(strips, int(pageWidth * 0.2));
    return std::clamp(want, std::min(300, pageWidth / 2), std::max(300, int(pageWidth * 0.42)));
}

int StudioPage::defaultViewerHeight(int sideWidth, int pageHeight) const
{
    // The picture 16:9 across the column plus the viewer's controls, as long as the mixer keeps its height
    const int want = sideWidth * 9 / 16 + 80;
    const int mixerNeeds = m_sideBox->minimumSizeHint().height() + 40;
    return std::clamp(want, std::min(120, pageHeight / 3), std::max(120, pageHeight - mixerNeeds));
}
