// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QDialog>

class QTableWidget;
class QLineEdit;

// Customize key bindings (like "Keyboard Customization" in DaVinci):
// Two slots per action ("Taste 1"/"Taste 2"), each takes a key or a mouse side button: double-click a cell and press it.
// Takes effect immediately, saved in keybindings.json.
class KeyBindingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit KeyBindingsDialog(QWidget* parent = nullptr);

private:
    void fill();
    void applyFilter();
    void editSlot(int row, int slot);
    void resetRow(int row);

    QLineEdit* m_filter;
    QTableWidget* m_table;
};
