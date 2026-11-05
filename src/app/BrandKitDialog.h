// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "core/BrandKit.h"

#include <QDialog>

#include <optional>

class QCheckBox;
class QComboBox;
class QFontComboBox;
class QLabel;
class QRadioButton;
class QSpinBox;
class QToolButton;

// Workspace › Brand Kit…: logo (corner, size), up to four colours, two fonts, intro/outro clip. Edits the global kit
// (all projects) or, switched to "this project only", the open project's own; "use the global kit" drops the
// project's own. The caller saves the global kit and applies the result to the open project (one undo step).
class BrandKitDialog : public QDialog {
    Q_OBJECT
public:
    BrandKitDialog(const BrandKit& global, const std::optional<BrandKit>& project, QWidget* parent = nullptr);

    BrandKit global() const { return m_global; }
    std::optional<BrandKit> project() const { return m_projectOnly ? std::optional<BrandKit>(m_project) : std::nullopt; }

    // Test helpers (without file and colour dialogs)
    void setProjectOnly(bool on);
    void setColor(int index, const QColor& c); // invalid = remove
    void setFont(int index, const QString& family); // empty = none
    void setFile(const QString& which, const QString& path); // "logo", "intro", "outro"

private:
    BrandKit& kit() { return m_projectOnly ? m_project : m_global; }
    void load();  // the edited kit into the widgets
    void store(); // the widgets into the edited kit
    void pickColor(int index);
    void pickFile(const QString& which);

    BrandKit m_global, m_project;
    bool m_projectOnly = false;
    bool m_hadOwn = false; // the project had its own kit when the dialog opened
    bool m_loading = false;
    QRadioButton* m_globalButton;
    QRadioButton* m_projectButton;
    QLabel* m_scopeHint;
    QHash<QString, QLabel*> m_files;
    QComboBox* m_corner;
    QSpinBox* m_size;
    QVector<QToolButton*> m_colors;
    QVector<QCheckBox*> m_fontOn;
    QVector<QFontComboBox*> m_fonts;
};
