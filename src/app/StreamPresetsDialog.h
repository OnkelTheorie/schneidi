#pragma once
#include "core/StreamPresets.h"

#include <QDialog>

class QCheckBox;
class QTableWidget;

// Import presets for files with several audio streams (core/StreamPresets.h): the Mix switch and one row per rule
// (stream title, place / mute / leave out, volume, track name). Rows without a title are dropped.
class StreamPresetsDialog : public QDialog {
    Q_OBJECT
public:
    explicit StreamPresetsDialog(const StreamPresets& presets, QWidget* parent = nullptr);
    StreamPresets presets() const;

private:
    void addRow(const StreamRule& rule);

    QCheckBox* m_muteMix;
    QTableWidget* m_rules;
};
