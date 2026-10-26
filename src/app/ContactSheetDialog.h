#pragma once
#include <QDialog>

class QComboBox;
class QLineEdit;
class QSpinBox;

// File › Save Contact Sheet: still images of the open timeline in a grid with timecodes, as one picture (to send
// around, plan a cut, check a render at a glance). Remembers count, columns, size and folder (settings "sheet/").
class ContactSheetDialog : public QDialog {
    Q_OBJECT
public:
    // name: timeline name (file name suggestion); dir: folder when nothing is remembered yet;
    // hasInOut: offer "In/Out range"
    ContactSheetDialog(const QString& name, const QString& dir, bool hasInOut, QWidget* parent = nullptr);

    int count() const;
    int columns() const;   // 0 = automatic
    int tileWidth() const; // pixels per picture
    bool inOut() const;
    QString path() const;

    void accept() override;

private:
    void browse();

    QSpinBox* m_count;
    QComboBox* m_columns;
    QComboBox* m_width;
    QComboBox* m_range;
    QLineEdit* m_path;
};
