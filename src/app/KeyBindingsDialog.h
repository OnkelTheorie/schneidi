// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "app/InputBindings.h"

#include <QDialog>
#include <QHash>

class QComboBox;
class QLabel;
class QTableWidget;
class QLineEdit;

// Customize key bindings (like "Keyboard Customization" in DaVinci):
// Two slots per action ("Taste 1"/"Taste 2"), each takes a key or a mouse side button: double-click a cell and press it.
// Below: the mouse wheel in the timeline, one action per modifier, with quick sets like other editors.
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
    void fillWheel(); // the wheel rows and the quick set from InputBindings
    QWidget* buildWheel();
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
    QComboBox* m_wheelPreset = nullptr;
    QHash<QString, QComboBox*> m_wheel; // modifier ("none", "ctrl", ...) -> its action
};
