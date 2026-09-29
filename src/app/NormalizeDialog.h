#pragma once
#include <QDialog>

class QDoubleSpinBox;
class QRadioButton;

// „Audiopegel normalisieren“ wie DaVinci (Normalize Audio Levels): Modus Sample Peak Program,
// Zielpegel in dBFS, Pegel relativ (alle Clips gleich) oder unabhängig (jeder Clip einzeln).
// Letzte Werte werden gemerkt.
class NormalizeDialog : public QDialog {
    Q_OBJECT
public:
    explicit NormalizeDialog(int clipCount, QWidget* parent = nullptr);
    double targetDb() const;
    bool relative() const;

    void accept() override;

private:
    QDoubleSpinBox* m_target;
    QRadioButton* m_relative;
    QRadioButton* m_independent;
};
