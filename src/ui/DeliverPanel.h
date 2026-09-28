#pragma once
#include <QWidget>

class Project;
class Exporter;
class QComboBox;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QLabel;

// Render-Einstellungen links auf der Deliver-Seite (wie in DaVinci).
class DeliverPanel : public QWidget {
    Q_OBJECT
public:
    DeliverPanel(Project* project, QWidget* parent = nullptr);

private:
    void applyPreset(int index);
    void startRender();
    void browse();

    Project* m_project;
    Exporter* m_exporter;
    QComboBox* m_preset;
    QLineEdit* m_name;
    QLineEdit* m_folder;
    QComboBox* m_codec;
    QComboBox* m_resolution;
    QComboBox* m_quality;
    QPushButton* m_renderBtn;
    QProgressBar* m_progress;
    QLabel* m_status;
};
