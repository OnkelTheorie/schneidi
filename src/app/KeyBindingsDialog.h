#pragma once
#include <QDialog>

class QTableWidget;
class QLineEdit;

// Tastenbelegung anpassen (wie "Keyboard Customization" in DaVinci):
// Zeile doppelklicken und neue Taste drücken. Wirkt sofort, wird in keybindings.json gespeichert.
class KeyBindingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit KeyBindingsDialog(QWidget* parent = nullptr);

private:
    void fill();
    void applyFilter();
    void editRow(int row);
    void resetRow(int row);

    QLineEdit* m_filter;
    QTableWidget* m_table;
};
