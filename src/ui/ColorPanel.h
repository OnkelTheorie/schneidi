#pragma once
#include "core/Types.h"

#include <QVector>
#include <QWidget>
#include <functional>

class Editor;
class QLabel;
class QToolButton;
class ScrubField;
class ColorWheel;

// Color page at the bottom (like DaVinci "Primaries – Color Wheels", slimmed down): Lift/Gamma/Gain/Offset wheels with
// Master (Y) and R/G/B, below them Contrast, Pivot, Saturation, Temperature, Tint, Exposure, plus LUT (.cube).
// Acts on the selected video clips, without a selection on the topmost video clip at the playhead (like DaVinci:
// the clip under the playhead). Keyframes for the whole correction (◀ ◆ ▶), dragging = one undo step.
class ColorPanel : public QWidget {
    Q_OBJECT
public:
    explicit ColorPanel(Editor* editor, QWidget* parent = nullptr);

    // Grade copy/paste like DaVinci's Color page: copy from the shown clip, paste onto the targets (selection or the
    // clip under the playhead), "=" / "-" = grade of the clip one/two before the shown one. false = nothing to do
    bool copyGrade();
    bool pasteGrade();
    bool gradeFromPrevious(int back);

public slots:
    void setPlayhead(int frame);

signals:
    void seekRequested(int frame); // keyframe arrows

protected:
    void showEvent(QShowEvent* e) override;
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
};
