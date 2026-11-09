// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/LazyFontBox.h"

#include <QComboBox>
#include <QFontComboBox>
#include <QHBoxLayout>
#include <QSignalBlocker>

LazyFontBox::LazyFontBox(QWidget* parent) : QWidget(parent)
{
    setFocusPolicy(Qt::ClickFocus);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
}

QFontComboBox* LazyFontBox::box()
{
    if (m_box) return m_box;
    m_box = new QFontComboBox(this);
    m_box->setFocusPolicy(Qt::ClickFocus);
    m_box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); // otherwise the list blows the width
    m_box->setMinimumContentsLength(8);
    m_box->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    if (m_fontSet) {
        const QSignalBlocker b(m_box);
        m_box->setCurrentFont(m_font);
    }
    connect(m_box, &QFontComboBox::currentFontChanged, this, [this](const QFont& f) {
        m_font = f;
        m_fontSet = true;
        emit currentFontChanged(f);
    });
    layout()->addWidget(m_box);
    updateGeometry();
    return m_box;
}

QFont LazyFontBox::currentFont() const
{
    if (m_box) return m_box->currentFont();
    return m_font;
}

void LazyFontBox::setCurrentFont(const QFont& font)
{
    m_font = font;
    m_fontSet = true;
    if (!m_box) return;
    const QSignalBlocker b(m_box); // a programmatic change is no user choice (the caller blocks this widget, too)
    m_box->setCurrentFont(font);
}

void LazyFontBox::showEvent(QShowEvent* e)
{
    box();
    QWidget::showEvent(e);
}

QSize LazyFontBox::sizeHint() const
{
    // Before the box exists: a plain combo box's height, so the layout does not jump when it comes
    if (m_box) return m_box->sizeHint();
    static const int height = QComboBox().sizeHint().height();
    return {120, height};
}

QSize LazyFontBox::minimumSizeHint() const
{
    return m_box ? m_box->minimumSizeHint() : QSize(40, sizeHint().height());
}
