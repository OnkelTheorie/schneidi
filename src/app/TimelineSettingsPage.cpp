// SPDX-License-Identifier: AGPL-3.0-only
#include "app/TimelineSettingsPage.h"

#include "app/Theme.h"
#include "core/I18n.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <cmath>
#include <QGridLayout>
#include <tuple>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QSpinBox>
#include <QLabel>
#include <QVBoxLayout>
#include <algorithm>

namespace {

QLabel* hint(const QString& text)
{
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    return l;
}

QFormLayout* group(QVBoxLayout* lay, const QString& title)
{
    auto* box = new QGroupBox(title);
    auto* form = new QFormLayout(box);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    lay->addWidget(box);
    return form;
}

QComboBox* combo(const char* name, const QVector<QPair<QString, int>>& items, int current)
{
    auto* c = new QComboBox;
    c->setObjectName(name);
    for (const auto& [text, value] : items) c->addItem(text, value);
    c->setCurrentIndex(std::max(0, c->findData(current)));
    return c;
}

} // namespace

TimelineSettingsPage::TimelineSettingsPage(QWidget* parent) : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(10);

    // Display: how times are shown (TimelineOptions::timeFormat, read at every paint)
    QFormLayout* display = group(lay, T("Anzeige"));
    {
        using TF = TimelineOptions::TimeFormat;
        const auto example = [](TF f) { return TimelineOptions::formatTime(112, 25, f); }; // 4.48 s at 25 fps
        m_timeFormat = combo("timeline_time_format",
                             {{T("Timecode (%1)").arg(example(TF::Timecode)), int(TF::Timecode)},
                              {T("Sekunden (%1)").arg(example(TF::Seconds)), int(TF::Seconds)},
                              {T("Frames (%1)").arg(example(TF::Frames)), int(TF::Frames)}},
                             int(TimelineOptions::timeFormat()));
    }
    display->addRow(T("Zeit anzeigen als"), m_timeFormat);
    display->addRow(hint(T("In Timeline, Vorschau, Studio, Trim-Ansicht und Media Pool. Eingabefelder verstehen jede "
                           "Schreibweise, „4,5s“ sind immer Sekunden und „112f“ immer Frames. Dateien (EDL, FCPXML) "
                           "behalten den Timecode.")));

    using TH = TimelineOptions::Thumbnails;
    m_thumbnails = combo("timeline_thumbnails",
                         {{T("Filmstreifen"), int(TH::Filmstrip)},
                          {T("Erstes Bild"), int(TH::Start)},
                          {T("Erstes und letztes Bild"), int(TH::StartEnd)},
                          {T("Keine"), int(TH::None)}},
                         int(TimelineOptions::thumbnails()));
    display->addRow(T("Bilder auf Videoclips"), m_thumbnails);
    display->addRow(hint(T("Weniger Bilder: mehr Übersicht bei vielen kurzen Clips, und schwache Rechner müssen beim "
                           "Scrollen weniger dekodieren.")));

    using WSh = TimelineOptions::WaveShape;
    using WSc = TimelineOptions::WaveScale;
    m_waveShape = combo("timeline_wave_shape",
                        {{T("Gespiegelt"), int(WSh::Mirrored)}, {T("Von unten"), int(WSh::Bottom)}},
                        int(TimelineOptions::waveShape()));
    display->addRow(T("Wellenform"), m_waveShape);
    m_waveScale = combo("timeline_wave_scale", {{T("dB"), int(WSc::Db)}, {T("Linear"), int(WSc::Linear)}},
                        int(TimelineOptions::waveScale()));
    display->addRow(T("Wellenform-Höhe"), m_waveScale);
    display->addRow(hint(T("Von unten (wie Premiere, Final Cut) ist auf flachen Spuren doppelt so hoch. dB zeigt "
                           "auch leise Sprache deutlich, linear die echte Lautstärke-Kurve (laute Stellen stechen "
                           "heraus).")));

    // Zoom and scroll: where wheel zoom stays put (ZoomAnchor; Edit timeline and Studio lanes, read live)
    QFormLayout* view = group(lay, T("Zoom und Scrollen"));
    m_zoomAnchor = combo("timeline_zoom_anchor",
                         {{T("Mausposition"), int(ZoomAnchor::Mode::Mouse)},
                          {T("Abspielmarke"), int(ZoomAnchor::Mode::Playhead)}},
                         int(ZoomAnchor::mode()));
    view->addRow(T("Zoom an"), m_zoomAnchor);
    view->addRow(hint(T("Wo das Mausrad zoomt. Abspielmarke: die rote Linie bleibt stehen, die Timeline wächst um sie "
                        "herum; ist sie gerade nicht zu sehen, zoomt es an der Maus. Tasten zoomen immer an der "
                        "Abspielmarke.")));

    // New clips: default lengths of stills and transitions (EditDefaults, applied after OK)
    QFormLayout* fresh = group(lay, T("Neue Clips"));
    const auto secondsBox = [](const char* name, double min, double max, double value) {
        auto* box = new QDoubleSpinBox;
        box->setObjectName(name);
        box->setRange(min, max);
        box->setDecimals(2);
        box->setSingleStep(0.5);
        box->setSuffix(" s");
        box->setValue(value);
        return box;
    };
    m_stillSeconds = secondsBox("timeline_still_seconds", 0.1, 600, TimelineOptions::stillSeconds());
    fresh->addRow(T("Standbilder"), m_stillSeconds);
    m_transitionSeconds = secondsBox("timeline_transition_seconds", 0.04, 30, TimelineOptions::transitionSeconds());
    fresh->addRow(T("Übergänge"), m_transitionSeconds);
    fresh->addRow(hint(T("So lang werden Bilder, die du ab jetzt importierst, und Übergänge ohne eigene Länge "
                         "(Strg+T, aus den Effekten gezogen). Schon importierte Bilder behalten ihre Länge.")));

    // Keys: the big step of the playhead and the big nudge (read at every key press)
    QFormLayout* keys = group(lay, T("Tasten"));
    const auto stepRow = [&](const char* name, TimelineOptions::Step step, QSpinBox*& count, QComboBox*& unit) {
        auto* row = new QWidget;
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        count = new QSpinBox;
        count->setObjectName(QString(name) + "_count");
        count->setRange(1, 999);
        count->setValue(step.count);
        unit = combo((QString(name) + "_unit").toUtf8().constData(), {{T("Frames"), 0}, {T("Sekunden"), 1}},
                     step.seconds ? 1 : 0);
        h->addWidget(count);
        h->addWidget(unit, 1);
        return row;
    };
    keys->addRow(T("Großer Schritt (Shift+Pfeil)"),
                 stepRow("timeline_big_step", TimelineOptions::bigStep(), m_stepCount, m_stepUnit));
    keys->addRow(T("Weit schieben (Shift+, und Shift+.)"),
                 stepRow("timeline_big_nudge", TimelineOptions::bigNudge(), m_nudgeCount, m_nudgeUnit));
    keys->addRow(hint(T("Großer Schritt: wie weit die Abspielmarke springt (1 Sekunde; Premiere 5 Frames, Final Cut "
                        "10 Frames). Weit schieben: wie weit Clips und Schnittpunkte rücken (5 Frames).")));

    // Snapping: what a drag snaps to and how near (read on every snap; on/off stays the magnet button, N)
    QFormLayout* snap = group(lay, T("Einrasten"));
    {
        auto* boxes = new QWidget;
        auto* grid = new QGridLayout(boxes);
        grid->setContentsMargins(0, 0, 0, 0);
        const QVector<std::tuple<int, const char*, QString>> targets = {
            {TimelineOptions::SnapClipEdges, "timeline_snap_clips", T("Clipkanten")},
            {TimelineOptions::SnapPlayhead, "timeline_snap_playhead", T("Abspielmarke")},
            {TimelineOptions::SnapMarkers, "timeline_snap_markers", T("Marker")},
            {TimelineOptions::SnapInOut, "timeline_snap_inout", T("In- und Out-Punkt")}};
        const int stored = TimelineOptions::snapTargets();
        for (int i = 0; i < targets.size(); ++i) {
            const auto& [flag, name, text] = targets[i];
            auto* box = new QCheckBox(text);
            box->setObjectName(name);
            box->setChecked(stored & flag);
            grid->addWidget(box, i / 2, i % 2);
            m_snapTargets.append({flag, box});
        }
        snap->addRow(T("Rastet ein an"), boxes);
    }
    using SR = TimelineOptions::SnapReach;
    m_snapReach = combo("timeline_snap_reach",
                        {{T("Klein"), int(SR::Small)}, {T("Mittel"), int(SR::Medium)}, {T("Groß"), int(SR::Large)}},
                        int(TimelineOptions::snapReach()));
    snap->addRow(T("Einrast-Abstand"), m_snapReach);
    snap->addRow(hint(T("Ein- und ausschalten mit dem Magnet-Knopf oder N, das bleibt nach dem Neustart. Einrast-Abstand: wie "
                        "nah eine Kante kommen muss. Der In/Out-Bereich rastet immer auch an Markern ein.")));

    // Playback: following the playhead (read at every play start), what it does after the user moved the playhead
    // (PlayheadMove, read at every move)
    QFormLayout* play = group(lay, T("Wiedergabe"));
    using F = TimelineOptions::Follow;
    m_follow = combo("timeline_follow_playback",
                     {{T("Aus"), int(F::Off)}, {T("Seitenweise"), int(F::Page)}, {T("Abspielmarke mittig"), int(F::Center)}},
                     int(TimelineOptions::follow()));
    play->addRow(T("Beim Abspielen mitscrollen"), m_follow);
    play->addRow(hint(T("Aus: die Timeline bleibt, wo du sie hinscrollst. Seitenweise: läuft die Abspielmarke aus dem "
                        "Bild, blättert die Timeline um. Mittig: die Timeline läuft unter der Abspielmarke durch. "
                        "Scrollst oder zoomst du beim Abspielen, bleibt sie bis zum nächsten Start stehen.")));
    m_space = combo("timeline_space_stops",
                    {{T("An der Stelle"), 0}, {T("Zurück zur Startposition"), 1}}, TimelineOptions::spaceReturns() ? 1 : 0);
    play->addRow(T("Leertaste hält an"), m_space);
    play->addRow(hint(T("Zurück zur Startposition: die Abspielmarke springt dorthin, wo du das Abspielen gestartet hast "
                        "(wie in Vegas), gut zum wiederholten Anhören eines Schnitts. K hält immer an der Stelle an; "
                        "„Stopp und zurück zur Startposition“ gibt es auch als eigene Taste (Tastenbelegung).")));
    m_afterSeek = combo("timeline_after_playhead_move",
                        {{T("Standard"), int(PlayheadMove::Mode::Standard)},
                         {T("Weiter wie vorher"), int(PlayheadMove::Mode::Continue)},
                         {T("Anhalten"), int(PlayheadMove::Mode::Stop)},
                         {T("Abspielen"), int(PlayheadMove::Mode::Play)}},
                        int(PlayheadMove::mode()));
    play->addRow(T("Nach dem Bewegen der Abspielmarke"), m_afterSeek);
    play->addRow(hint(T("Standard: Klicken und Ziehen in Lineal, Überblick und Studio spielt weiter, wenn es lief; die "
                        "Leiste unter der Vorschau und Sprungtasten (Pos1/Ende, Schnittpunkte, Marker) halten an. "
                        "Weiter wie vorher: lief es, spielt es an der neuen Stelle weiter, sonst bleibt es stehen. "
                        "Abspielen: startet beim Loslassen. Einzelbild-Schritte mit den Pfeiltasten halten immer an.")));

    // Mouse: what a click into the tracks does (read at every click)
    QFormLayout* mouse = group(lay, T("Maus"));
    m_click = combo("timeline_click_sets_playhead",
                    {{T("Nur auswählen"), 0}, {T("Auswählen und Abspielmarke setzen"), 1}},
                    TimelineOptions::clickSetsPlayhead() ? 1 : 0);
    mouse->addRow(T("Klick in die Spuren"), m_click);
    mouse->addRow(hint(T("Nur auswählen: die Abspielmarke setzt du im Lineal. Abspielmarke setzen: ein Klick auf einen "
                         "Clip oder eine leere Stelle setzt sie auch dorthin (wie in Vegas, Final Cut, Shotcut, "
                         "CapCut). Ziehen wählt weiter mit dem Rahmen aus bzw. verschiebt.")));

    lay->addStretch(1);
}

