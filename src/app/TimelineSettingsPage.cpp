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

    // Playback: what it does after the user moved the playhead (PlayheadMove, read at every move)
    QFormLayout* play = group(lay, T("Wiedergabe"));
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

    lay->addStretch(1);
}

ZoomAnchor::Mode TimelineSettingsPage::zoomAnchor() const { return ZoomAnchor::Mode(m_zoomAnchor->currentData().toInt()); }

PlayheadMove::Mode TimelineSettingsPage::afterPlayheadMove() const
{
    return PlayheadMove::Mode(m_afterSeek->currentData().toInt());
}

void TimelineSettingsPage::store() const
{
    if (zoomAnchor() != ZoomAnchor::mode()) ZoomAnchor::setMode(zoomAnchor());
    if (afterPlayheadMove() != PlayheadMove::mode()) PlayheadMove::setMode(afterPlayheadMove());
}
