#include "app/ClipSpeedDialog.h"
#include "core/I18n.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

#include <cmath>

ClipSpeedDialog::ClipSpeedDialog(const Editor::Retime& current, int clipLength, double fps, QWidget* parent)
    : QDialog(parent), m_projectFps(fps), m_clipLength(clipLength), m_baseLength(clipLength * current.speed)
{
    setWindowTitle(T("Clip-Geschwindigkeit ändern"));
    auto* form = new QFormLayout;

    m_percent = new QDoubleSpinBox;
    m_percent->setRange(1, 10000);
    m_percent->setDecimals(1);
    m_percent->setSuffix(" %");
    m_percent->setValue(current.speed * 100);
    form->addRow(T("Geschwindigkeit"), m_percent);

    m_fps = new QDoubleSpinBox;
    m_fps->setRange(0.01, 10000);
    m_fps->setDecimals(3);
    m_fps->setValue(current.speed * fps);
    form->addRow(T("Bilder pro Sekunde"), m_fps);

    m_duration = new QLabel;
    form->addRow(T("Dauer"), m_duration);

    m_reverse = new QCheckBox(T("Rückwärts"));
    m_reverse->setChecked(current.reverse);
    m_freeze = new QCheckBox(T("Standbild"));
    m_freeze->setChecked(current.freeze);
    m_pitch = new QCheckBox(T("Tonhöhe halten"));
    m_pitch->setChecked(current.keepPitch);
    m_ripple = new QCheckBox(T("Nachfolgende Clips verschieben (Ripple)"));
    m_ripple->setChecked(true);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* lay = new QVBoxLayout(this);
    lay->addLayout(form);
    for (QCheckBox* b : {m_reverse, m_freeze, m_pitch, m_ripple}) lay->addWidget(b);
    lay->addWidget(buttons);

    connect(m_percent, &QDoubleSpinBox::valueChanged, this, &ClipSpeedDialog::percentEdited);
    connect(m_fps, &QDoubleSpinBox::valueChanged, this, &ClipSpeedDialog::fpsEdited);
    connect(m_ripple, &QCheckBox::toggled, this, &ClipSpeedDialog::updateDuration);
    // Standbild: Geschwindigkeit spielt keine Rolle (wie DaVinci ausgegraut)
    connect(m_freeze, &QCheckBox::toggled, this, [this](bool on) {
        for (QWidget* w : {static_cast<QWidget*>(m_percent), static_cast<QWidget*>(m_fps),
                           static_cast<QWidget*>(m_reverse), static_cast<QWidget*>(m_pitch)})
            w->setEnabled(!on);
        updateDuration();
    });
    m_freeze->toggled(m_freeze->isChecked());
}

void ClipSpeedDialog::percentEdited()
{
    if (m_updating) return;
    m_updating = true;
    m_fps->setValue(m_percent->value() / 100 * m_projectFps);
    m_updating = false;
    updateDuration();
}

void ClipSpeedDialog::fpsEdited()
{
    if (m_updating) return;
    m_updating = true;
    m_percent->setValue(m_fps->value() / m_projectFps * 100);
    m_updating = false;
    updateDuration();
}

void ClipSpeedDialog::updateDuration()
{
    const bool keepLength = !m_ripple->isChecked() || m_freeze->isChecked();
    const int f = keepLength ? m_clipLength : std::max(1, int(std::lround(m_baseLength / (m_percent->value() / 100))));
    const int fps = std::max(1, int(std::lround(m_projectFps)));
    m_duration->setText(QString("%1:%2:%3")
                            .arg(f / fps / 60, 2, 10, QChar('0'))
                            .arg(f / fps % 60, 2, 10, QChar('0'))
                            .arg(f % fps, 2, 10, QChar('0')));
}

Editor::Retime ClipSpeedDialog::retime() const
{
    return {m_percent->value() / 100, m_reverse->isChecked(), m_freeze->isChecked(), m_pitch->isChecked()};
}

bool ClipSpeedDialog::ripple() const { return m_ripple->isChecked(); }
