#include "app/NormalizeDialog.h"
#include "core/I18n.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QSettings>
#include <QVBoxLayout>
#include <cmath>

namespace {
// Ziellautheit gängiger Plattformen/Normen (LUFS)
struct LoudnessPreset {
    const char* name;
    double lufs;
};
const LoudnessPreset kLoudnessPresets[] = {
    {"YouTube / Spotify (-14 LUFS)", -14.0},
    {"Apple Music / Podcast (-16 LUFS)", -16.0},
    {"EBU R128 (-23 LUFS)", -23.0},
    {"ATSC A/85 (-24 LUFS)", -24.0},
};
} // namespace

NormalizeDialog::NormalizeDialog(int clipCount, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(T("Audiopegel normalisieren"));
    QSettings settings;
    auto* form = new QFormLayout;

    m_mode = new QComboBox;
    m_mode->addItem(T("Spitzenpegel (Sample Peak Program)"));
    m_mode->addItem(T("Lautheit (ITU-R BS.1770-4)"));
    form->addRow(T("Normalisierung"), m_mode);

    m_preset = new QComboBox;
    for (const auto& p : kLoudnessPresets) m_preset->addItem(p.name, p.lufs);
    m_preset->addItem(T("Eigener Wert"));
    m_presetLabel = new QLabel(T("Vorgabe"));
    form->addRow(m_presetLabel, m_preset);

    m_target = new QDoubleSpinBox;
    m_target->setRange(-60, 0);
    m_target->setDecimals(1);
    m_target->setSingleStep(0.5);
    m_peakTarget = settings.value("audio/normalizeTarget", -9.0).toDouble(); // Standard wie DaVinci
    m_loudTarget = settings.value("audio/normalizeLoudness", -14.0).toDouble();
    form->addRow(T("Zielpegel"), m_target);

    m_relative = new QRadioButton(T("Relativ"));
    m_independent = new QRadioButton(T("Unabhängig"));
    m_independent->setToolTip(T("Jeder Clip erreicht den Zielpegel für sich"));
    auto* group = new QButtonGroup(this);
    group->addButton(m_relative);
    group->addButton(m_independent);
    (settings.value("audio/normalizeRelative", false).toBool() ? m_relative : m_independent)->setChecked(true);
    auto* levelRow = new QHBoxLayout;
    levelRow->addWidget(m_relative);
    levelRow->addWidget(m_independent);
    levelRow->addStretch();
    form->addRow(T("Pegel setzen"), levelRow);
    // Bei nur einem Clip sind beide gleich
    m_relative->setEnabled(clipCount > 1);
    m_independent->setEnabled(clipCount > 1);

    m_mode->setCurrentIndex(settings.value("audio/normalizeMode").toString() == "loudness" ? 1 : 0);
    connect(m_mode, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        // Wert des vorigen Modus merken, bevor die Einheit wechselt
        (mode() == Mode::Loudness ? m_peakTarget : m_loudTarget) = m_target->value();
        updateMode();
    });
    connect(m_preset, qOverload<int>(&QComboBox::activated), this, [this](int i) {
        const QVariant v = m_preset->itemData(i);
        if (v.isValid()) m_target->setValue(v.toDouble());
    });
    connect(m_target, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &NormalizeDialog::syncPreset);
    updateMode();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* lay = new QVBoxLayout(this);
    lay->addLayout(form);
    lay->addWidget(buttons);
}

void NormalizeDialog::updateMode()
{
    const bool loud = mode() == Mode::Loudness;
    m_preset->setVisible(loud);
    m_presetLabel->setVisible(loud);
    m_target->setSuffix(loud ? " LUFS" : " dBFS");
    m_target->setValue(loud ? m_loudTarget : m_peakTarget);
    syncPreset();
    m_relative->setToolTip(loud ? T("Alle Clips um denselben Betrag – ihre gemeinsame Lautheit erreicht den Zielpegel")
                                : T("Alle Clips um denselben Betrag – der lauteste erreicht den Zielpegel"));
    adjustSize();
}

// Vorgabe passend zum Zielwert anzeigen (sonst „Eigener Wert“)
void NormalizeDialog::syncPreset()
{
    if (mode() != Mode::Loudness) return;
    int idx = m_preset->count() - 1;
    for (int i = 0; i < m_preset->count() - 1; ++i)
        if (std::abs(m_preset->itemData(i).toDouble() - m_target->value()) < 0.05) idx = i;
    m_preset->setCurrentIndex(idx);
}

NormalizeDialog::Mode NormalizeDialog::mode() const
{
    return m_mode->currentIndex() == 1 ? Mode::Loudness : Mode::SamplePeak;
}
double NormalizeDialog::targetDb() const { return m_target->value(); }
bool NormalizeDialog::relative() const { return m_relative->isChecked(); }

void NormalizeDialog::accept()
{
    QSettings settings;
    const bool loud = mode() == Mode::Loudness;
    settings.setValue("audio/normalizeMode", loud ? "loudness" : "peak");
    settings.setValue(loud ? "audio/normalizeLoudness" : "audio/normalizeTarget", targetDb());
    if (m_relative->isEnabled()) settings.setValue("audio/normalizeRelative", relative());
    QDialog::accept();
}
