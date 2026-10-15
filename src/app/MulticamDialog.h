#pragma once
#include "core/Multicam.h"

#include <QDialog>
#include <QStringList>

class Project;
class QComboBox;
class QLineEdit;

// "Create Multicam Clip Using Selected Clips" like DaVinci: name, how the angles are synced (sound, first source
// marker, source In point, start) and which audio stream the sound sync listens to. One angle per file, in the
// order given. The last sync settings are remembered.
class MulticamDialog : public QDialog {
    Q_OBJECT
public:
    MulticamDialog(const Project* project, const QStringList& files, const QString& defaultName,
                   QWidget* parent = nullptr);
    QString name() const;
    Multicam::SyncMethod method() const;
    int stream() const; // audio stream for the sound sync (0 = first)

    void accept() override;

private:
    QLineEdit* m_name;
    QComboBox* m_method;
    QComboBox* m_stream;
};
