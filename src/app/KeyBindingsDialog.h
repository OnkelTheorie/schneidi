// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "app/InputBindings.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QTableWidget;
class QLineEdit;

// Customize key bindings (like "Keyboard Customization" in DaVinci):
// Two slots per action ("Taste 1"/"Taste 2"), each takes a key or a mouse side button: double-click a cell and press it.
// Takes effect immediately, saved in keybindings.json.
class KeyBindingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit KeyBindingsDialog(QWidget* parent = nullptr);
    // Import a Premiere .kys file as a preset of its own and apply it; shows "N taken, M unknown" (the "Import…" button
    // after its file dialog)
    bool importFile(const QString& path);

private:
    void fill();
    void applyFilter();
    void editSlot(int row, int slot);
    void resetRow(int row);
    // "M was on „Add marker“ - removed there": what an assignment took from other actions
    void showTaken(const QVector<InputBindings::Taken>& taken);

    QLineEdit* m_filter;
    QTableWidget* m_table;
    QComboBox* m_preset;
    QLabel* m_comeFrom;
    QLabel* m_summary;
    QLabel* m_notice;
};
