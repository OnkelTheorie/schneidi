// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "core/StreamPresets.h"

#include <QDialog>

class QCheckBox;
class QTableWidget;

// Import presets for files with several audio streams (core/StreamPresets.h): the Mix switch and one row per rule
// (stream title, place / mute / leave out, volume, track name). Rows without a title are dropped. "Only for this
// project": the presets are saved in the project (its own, e.g. from its start template) instead of the settings.
class StreamPresetsDialog : public QDialog {
    Q_OBJECT
public:
    explicit StreamPresetsDialog(const StreamPresets& presets, QWidget* parent = nullptr);
    StreamPresets presets() const;
    void setProjectOnly(bool on);
    bool projectOnly() const;

private:
    void addRow(const StreamRule& rule);

    QCheckBox* m_projectOnly;
    QCheckBox* m_muteMix;
    QTableWidget* m_rules;
};
