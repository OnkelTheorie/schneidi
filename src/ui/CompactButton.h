// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QToolButton>

// A tool button with text that never holds its panel wide: given less room than its text needs, the text ends in "…";
// with a short form (setCompactText, e.g. only the symbol of "▦ Media Pool" or "+" for "Import…") that one is shown
// once the elided text would keep only a few letters. Its minimum width is that of the short form, its size hint
// that of the full text; shortened, the tooltip starts with the full text (then the button's own tooltip). For
// text-only and text-beside-icon buttons (the usual tool buttons).
class CompactButton : public QToolButton {
public:
    // Horizontally it may shrink below its hint (a tool button is Fixed), never grow beyond it
    explicit CompactButton(QWidget* parent = nullptr) : QToolButton(parent)
    {
        setSizePolicy(QSizePolicy::Maximum, sizePolicy().verticalPolicy());
    }
    // The short form shown when the full text does not fit; empty = the full text elided
    void setCompactText(const QString& text);
    const QString& compactText() const { return m_compact; }
    // The text shown now (full, short form or elided): tests
    QString shownText() const;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;
    bool event(QEvent* e) override;

private:
    bool showsText() const;
    int textWidth(const QString& s) const;
    QString m_compact;
};
