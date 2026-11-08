// SPDX-License-Identifier: AGPL-3.0-only
#include "app/EqDialog.h"

#include "core/Editor.h"
#include "core/Equalizer.h"
#include "core/Project.h"
#include "ui/EqCurve.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QVBoxLayout>
#include <cmath>

namespace {
QString shown(double v)
{
    QString s = QString::number(v, 'g', 6);
    return s.replace('.', QLocale().decimalPoint());
}

bool parsed(const QLineEdit* e, double* v)
{
    bool ok = false;
    const double x = QString(e->text()).trimmed().replace(',', '.').toDouble(&ok);
    if (ok) *v = x;
    return ok;
}

QString bandName(EqBand::Kind k)
{
    switch (k) {
    case EqBand::Kind::HighPass: return T("Hochpass");
    case EqBand::Kind::LowShelf: return T("Low Shelf");
    case EqBand::Kind::HighShelf: return T("High Shelf");
    case EqBand::Kind::LowPass: return T("Tiefpass");
    case EqBand::Kind::Bell: break;
    }
    return T("Glocke");
}
} // namespace

EqDialog::EqDialog(Editor* editor, int track, QWidget* parent) : QDialog(parent), m_editor(editor), m_track(track)
{
    setWindowTitle(T("EQ – %1").arg(trackShortName({TrackKind::Audio, track})));
    auto* root = new QVBoxLayout(this);
    m_on = new QCheckBox(T("EQ an"));
    m_on->setObjectName("eqOn");
    root->addWidget(m_on);
    m_curve = new EqCurve(editor, track);
    root->addWidget(m_curve, 1);

    auto* grid = new QGridLayout;
    grid->addWidget(new QLabel(T("Frequenz (Hz)")), 2, 0);
    grid->addWidget(new QLabel(T("Verstärkung (dB)")), 3, 0);
    grid->addWidget(new QLabel("Q"), 4, 0);
    const TrackEq defaults;
    for (int i = 0; i < 6; ++i) {
        BandRow& row = m_bands[size_t(i)];
        row.on = new QCheckBox(QString("%1 %2").arg(i + 1).arg(bandName(defaults.bands[size_t(i)].kind)));
        grid->addWidget(row.on, 1, i + 1);
        auto field = [&](int r) {
            auto* e = new QLineEdit;
            e->setMaximumWidth(80);
            grid->addWidget(e, r, i + 1);
            connect(e, &QLineEdit::editingFinished, this, [this, i] { commit(i); });
            return e;
        };
        row.freq = field(2);
        row.gain = field(3);
        row.q = field(4);
        const EqBand::Kind k = defaults.bands[size_t(i)].kind;
        if (k == EqBand::Kind::HighPass || k == EqBand::Kind::LowPass) row.gain->setEnabled(false);
        connect(row.on, &QCheckBox::toggled, this, [this, i] { commit(i); });
    }
    root->addLayout(grid);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(m_on, &QCheckBox::toggled, this, [this](bool on) {
        const Timeline& tl = m_editor->project()->timeline();
        if (m_track < 0 || m_track >= tl.audio.size()) return;
        TrackEq eq = tl.audio[m_track].eq;
        eq.on = on;
        m_editor->setTrackEq(m_track, eq);
    });
    connect(editor->project(), &Project::timelineChanged, this, &EqDialog::sync);
    sync();
}

void EqDialog::sync()
{
    const Timeline& tl = m_editor->project()->timeline();
    if (m_track < 0 || m_track >= tl.audio.size()) return;
    const TrackEq& eq = tl.audio[m_track].eq;
    {
        const QSignalBlocker block(m_on);
        m_on->setChecked(eq.on);
    }
    for (int i = 0; i < 6; ++i) {
        const EqBand& b = eq.bands[size_t(i)];
        BandRow& row = m_bands[size_t(i)];
        const QSignalBlocker block(row.on);
        row.on->setChecked(b.on);
        row.freq->setText(shown(std::round(b.freq)));
        row.gain->setText(shown(b.gainDb));
        row.q->setText(shown(b.q));
    }
}

void EqDialog::commit(int band)
{
    const Timeline& tl = m_editor->project()->timeline();
    if (m_track < 0 || m_track >= tl.audio.size()) return;
    TrackEq eq = tl.audio[m_track].eq;
    EqBand& b = eq.bands[size_t(band)];
    const BandRow& row = m_bands[size_t(band)];
    b.on = row.on->isChecked();
    double freq = 0;
    if (parsed(row.freq, &freq) && std::round(freq) != std::round(b.freq)) b.freq = freq; // shown rounded
    parsed(row.gain, &b.gainDb);
    parsed(row.q, &b.q);
    if (eq != tl.audio[m_track].eq) eq.on = true; // changing a band switches the EQ on (like dragging)
    m_editor->setTrackEq(m_track, eq);
    sync();
}
