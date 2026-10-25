#pragma once
#include "core/Types.h"

#include <QTimer>
#include <QVector>
#include <QWidget>
#include <atomic>
#include <functional>
#include <memory>

class Editor;
class QCheckBox;
class QComboBox;
class QLabel;
class QThread;
class QToolButton;
class ScrubField;
class ColorWheel;
class ViewerOverlay;
class WindowOverlay;

// Color page at the bottom (like DaVinci "Primaries – Color Wheels", slimmed down): Lift/Gamma/Gain/Offset wheels with
// Master (Y) and R/G/B, below them Contrast, Pivot, Saturation, Temperature, Tint, Exposure, plus LUT (.cube).
// Acts on the selected video clips, without a selection on the topmost video clip at the playhead (like DaVinci:
// the clip under the playhead). Keyframes for the whole correction (◀ ◆ ▶), dragging = one undo step.
// Windows (shape masks, core/GradeWindows) at the right: the wheels and sliders act on the chosen layer – the
// correction of the whole picture or one window; the chosen window is drawn in the viewer (overlay()).
class ColorPanel : public QWidget {
    Q_OBJECT
public:
    explicit ColorPanel(Editor* editor, QWidget* parent = nullptr);
    ~ColorPanel() override;

    // Grade copy/paste like DaVinci's Color page: copy from the shown clip, paste onto the targets (selection or the
    // clip under the playhead), "=" / "-" = grade of the clip one/two before the shown one. false = nothing to do
    bool copyGrade();
    bool pasteGrade();
    bool gradeFromPrevious(int back);

    // Outline of the chosen window for the viewer (draws only while the panel is shown)
    ViewerOverlay* overlay() const;
    int layer() const { return m_layer; } // 0 = whole picture, 1..4 = window
    void setLayer(int layer);
    void nextLayer(); // whole picture -> window 1 -> … -> whole picture
    // New window (shape "rectangle"/"ellipse") on the shown clip and the other targets, then it is the chosen layer
    void addWindow(const QString& shape);
    // Tracker of the chosen window from the playhead to the clip end (forward) or start, in the background
    void startTracking(bool forward);
    void stopTracking();
    // Another project replaces this one: stop and drop the result (clip ids start over in every project)
    void discardTracking();
    bool tracking() const { return m_trackThread != nullptr; }

public slots:
    void setPlayhead(int frame);

signals:
    void seekRequested(int frame); // keyframe arrows
    void trackingFinished();

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    struct Wheel {
        QString name;
        AnimParam p[4]; // Y, R, G, B
        double neutral; // default value of each channel
        double range;   // channel change at the wheel edge
        ColorWheel* wheel = nullptr;
        ScrubField* field[4] = {};
    };
    struct Slider {
        AnimParam p;
        ScrubField* field = nullptr;
    };

    QWidget* buildWheel(Wheel& w);
    QWidget* buildWindows();
    // Parameter of the chosen layer for a grade parameter (window: its own copy, core/GradeWindows)
    AnimParam mapped(AnimParam p) const;
    QVector<AnimParam> layerParams() const; // everything the keyframe button keys on the chosen layer
    void refreshWindows(const Clip* c, int t);
    QWidget* buildSlider(Slider& s, const QString& label, double min, double max, double step, int decimals);
    QToolButton* smallButton(const QString& text, const QString& tip);
    QVector<int> targets() const;
    const Clip* current() const; // displayed clip (earliest of the targets)
    int localFrame(const Clip& c) const;
    void setValues(const QVector<QPair<AnimParam, double>>& values, const QString& text, const QString& key);
    void finish();
    void loadLut();
    void refresh();

    Editor* m_editor;
    int m_playhead = 0;
    QVector<Wheel> m_wheels;
    QVector<Slider> m_sliders;
    QLabel* m_clipName = nullptr;
    QLabel* m_lutName = nullptr;
    QToolButton* m_lutClear = nullptr;
    QToolButton* m_enabled = nullptr;
    QToolButton* m_keyPrev = nullptr;
    QToolButton* m_keyDiamond = nullptr;
    QToolButton* m_keyNext = nullptr;
    QWidget* m_body = nullptr;
    QLabel* m_empty = nullptr;
    QLabel* m_title = nullptr;

    // Windows
    int m_layer = 0;
    WindowOverlay* m_overlay = nullptr;
    QVector<QToolButton*> m_layerButtons; // 0 = whole picture, 1..4 = windows
    QToolButton* m_addRect = nullptr;
    QToolButton* m_addEllipse = nullptr;
    QWidget* m_windowControls = nullptr;
    QComboBox* m_shape = nullptr;
    QComboBox* m_fill = nullptr;
    QCheckBox* m_invert = nullptr;
    QToolButton* m_fillColor = nullptr;
    struct WindowField {
        QString key;
        ScrubField* field = nullptr;
    };
    QVector<WindowField> m_windowFields;

    // Tracking
    QToolButton* m_trackBack = nullptr;
    QToolButton* m_trackForward = nullptr;
    QToolButton* m_trackStop = nullptr;
    QLabel* m_trackStatus = nullptr;
    QThread* m_trackThread = nullptr;
    std::shared_ptr<std::atomic<bool>> m_trackCancel;
    std::shared_ptr<std::atomic<int>> m_trackDone, m_trackTotal;
    int m_trackGeneration = 0; // +1 per run and by discardTracking(): a finished run of another one is dropped
    QTimer m_trackPoll;
};