ZoomAnchor::Mode TimelineSettingsPage::zoomAnchor() const { return ZoomAnchor::Mode(m_zoomAnchor->currentData().toInt()); }

PlayheadMove::Mode TimelineSettingsPage::afterPlayheadMove() const
{
    return PlayheadMove::Mode(m_afterSeek->currentData().toInt());
}

TimelineOptions::Follow TimelineSettingsPage::follow() const
{
    return TimelineOptions::Follow(m_follow->currentData().toInt());
}

bool TimelineSettingsPage::clickSetsPlayhead() const { return m_click->currentData().toInt() == 1; }

TimelineOptions::TimeFormat TimelineSettingsPage::timeFormat() const
{
    return TimelineOptions::TimeFormat(m_timeFormat->currentData().toInt());
}

int TimelineSettingsPage::snapTargets() const
{
    int flags = 0;
    for (const auto& [flag, box] : m_snapTargets)
        if (box->isChecked()) flags |= flag;
    return flags;
}

TimelineOptions::SnapReach TimelineSettingsPage::snapReach() const
{
    return TimelineOptions::SnapReach(m_snapReach->currentData().toInt());
}

bool TimelineSettingsPage::spaceReturns() const { return m_space->currentData().toInt() == 1; }

double TimelineSettingsPage::stillSeconds() const { return m_stillSeconds->value(); }

