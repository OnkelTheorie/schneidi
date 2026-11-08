// SPDX-License-Identifier: AGPL-3.0-only
#include "app/DynamicsDialog.h"

#include "core/Dynamics.h"
#include "core/Editor.h"
#include "core/Project.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QVBoxLayout>

namespace {
// Number in the field: decimal point of the UI language, no group separator; both '.' and ',' are read
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
} // namespace

DynamicsDialog::DynamicsDialog(Editor* editor, int track, QWidget* parent)
    : QDialog(parent), m_editor(editor), m_track(track)
{
    setWindowTitle(T("Kompressor – %1").arg(trackShortName({TrackKind::Audio, track})));
    auto* root = new QVBoxLayout(this);
    m_on = new QCheckBox(T("Kompressor an"));
    m_on->setObjectName("dynamicsOn");
    root->addWidget(m_on);
    auto* form = new QFormLayout;
    auto field = [&](const char* name, const QString& label, const QString& unit, const QString& tip) {
        auto* e = new QLineEdit;
        e->setObjectName(name);
        e->setToolTip(tip);
        e->setMaximumWidth(90);
        auto* row = new QHBoxLayout;
        row->addWidget(e);
        row->addWidget(new QLabel(unit));
        row->addStretch(1);
        form->addRow(label, row);
        connect(e, &QLineEdit::editingFinished, this, &DynamicsDialog::commit);
        return e;
    };
    m_threshold = field("threshold", T("Threshold"), "dB", T("Ab diesem Pegel wird komprimiert (-60 … 0 dB)"));
    m_ratio = field("ratio", T("Ratio"), ": 1", T("Wie stark über dem Threshold verringert wird (1 … 100)"));
    m_attack = field("attack", T("Attack"), "ms", T("Wie schnell der Kompressor auf lauten Ton reagiert"));
    m_release = field("release", T("Release"), "ms", T("Wie schnell er danach wieder loslässt"));
    m_knee = field("knee", T("Knee"), "dB", T("Weicher Übergang um den Threshold (0 = hart)"));
    m_makeup = field("makeup", T("Make-up"), "dB", T("Verstärkung nach dem Komprimieren"));
    root->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(m_on, &QCheckBox::toggled, this, &DynamicsDialog::commit);
    connect(editor->project(), &Project::timelineChanged, this, &DynamicsDialog::sync);
    sync();
}

void DynamicsDialog::sync()
{
    const Timeline& tl = m_editor->project()->timeline();
    if (m_track < 0 || m_track >= tl.audio.size()) return;
    const TrackDynamics& d = tl.audio[m_track].dynamics;
    const QSignalBlocker block(m_on);
    m_on->setChecked(d.on);
    const std::pair<QLineEdit*, double> values[] = {{m_threshold, d.thresholdDb}, {m_ratio, d.ratio},
                                                    {m_attack, d.attackMs},       {m_release, d.releaseMs},
                                                    {m_knee, d.kneeDb},           {m_makeup, d.makeupDb}};
    for (const auto& [e, v] : values) e->setText(shown(v));
}

void DynamicsDialog::commit()
{
    const Timeline& tl = m_editor->project()->timeline();
    if (m_track < 0 || m_track >= tl.audio.size()) return;
    TrackDynamics d = tl.audio[m_track].dynamics;
    d.on = m_on->isChecked();
    parsed(m_threshold, &d.thresholdDb);
    parsed(m_ratio, &d.ratio);
    parsed(m_attack, &d.attackMs);
    parsed(m_release, &d.releaseMs);
    parsed(m_knee, &d.kneeDb);
    parsed(m_makeup, &d.makeupDb);
    m_editor->setTrackDynamics(m_track, d);
    sync(); // clamped values / invalid text back to the model's value
}
