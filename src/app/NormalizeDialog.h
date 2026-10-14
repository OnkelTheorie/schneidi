#pragma once
#include <QDialog>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QRadioButton;

// "Normalize audio levels" like DaVinci (Normalize Audio Levels): mode Sample Peak Program (target level in dBFS)
// or loudness according to ITU-R BS.1770-4 (target loudness in LUFS, presets YouTube -14, EBU R128 -23 …),
// level relative (all clips equal) or independent (each clip individually). Last values are remembered.
class NormalizeDialog : public QDialog {
    Q_OBJECT
public:
    enum class Mode { SamplePeak, Loudness };

    explicit NormalizeDialog(int clipCount, QWidget* parent = nullptr);
    Mode mode() const;
    double targetDb() const; // dBFS resp. LUFS depending on the mode
    bool relative() const;

    void accept() override;

private:
    void updateMode();
    void syncPreset();

    QComboBox* m_mode;
    QComboBox* m_preset; // Loudness presets
    QLabel* m_presetLabel;
    QDoubleSpinBox* m_target;
    QRadioButton* m_relative;
    QRadioButton* m_independent;
    double m_peakTarget = -9.0, m_loudTarget = -14.0;
};
