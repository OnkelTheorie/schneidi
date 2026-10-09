#pragma once
#include "engine/Extensions.h"

#include <QDialog>
#include <QVector>

class QGridLayout;
class QLabel;
class QProgressBar;
class QPushButton;

// Workspace → Extensions: optional downloads (engine/Extensions), installed and removed here. One download at a time;
// an item's dependencies (whisper with a model) come along. Closing cancels a running download.
class ExtensionsDialog : public QDialog {
    Q_OBJECT
public:
    explicit ExtensionsDialog(QWidget* parent = nullptr);

    void reject() override;

private:
    void refresh();
    void install(const Extensions::Item& item);
    void startNext();

    struct Row {
        const Extensions::Item* item;
        QLabel* status;
        QPushButton* button;
    };
    QVector<Row> m_rows;
    QLabel* m_programNote = nullptr;
    QProgressBar* m_progress;
    QLabel* m_progressLabel;
    QPushButton* m_cancel;
    Extensions::Installer m_installer;
    QVector<const Extensions::Item*> m_queue;
    const Extensions::Item* m_current = nullptr;
};
