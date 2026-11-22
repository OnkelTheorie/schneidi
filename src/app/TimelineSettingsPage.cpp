// SPDX-License-Identifier: AGPL-3.0-only
#include "app/TimelineSettingsPage.h"

#include "app/Theme.h"
#include "core/I18n.h"

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
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

void TimelineSettingsPage::store() const
{
    if (timeFormat() != TimelineOptions::timeFormat()) TimelineOptions::setTimeFormat(timeFormat());
    if (clickSetsPlayhead() != TimelineOptions::clickSetsPlayhead()) TimelineOptions::setClickSetsPlayhead(clickSetsPlayhead());
    if (follow() != TimelineOptions::follow()) TimelineOptions::setFollow(follow());
    if (zoomAnchor() != ZoomAnchor::mode()) ZoomAnchor::setMode(zoomAnchor());
    if (afterPlayheadMove() != PlayheadMove::mode()) PlayheadMove::setMode(afterPlayheadMove());
}
