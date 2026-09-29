#include "app/NormalizeDialog.h"
#include "core/I18n.h"

#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QSettings>
#include <QVBoxLayout>

NormalizeDialog::NormalizeDialog(int clipCount, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(T("Audiopegel normalisieren"));
    QSettings settings;
    auto* form = new QFormLayout;
    form->addRow(T("Normalisierung"), new QLabel(T("Spitzenpegel (Sample Peak Program)")));

    m_target = new QDoubleSpinBox;
    m_target->setRange(-60, 0);
    m_target->setDecimals(1);
    m_target->setSingleStep(0.5);
    m_target->setSuffix(" dBFS");
    m_target->setValue(settings.value("audio/normalizeTarget", -9.0).toDouble()); // Standard wie DaVinci
    form->addRow(T("Zielpegel"), m_target);

    m_relative = new QRadioButton(T("Relativ"));
    m_relative->setToolTip(T("Alle Clips um denselben Betrag – der lauteste erreicht den Zielpegel"));
    m_independent = new QRadioButton(T("Unabhängig"));
    m_independent->setToolTip(T("Jeder Clip erreicht den Zielpegel für sich"));
    auto* group = new QButtonGroup(this);
    group->addButton(m_relative);
    group->addButton(m_independent);
    (settings.value("audio/normalizeRelative", false).toBool() ? m_relative : m_independent)->setChecked(true);
    auto* mode = new QHBoxLayout;
    mode->addWidget(m_relative);
    mode->addWidget(m_independent);
    mode->addStretch();
    form->addRow(T("Pegel setzen"), mode);
    // Bei nur einem Clip sind beide gleich
    m_relative->setEnabled(clipCount > 1);
    m_independent->setEnabled(clipCount > 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* lay = new QVBoxLayout(this);
    lay->addLayout(form);
    lay->addWidget(buttons);
}

double NormalizeDialog::targetDb() const { return m_target->value(); }
bool NormalizeDialog::relative() const { return m_relative->isChecked(); }

void NormalizeDialog::accept()
{
    QSettings settings;
    settings.setValue("audio/normalizeTarget", targetDb());
    if (m_relative->isEnabled()) settings.setValue("audio/normalizeRelative", relative());
    QDialog::accept();
}
