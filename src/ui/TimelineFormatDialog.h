// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QDialog>
#include <QSize>

class QComboBox;
class QSpinBox;

// Resolution of one timeline (Media Pool › Timeline resolution…): "Use project settings" (default) or its own size
// from the presets (9:16 short next to a 16:9 edit) or free. size() empty = the project's.
class TimelineFormatDialog : public QDialog {
    Q_OBJECT
public:
    TimelineFormatDialog(const QString& timeline, QSize own, QSize project, QWidget* parent = nullptr);
    QSize size() const;

private:
    void chosen();
    void sizeEdited();

    QComboBox* m_choice;
    QSpinBox* m_width;
    QSpinBox* m_height;
    QSize m_project;
    bool m_updating = false;
};
