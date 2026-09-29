#pragma once
#include <QDialog>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QRadioButton;

// „Audiopegel normalisieren“ wie DaVinci (Normalize Audio Levels): Modus Sample Peak Program (Zielpegel in dBFS)
// oder Lautheit nach ITU-R BS.1770-4 (Ziellautheit in LUFS, Vorgaben YouTube -14, EBU R128 -23 …),
// Pegel relativ (alle Clips gleich) oder unabhängig (jeder Clip einzeln). Letzte Werte werden gemerkt.
class NormalizeDialog : public QDialog {
    Q_OBJECT
public:
    enum class Mode { SamplePeak, Loudness };

    explicit NormalizeDialog(int clipCount, QWidget* parent = nullptr);
    Mode mode() const;
    double targetDb() const; // dBFS bzw. LUFS je nach Modus
    bool relative() const;

    void accept() override;

private:
    void updateMode();
    void syncPreset();

    QComboBox* m_mode;
    QComboBox* m_preset; // Lautheits-Vorgaben
    QLabel* m_presetLabel;
    QDoubleSpinBox* m_target;
    QRadioButton* m_relative;
    QRadioButton* m_independent;
    double m_peakTarget = -9.0, m_loudTarget = -14.0;
};
