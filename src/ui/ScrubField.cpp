// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/ScrubField.h"

#include "app/Theme.h"
#include "core/I18n.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <algorithm>
#include <cmath>

ScrubField::ScrubField(double min, double max, double step, int decimals, QWidget* parent)
    : QLineEdit(parent), m_min(min), m_max(max), m_step(step), m_decimals(decimals)
{
    setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    fitWidth();
    setReadOnly(true);
    setCursor(Qt::SizeHorCursor);
    setFocusPolicy(Qt::ClickFocus);
    setStyleSheet(QString("QLineEdit { background: %1; border: 1px solid %1; border-radius: 2px; padding: 1px 4px; }"
                          "QLineEdit:focus { border-color: %2; }")
                      .arg(Theme::field.name(), Theme::primary.name()));
    showValue();
}

// The widest text the field shows: its smallest and largest value with sign, decimals and suffix ("-100,00 %"), or
// the text for the smallest value ("-∞"). A huge range (a counter up to 1e9) counts as four digits: the panel should
// not get wide for values hardly anyone types
int ScrubField::widestText() const
{
    const QFontMetrics fm = fontMetrics();
    const auto shown = [this, &fm](double v) {
        const QString n = QString::number(std::clamp(v, -9999.0, 9999.0), 'f', m_decimals);
        return fm.horizontalAdvance(QString(n).replace('.', I18n::decimalPoint()) + m_suffix);
    };
    int width = std::max(shown(m_min), shown(m_max));
    if (!m_minText.isEmpty()) width = std::max(width, fm.horizontalAdvance(m_minText));
    if (!m_zeroText.isEmpty()) width = std::max(width, fm.horizontalAdvance(m_zeroText));
    return width;
}

// Room for a value like "100,000" also with a large UI font (a fixed 64 px cut off the first digit there), and for
// the widest text this field shows. A shrinkable field gives down to a short value ("0,000") or the text it shows
// right now, whichever is wider ("-100,00 %", "-∞" are never cut): reserving the widest text of every field's range
// made the Inspector ~40 px wider (two position fields side by side) for values rarely shown
void ScrubField::fitWidth()
{
    const QFontMetrics fm = fontMetrics();
    const int padding = 16; // padding and border of the style sheet, a little air
    const int usual = std::max(64, fm.horizontalAdvance(QStringLiteral("100,000")) + padding);
    if (!m_shrinkable) {
        setFixedWidth(std::max(usual, widestText() + padding));
        return;
    }
    const int shown = std::max(fm.horizontalAdvance(QStringLiteral("0,000")), fm.horizontalAdvance(text())) + padding;
    setMinimumWidth(shown);
    setMaximumWidth(std::max(usual, shown));
    setSizePolicy(QSizePolicy::Preferred, sizePolicy().verticalPolicy());
}

void ScrubField::setShrinkable(bool on)
{
    m_shrinkable = on;
    fitWidth();
}

void ScrubField::changeEvent(QEvent* e)
{
    QLineEdit::changeEvent(e);
    if (e->type() == QEvent::FontChange || e->type() == QEvent::StyleChange) fitWidth();
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
    fitWidth();
    setValue(m_value);
}

void ScrubField::setSuffix(const QString& s)
{
    m_suffix = s;
    fitWidth();
    showValue();
}

void ScrubField::setMinimumText(const QString& t)
{
    m_minText = t;
    fitWidth();
    showValue();
}

void ScrubField::setZeroText(const QString& t)
{
    m_zeroText = t;
    fitWidth();
    showValue();
}

void ScrubField::showValue()
{
    if (!m_minText.isEmpty() && m_value <= m_min) setText(m_minText);
    else if (!m_zeroText.isEmpty() && m_value == 0) setText(m_zeroText);
    else setText(number() + m_suffix);
    if (m_shrinkable) fitWidth(); // never narrower than the text shown now
}

// Decimal separator of the UI language, like the other number fields
QString ScrubField::number() const
{
    return QString::number(m_value, 'f', m_decimals).replace('.', I18n::decimalPoint());
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
    // Keep the remainder below the displayed precision (slow dragging, Shift), discard at the edge
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
        startEditing(); // just clicked -> type in the number
    }
}

void ScrubField::startEditing()
{
    m_editing = true;
    setReadOnly(false);
    setCursor(Qt::IBeamCursor);
    setText(number());
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
    const double v = text().trimmed().replace(',', '.').toDouble(&ok); // comma or dot
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
    commitText(); // clicking away commits, like in DaVinci
    QLineEdit::focusOutEvent(e);
}
