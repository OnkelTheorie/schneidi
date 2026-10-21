#pragma once
#include "core/Interchange.h"

#include <QDialog>
#include <QVector>

class QButtonGroup;
class QLabel;
class QLineEdit;
class QPushButton;

// File › Export Timeline: chosen by the program that opens the file (Premiere, DaVinci …), not by format name;
// each entry says how to import it there and what comes along. Remembers the choice and folder (settings "interchange/").
class TimelineExportDialog : public QDialog {
    Q_OBJECT
public:
    // name: timeline name (file name suggestion); dir: folder when nothing is remembered yet
    TimelineExportDialog(const QString& name, const QString& dir, QWidget* parent = nullptr);
    QString path() const;           // with the extension of the chosen format
    Interchange::Format format() const;
    QString targetId() const;       // "premiere", "resolve", "resolve-xml", "edl"
    void selectTarget(const QString& id);

    void accept() override;

private:
    void updateTarget();
    void browse();

    QButtonGroup* m_group;
    QLabel* m_carries;
    QLineEdit* m_path;
    QPushButton* m_ok;
};