double TimelineSettingsPage::transitionSeconds() const { return m_transitionSeconds->value(); }

TimelineOptions::Step TimelineSettingsPage::bigStep() const
{
    return {m_stepCount->value(), m_stepUnit->currentData().toInt() == 1};
}

TimelineOptions::Step TimelineSettingsPage::bigNudge() const
{
    return {m_nudgeCount->value(), m_nudgeUnit->currentData().toInt() == 1};
}

TimelineOptions::Thumbnails TimelineSettingsPage::thumbnails() const
{
    return TimelineOptions::Thumbnails(m_thumbnails->currentData().toInt());
}

TimelineOptions::WaveShape TimelineSettingsPage::waveShape() const
{
    return TimelineOptions::WaveShape(m_waveShape->currentData().toInt());
}

TimelineOptions::WaveScale TimelineSettingsPage::waveScale() const
{
    return TimelineOptions::WaveScale(m_waveScale->currentData().toInt());
}

void TimelineSettingsPage::store() const
{
    if (waveShape() != TimelineOptions::waveShape()) TimelineOptions::setWaveShape(waveShape());
    if (waveScale() != TimelineOptions::waveScale()) TimelineOptions::setWaveScale(waveScale());
    if (thumbnails() != TimelineOptions::thumbnails()) TimelineOptions::setThumbnails(thumbnails());
    if (!(bigStep() == TimelineOptions::bigStep())) TimelineOptions::setBigStep(bigStep());
    if (!(bigNudge() == TimelineOptions::bigNudge())) TimelineOptions::setBigNudge(bigNudge());
    if (std::abs(stillSeconds() - TimelineOptions::stillSeconds()) > 1e-9) TimelineOptions::setStillSeconds(stillSeconds());
    if (std::abs(transitionSeconds() - TimelineOptions::transitionSeconds()) > 1e-9)
        TimelineOptions::setTransitionSeconds(transitionSeconds());
    if (spaceReturns() != TimelineOptions::spaceReturns()) TimelineOptions::setSpaceReturns(spaceReturns());
    if (snapTargets() != TimelineOptions::snapTargets()) TimelineOptions::setSnapTargets(snapTargets());
    if (snapReach() != TimelineOptions::snapReach()) TimelineOptions::setSnapReach(snapReach());
    if (timeFormat() != TimelineOptions::timeFormat()) TimelineOptions::setTimeFormat(timeFormat());
    if (clickSetsPlayhead() != TimelineOptions::clickSetsPlayhead()) TimelineOptions::setClickSetsPlayhead(clickSetsPlayhead());
    if (follow() != TimelineOptions::follow()) TimelineOptions::setFollow(follow());
    if (zoomAnchor() != ZoomAnchor::mode()) ZoomAnchor::setMode(zoomAnchor());
    if (afterPlayheadMove() != PlayheadMove::mode()) PlayheadMove::setMode(afterPlayheadMove());
}
