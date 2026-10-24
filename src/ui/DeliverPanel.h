#pragma once
#include "core/RenderJob.h"

#include <QPointer>
#include <QSize>
#include <QWidget>

class Project;
class RenderQueue;
class RenderQueuePanel;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QLabel;
class QToolButton;

// Deliver page like DaVinci: on the left the render settings with presets (built-in + custom) and
// "Add to Render Queue", on the right the queue (queuePanel(), placed by the main window).
class DeliverPanel : public QWidget {
    Q_OBJECT
public:
    DeliverPanel(Project* project, QWidget* parent = nullptr);
    ~DeliverPanel() override;

    RenderQueuePanel* queuePanel() const { return m_queuePanel; }
    RenderQueue* renderQueue() const { return m_queue; }

    // Current settings (resolution as timeline / shorter edge / fixed size, depending on the choice)
    RenderSettings settings() const;
    void setSettings(const RenderSettings& s);
    // Job from the current settings (path, size, range); without confirmation
    RenderJob makeJob() const;
    // With confirmation if the file exists / same target in the queue; false = not added
    bool addToQueue(bool confirm = true);

    // Presets (index in presets(), -1 = custom settings): select, save/delete custom ones
    int currentPreset() const;
    void selectPreset(int index);
    bool savePreset(const QString& name); // same name = overwrite; built-in ones not
    bool deletePreset(int index);         // only custom ones
    const QVector<RenderPreset>& presets() const { return m_presets; }

protected:
    void showEvent(QShowEvent* e) override;

private:
    void applyPreset(int comboIndex);
    void updateEncoders(); // encoder choices for the format (hardware ones once the graphics card probe is done)
    void updateRange();
    void updateFormat(); // resolutions matching the project settings
    void updateControls(); // fields depending on the format (audio only, quality)
    void reloadPresets(const QString& select = {});
    void syncPresetToSettings(); // setting changed -> matching preset or "Eigene Einstellungen"
    void presetMenu();
    void browse();
    int selectSize(QSize s); // choose resolution (add if needed)

    Project* m_project;
    RenderQueue* m_queue;
    // Lives elsewhere in the window (not a child), but hangs on m_queue -> dies with this panel
    QPointer<RenderQueuePanel> m_queuePanel;
    QVector<RenderPreset> m_presets; // built-in + custom; combo index = list index + 1
    QComboBox* m_preset;
    QToolButton* m_presetMenu;
    QLineEdit* m_name;
    QLineEdit* m_folder;
    QComboBox* m_format;
    QComboBox* m_resolution; // data: QSize
    QComboBox* m_rate; // output frame rate, data = QSize(num, den), empty = timeline
    QSize m_timelineSize; // last known timeline resolution
    QComboBox* m_quality;
    QComboBox* m_encoder; // video encoder (data: int(VideoEncoder)); hardware ones only when they work here
    VideoEncoder m_wantedEncoder = VideoEncoder::Auto; // from the preset/last choice (kept across format changes)
    QComboBox* m_audioBitrate;
    QComboBox* m_audioBits;
    QComboBox* m_threads; // threads for rendering (data: count, 0 = all)
    QComboBox* m_range; // whole timeline / In-Out range
    QCheckBox* m_smart; // smart render (H.264/H.265 only)
    QComboBox* m_subtitles; // Subtitles: none / burn in / SRT file (data: RenderSettings::Subtitles)
    bool m_hadRange = false;
    bool m_applying = false;
    int m_wantedBits = 24; // last chosen bit depth (stays on format change, if possible)
    QPushButton* m_addBtn;
    QLabel* m_status;
};
