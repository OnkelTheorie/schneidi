// SPDX-License-Identifier: AGPL-3.0-only
#include "app/FitMusicDialog.h"
#include "app/Theme.h"
#include "core/I18n.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

// "31,5 s" in the UI language
QString secondsText(double s)
{
    return T("%1 s").arg(QString::number(s, 'f', 1).replace('.', I18n::decimalPoint()));
}

} // namespace

FitMusicDialog::FitMusicDialog(int songFrames, int timelineFrames, int markIn, int markOut, double fps, QWidget* parent)
    : QDialog(parent), m_song(songFrames), m_timelineFrames(timelineFrames), m_in(markIn), m_out(markOut),
      m_fps(fps > 0 ? fps : 25)
{
    setObjectName("fit_music_dialog");
    setWindowTitle(T("Musik auf Länge"));
    auto* intro = new QLabel(T("Kürzt den Song auf ganze Phrasen und Takte: Anfang und das echte Ende bleiben, "
                               "Teile aus der Mitte fallen weg. Ein Rückgängig-Schritt."));
    intro->setWordWrap(true);

    const bool hasRange = m_in >= 0 && m_out >= m_in;
    m_timeline = new QRadioButton(timelineFrames > 0 ? T("Länge der Timeline (%1)").arg(secondsText(timelineFrames / m_fps))
                                                     : T("Länge der Timeline"));
    m_timeline->setObjectName("fit_music_timeline");
    m_timeline->setEnabled(timelineFrames > 0);
    m_inOut = new QRadioButton(hasRange ? T("In/Out-Bereich (%1, beginnt am In-Punkt)").arg(secondsText((m_out - m_in + 1) / m_fps))
                                        : T("In/Out-Bereich (nicht gesetzt)"));
    m_inOut->setObjectName("fit_music_inout");
    m_inOut->setEnabled(hasRange);
    m_number = new QRadioButton(T("Eigene Länge"));
    m_number->setObjectName("fit_music_custom");
    m_seconds = new QDoubleSpinBox;
    m_seconds->setObjectName("fit_music_seconds");
    m_seconds->setRange(1, 3600);
    m_seconds->setDecimals(1);
    m_seconds->setSuffix(T(" s"));
    m_seconds->setValue(std::clamp(std::round(m_song / m_fps / 2), 1.0, 3600.0));
    auto* numberRow = new QHBoxLayout;
    numberRow->addWidget(m_number);
    numberRow->addWidget(m_seconds, 1);

    m_end = new QComboBox;
    m_end->setObjectName("fit_music_end");
    m_end->addItem(T("Eigenes Ende des Songs"));
    m_end->addItem(T("Ausblenden"));
    m_end->setToolTip(T("Eigenes Ende: die letzte Phrase mit Ausklang bleibt, gekürzt wird in der Mitte. Ausblenden: "
                        "der Song läuft vom Anfang und wird am Ende der Länge ausgeblendet."));

    m_hint = new QLabel;
    m_hint->setObjectName("fit_music_hint");
    m_hint->setWordWrap(true);

    auto* form = new QFormLayout;
    form->addRow(T("Ende"), m_end);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    QPushButton* ok = buttons->addButton(T("Kürzen"), QDialogButtonBox::AcceptRole);
    ok->setObjectName("fit_music_ok");
    ok->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* lay = new QVBoxLayout(this);
    lay->addWidget(intro);
    lay->addWidget(m_timeline);
    lay->addWidget(m_inOut);
    lay->addLayout(numberRow);
    lay->addLayout(form);
    lay->addWidget(m_hint);
    lay->addWidget(buttons);

    // The first choice that has a length: the In/Out range when set (the user marked what to fill), else the timeline
    setMode(hasRange ? Mode::InOut : timelineFrames > 0 ? Mode::Timeline : Mode::Seconds);
    for (QRadioButton* b : {m_timeline, m_inOut, m_number}) connect(b, &QRadioButton::toggled, this, &FitMusicDialog::updateHint);
    connect(m_seconds, &QDoubleSpinBox::valueChanged, this, [this] {
        m_number->setChecked(true);
        updateHint();
    });
    updateHint();
    setMinimumWidth(400);
}

FitMusicDialog::Mode FitMusicDialog::mode() const
{
    return m_timeline->isChecked() ? Mode::Timeline : m_inOut->isChecked() ? Mode::InOut : Mode::Seconds;
}

void FitMusicDialog::setMode(Mode m)
{
    (m == Mode::Timeline ? m_timeline : m == Mode::InOut ? m_inOut : m_number)->setChecked(true);
}

void FitMusicDialog::setSeconds(double s)
{
    m_seconds->setValue(s);
    m_number->setChecked(true);
}

double FitMusicDialog::seconds() const { return m_seconds->value(); }

bool FitMusicDialog::fadeOut() const { return m_end->currentIndex() == 1; }

int FitMusicDialog::targetFrames() const
{
    switch (mode()) {
    case Mode::Timeline: return m_timelineFrames;
    case Mode::InOut: return m_out - m_in + 1;
    case Mode::Seconds: break;
    }
    return int(std::lround(m_seconds->value() * m_fps));
}

QJsonObject FitMusicDialog::opFields() const
{
    QJsonObject o;
    if (mode() == Mode::Timeline) {
        o["length"] = "timeline";
    } else {
        o["length"] = targetFrames(); // frames
        if (mode() == Mode::InOut) o["at"] = m_in;
    }
    if (fadeOut()) o["end"] = "fade";
    return o;
}

void FitMusicDialog::updateHint()
{
    const int target = targetFrames();
    // Said before the op refuses it: the song cannot get longer by cutting
    if (target > m_song) {
        m_hint->setText(T("Der Song ist nur %1 lang – kürzer als die gewünschte Länge.").arg(secondsText(m_song / m_fps)));
        m_hint->setStyleSheet(QString("color: %1;").arg(Theme::warning.name()));
    } else {
        m_hint->setText(T("Song: %1 → %2").arg(secondsText(m_song / m_fps), secondsText(target / m_fps)));
        m_hint->setStyleSheet({});
    }
}
