// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QLineEdit>
#include <functional>

// Number field like in DaVinci: click = type a value (Enter commits, Esc cancels),
// hold and drag left/right = change the value (Shift = fine).
class ScrubField : public QLineEdit {
public:
    ScrubField(double min, double max, double step, int decimals, QWidget* parent = nullptr);

    double value() const { return m_value; }
    void setValue(double v); // without callback
    void setRange(double min, double max);
    void setSuffix(const QString& s);
    // Display for the smallest value (e.g. "-∞")
    void setMinimumText(const QString& t);
    // Display for the value 0 when 0 means something else than a number (a range's end "0 = to the clip's end")
    void setZeroText(const QString& t);
    // May get narrower than its usual width when the panel is narrow (the Inspector): wider only for a long value,
    // never narrower than the text it shows (sign, suffix, "-∞")
    void setShrinkable(bool on);

    std::function<void(double)> onChange; // on every change (also while dragging)
    std::function<void()> onFinish;       // drag released / input committed

protected:
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
    void changeEvent(QEvent*) override; // font change: fitWidth()

private:
    void fitWidth();
    int widestText() const;
    void showValue();
    QString number() const;
    void startEditing();
    void commitText();
    void change(double v);

    double m_value = 0, m_min, m_max, m_step;
    int m_decimals;
    bool m_shrinkable = false;
    QString m_suffix, m_minText, m_zeroText;
    bool m_editing = false;
    bool m_pressed = false, m_dragging = false;
    int m_lastX = 0, m_pressX = 0;
    double m_accum = 0;
};
