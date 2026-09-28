#pragma once
#include <QLineEdit>
#include <functional>

// Zahlenfeld wie in DaVinci: klicken = Wert eintippen (Enter übernimmt, Esc bricht ab),
// gedrückt halten und nach links/rechts ziehen = Wert ändern (Shift = fein).
class ScrubField : public QLineEdit {
public:
    ScrubField(double min, double max, double step, int decimals, QWidget* parent = nullptr);

    double value() const { return m_value; }
    void setValue(double v); // ohne Callback
    void setRange(double min, double max);
    void setSuffix(const QString& s) { m_suffix = s; showValue(); }
    // Anzeige für den kleinsten Wert (z. B. "-∞")
    void setMinimumText(const QString& t) { m_minText = t; showValue(); }

    std::function<void(double)> onChange; // bei jeder Änderung (auch während des Ziehens)
    std::function<void()> onFinish;       // Ziehen losgelassen / Eingabe übernommen

protected:
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void focusOutEvent(QFocusEvent*) override;

private:
    void showValue();
    void startEditing();
    void commitText();
    void change(double v);

    double m_value = 0, m_min, m_max, m_step;
    int m_decimals;
    QString m_suffix, m_minText;
    bool m_editing = false;
    bool m_pressed = false, m_dragging = false;
    int m_lastX = 0, m_pressX = 0;
    double m_accum = 0;
};
