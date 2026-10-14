#pragma once
#include <QDialog>

class QTableWidget;
class QLineEdit;

// Customize key bindings (like "Keyboard Customization" in DaVinci):
// Double-click a row and press a new key, column "Maustaste" for side buttons. Takes effect immediately, saved in keybindings.json.
class KeyBindingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit KeyBindingsDialog(QWidget* parent = nullptr);

private:
    void fill();
    void applyFilter();
    void editRow(int row);
    void editMouse(int row);
    void resetRow(int row);

    QLineEdit* m_filter;
    QTableWidget* m_table;
};
