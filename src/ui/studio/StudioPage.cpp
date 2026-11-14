// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/studio/StudioPage.h"

#include "app/Theme.h"
#include "core/I18n.h"
#include "ui/ElidedLabel.h"
#include "ui/studio/StudioMixer.h"
#include "ui/studio/StudioTracks.h"

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

StudioPage::StudioPage(Editor* editor, Engine* engine, MediaCache* cache, QWidget* parent) : QWidget(parent)
{
    setObjectName("studio_page");
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    m_root = new QSplitter(Qt::Vertical);
    m_top = new QSplitter(Qt::Horizontal);

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
    auto* barLay = new QHBoxLayout(bar);
    barLay->setContentsMargins(6, 2, 6, 2);
    barLay->setSpacing(4);
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
    m_pen = tool(T("Stift"), "studio_tool_pen",
                 T("Lautstärke zeichnen: über die Clips ziehen – oben lauter, unten leiser (ein Strich = ein Schritt "
                   "zum Rückgängigmachen)"));
    m_pen->setProperty("tourTarget", "studio_pen");
    auto* group = new QButtonGroup(this);
    group->addButton(m_select);
    group->addButton(m_pen);
    m_select->setChecked(true);
    barLay->addSpacing(8);
    auto* smoothLabel = new QLabel(T("Glätten"));
    barLay->addWidget(smoothLabel);
    m_smoothing = new QComboBox;
    m_smoothing->setObjectName("studio_smoothing");
    m_smoothing->setToolTip(T("Wie stark ein gezeichneter Strich geglättet wird"));
    for (const Smoothing& s : kSmoothing) m_smoothing->addItem(T(s.name), s.frames);
    barLay->addWidget(m_smoothing);
    barLay->addSpacing(8);
    auto* fit = new QToolButton;
    fit->setObjectName("studio_fit");
    fit->setText(T("Einpassen"));
    fit->setFocusPolicy(Qt::NoFocus);
    fit->setToolTip(T("Das ganze Projekt in die Breite einpassen"));
    barLay->addWidget(fit);
    tracksLay->addWidget(bar);
    m_tracks = new StudioTracks(editor, cache);
    tracksLay->addWidget(m_tracks, 1);

    connect(m_select, &QToolButton::toggled, this, [this](bool on) {
        if (on) m_tracks->setTool(StudioTracks::Tool::Select);
    });
    connect(m_pen, &QToolButton::toggled, this, [this](bool on) {
        if (on) m_tracks->setTool(StudioTracks::Tool::Pen);
    });
    connect(m_tracks, &StudioTracks::toolChanged, this, [this](StudioTracks::Tool t) {
        (t == StudioTracks::Tool::Pen ? m_pen : m_select)->setChecked(true);
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

    m_top->addWidget(tracksBox); // the viewer comes in front of it (index 0) when the page shows
    m_top->setStretchFactor(0, 1);
    m_mixer = new StudioMixer(editor, engine);
    m_root->addWidget(m_top);
    m_root->addWidget(m_mixer);
    m_root->setStretchFactor(0, 1);
    m_root->setStretchFactor(1, 1);
    m_root->setChildrenCollapsible(false);
    lay->addWidget(m_root);
}
