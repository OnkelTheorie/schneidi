// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "core/Types.h"

#include <QHash>
#include <QIcon>
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
class CurveEditor;
class MasterWheel;
class QualifierBar;
class QStackedWidget;
class ViewerOverlay;
class WindowOverlay;

// Color page at the bottom, three palettes (header buttons): the wheels (Lift/Gamma/Gain/Offset with a master wheel
// for Y and the R/G/B balance, below them Contrast, Pivot, Saturation, Temperature, Tint, Exposure, plus LUT), the
// curves (core/GradeCurves) and the qualifier (core/GradeQualifier: pick colours, show the selection as a mask).
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
    int layer() const { return m_layer; } // 0 = whole picture, 1..GradeWindows::kMax = window, GradeQualifier::kLayer
    enum Palette { Wheels, CurvesPalette, QualifierPalette, StillsPalette };
    void setPalette(int palette, bool remember = true); // remember = the next start shows it again
    int palette() const { return m_palette; }
    // A colour picked for the qualifier (MainWindow's pipette): its range is set around it
    void applyQualifierColor(const QColor& color);
    // A grabbed frame (MainWindow's answer to stillGrabRequested): a still with the shown clip's grade
    void addStill(const QImage& picture, const QString& timecode);
    int comparedStill() const { return m_compareId; } // 0 = no compare
    void setComparedStill(int stillId);               // 0 = off
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
    // "Start the Color tour" where no clip is shown; off while a tour runs (it would sit beside the tour's bubble)
    void setTourButtonAllowed(bool on);

public slots:
    void setPlayhead(int frame);
    // The preview's frame (after the grade): the curves' histogram, while the curves show (at most every 150 ms)
    void setFrame(const QImage& frame);

signals:
    void seekRequested(int frame); // keyframe arrows
    void trackingFinished();
    void tourRequested(); // the button shown without a clip: the Color tour
    void qualifierPickRequested(int clipId); // the qualifier's pipette: pick on the clip's picture before it
    void qualifierMaskChanged(int clipId);   // "show mask" for this clip (0 = off), for Engine::setQualifierMask
    void stillGrabRequested();               // "grab still": the frame shown in the viewer, back via addStill

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    struct Wheel {
        QString name;
        AnimParam p[4]; // Y, R, G, B
        double neutral; // default value of each channel
        double range;   // channel change at the wheel edge
        ColorWheel* wheel = nullptr;
        MasterWheel* master = nullptr;
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
    QWidget* buildCurves();
    QWidget* buildQualifier();
    void refreshCurves(const Clip* c);
    void refreshQualifier(const Clip* c, int t);
    void updateMask(); // tells the viewer which clip shows its qualifier mask
    void layoutWheels(int columns); // 4 in a row or 2 x 2
    void fitWidth();                // the wheels' layout for the width the page has
    QWidget* buildStills();
    void refreshStills();
    int selectedStill() const;
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
    class ElidedLabel* m_lutName = nullptr;
    QToolButton* m_lutClear = nullptr;
    QToolButton* m_enabled = nullptr;
    QToolButton* m_keyPrev = nullptr;
    QToolButton* m_keyDiamond = nullptr;
    QToolButton* m_keyNext = nullptr;
    QWidget* m_body = nullptr;
    QWidget* m_bodyScroll = nullptr; // m_body in a scroll area
    QWidget* m_empty = nullptr;
    class QPushButton* m_tourButton = nullptr;
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

    // Wheels' layout
    class QGridLayout* m_wheelGrid = nullptr;
    QVector<QWidget*> m_wheelBoxes, m_wheelUnders;
    int m_wheelColumns = 0, m_fourWide = 0, m_twoWide = 0;
    QWidget* m_windowsBox = nullptr;
    class QBoxLayout* m_outer = nullptr;
    QWidget* m_windowsLine = nullptr;
    // Palettes
    int m_palette = Wheels;
    QStackedWidget* m_palettes = nullptr;
    QVector<QToolButton*> m_paletteButtons;
    // Curves
    CurveEditor* m_curve = nullptr;
    QVector<QToolButton*> m_curveButtons; // by GradeCurves::Kind
    QToolButton* m_curveReset = nullptr;
    QLabel* m_curveNote = nullptr;
    QImage m_histFrame, m_lastFrame;
    QToolButton* m_stillSide = nullptr;
    QTimer m_histTimer;
    // Qualifier
    QWidget* m_qualEmpty = nullptr;
    QWidget* m_qualControls = nullptr;
    QCheckBox* m_qualInvert = nullptr;
    QComboBox* m_qualWindow = nullptr;
    QToolButton* m_qualMask = nullptr;
    struct QualField {
        QString key;
        ScrubField* field = nullptr;
    };
    QVector<QualField> m_qualFields;
    QualifierBar* m_qualBars[3] = {};
    bool m_showMask = false;
    int m_maskSent = 0;
    // Stills
    class QListWidget* m_stillList = nullptr;
    QToolButton* m_stillCompare = nullptr;
    QToolButton* m_stillApply = nullptr;
    QToolButton* m_stillGrab = nullptr;
    // decoded once per still, only when the stills show (ids are per project); with the hash of the image it was
    // decoded from: a deleted or undone still's id comes back for the next one
    QHash<int, std::pair<size_t, QIcon>> m_stillIcons;
    QToolButton* m_stillRemove = nullptr;
    int m_compareId = 0;

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
