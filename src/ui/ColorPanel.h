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

// Color-Seite unten (wie DaVinci „Primaries – Color Wheels“, abgespeckt): Räder Lift/Gamma/Gain/Offset mit
// Master (Y) und R/G/B, darunter Kontrast, Pivot, Sättigung, Temperatur, Tönung, Belichtung, dazu LUT (.cube).
// Wirkt auf die ausgewählten Videoclips, ohne Auswahl auf den obersten Videoclip am Playhead (wie DaVinci:
// der Clip unter dem Playhead). Keyframes für die ganze Korrektur (◀ ◆ ▶), Ziehen = ein Undo-Schritt.
class ColorPanel : public QWidget {
    Q_OBJECT
public:
    explicit ColorPanel(Editor* editor, QWidget* parent = nullptr);

public slots:
    void setPlayhead(int frame);

signals:
    void seekRequested(int frame); // Keyframe-Pfeile

protected:
    void showEvent(QShowEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    struct Wheel {
        QString name;
        AnimParam p[4]; // Y, R, G, B
        double neutral; // Standardwert jedes Kanals
        double range;   // Kanal-Änderung am Radrand
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
    const Clip* current() const; // angezeigter Clip (frühester der Ziele)
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
