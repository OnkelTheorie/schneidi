// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// A font family chooser that builds its QFontComboBox on the first showing. QFontComboBox fills itself with every
// family of the system and loads its icons through the image plugins (on KDE dozens of them) right in its
// constructor: two of them in the Inspector cost the start more than 100 ms, for pages most sessions never open.
// The same API as far as the Inspector uses it; until it shows, the font is only remembered.

#include <QFont>
#include <QWidget>

class QFontComboBox;

class LazyFontBox : public QWidget {
    Q_OBJECT
public:
    explicit LazyFontBox(QWidget* parent = nullptr);

    QFont currentFont() const;
    void setCurrentFont(const QFont& font);
    // The real box (made now if it is not there yet): tests, tours
    QFontComboBox* box();
    bool built() const { return m_box != nullptr; }

signals:
    void currentFontChanged(const QFont& font);

protected:
    void showEvent(QShowEvent* e) override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

private:
    QFontComboBox* m_box = nullptr;
    QFont m_font;
    bool m_fontSet = false;
};
