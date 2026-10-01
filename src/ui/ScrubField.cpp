#include "ui/ScrubField.h"

#include "app/Theme.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <algorithm>
#include <cmath>

ScrubField::ScrubField(double min, double max, double step, int decimals, QWidget* parent)
    : QLineEdit(parent), m_min(min), m_max(max), m_step(step), m_decimals(decimals)
{
    setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    setFixedWidth(64);
    setReadOnly(true);
    setCursor(Qt::SizeHorCursor);
    setFocusPolicy(Qt::ClickFocus);
    setStyleSheet(QString("QLineEdit { background: %1; border: 1px solid %1; border-radius: 2px; padding: 1px 4px; }"
                          "QLineEdit:focus { border-color: %2; }")
                      .arg(Theme::field.name(), Theme::primary.name()));
    showValue();
}

void ScrubField::setValue(double v)
{
    m_value = std::clamp(v, m_min, m_max);
    if (!m_editing) showValue();
}

void ScrubField::setRange(double min, double max)
{
    m_min = min;
    m_max = max;
    setValue(m_value);
}

void ScrubField::showValue()
{
    if (!m_minText.isEmpty() && m_value <= m_min) setText(m_minText);
    else setText(QString::number(m_value, 'f', m_decimals) + m_suffix);
}

void ScrubField::change(double v)
{
    const double p = std::pow(10.0, m_decimals);
    v = std::clamp(std::round(v * p) / p, m_min, m_max);
    if (v == m_value) return;
    m_value = v;
    showValue();
    if (onChange) onChange(v);
}

void ScrubField::mousePressEvent(QMouseEvent* e)
{
    if (m_editing || e->button() != Qt::LeftButton) return QLineEdit::mousePressEvent(e);
    m_pressed = true;
    m_dragging = false;
    m_pressX = m_lastX = int(e->position().x());
    m_accum = 0;
}

void ScrubField::mouseMoveEvent(QMouseEvent* e)
{
    if (m_editing || !m_pressed) return QLineEdit::mouseMoveEvent(e);
    const int x = int(e->position().x());
    if (!m_dragging && std::abs(x - m_pressX) < 3) return;
    m_dragging = true;
    const double factor = (e->modifiers() & Qt::ShiftModifier) ? 0.1 : 1.0;
    m_accum += (x - m_lastX) * m_step * factor;
    m_lastX = x;
    // Rest unterhalb der angezeigten Genauigkeit aufheben (langsames Ziehen, Shift), am Rand verwerfen
    const double target = m_value + m_accum;
    change(target);
    m_accum = m_value <= m_min || m_value >= m_max ? 0.0 : target - m_value;
}

void ScrubField::mouseReleaseEvent(QMouseEvent* e)
{
    if (m_editing || e->button() != Qt::LeftButton) return QLineEdit::mouseReleaseEvent(e);
    const bool dragged = m_dragging;
    m_pressed = m_dragging = false;
    if (dragged) {
        clearFocus();
        if (onFinish) onFinish();
    } else {
        startEditing(); // nur geklickt -> Zahl eintippen
    }
}

void ScrubField::startEditing()
{
    m_editing = true;
    setReadOnly(false);
    setCursor(Qt::IBeamCursor);
    setText(QString::number(m_value, 'f', m_decimals));
    setFocus();
    selectAll();
}

void ScrubField::commitText()
{
    if (!m_editing) return;
    m_editing = false;
    setReadOnly(true);
    setCursor(Qt::SizeHorCursor);
    bool ok = false;
    const double v = text().trimmed().replace(',', '.').toDouble(&ok); // Komma oder Punkt
    if (ok) change(v);
    showValue();
    if (ok && onFinish) onFinish();
}

void ScrubField::keyPressEvent(QKeyEvent* e)
{
    if (m_editing && (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)) {
        commitText();
        clearFocus();
        return;
    }
    if (m_editing && e->key() == Qt::Key_Escape) {
        m_editing = false;
        setReadOnly(true);
        setCursor(Qt::SizeHorCursor);
        showValue();
        clearFocus();
        return;
    }
    QLineEdit::keyPressEvent(e);
}

void ScrubField::focusOutEvent(QFocusEvent* e)
{
    commitText(); // wegklicken übernimmt wie in DaVinci
    QLineEdit::focusOutEvent(e);
}
