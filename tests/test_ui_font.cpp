// SPDX-License-Identifier: AGPL-3.0-only
// UI font for Hindi (Devanagari), which the main UI fonts lack:
// - the letters come from a font made to sit next to the UI font (Noto Sans Devanagari on Linux), not from the first
//   font the system finds with the script - on Debian that was FreeSans, whose Devanagari is small and cramped, and
//   the Hindi UI looked a size smaller than the English one;
// - with Hindi as the UI language the font is a little larger: Devanagari letters carry their vowel signs above and
//   below the line, at the same point size their body is smaller than Latin text and hard to read at 9 pt.
// Skipped when Noto Sans Devanagari is not installed (the Linux font this checks for).
// UI_FONT_DUMP=<folder>: a Latin and Hindi line at the UI size as a picture.

#include "check.h"

#include "app/Theme.h"
#include "core/I18n.h"

#include <QApplication>
#include <QFontDatabase>
#include <QGlyphRun>
#include <QImage>
#include <QPainter>
#include <QTextLayout>

namespace {

// The families that draw the text (after Qt's fallback)
QStringList usedFamilies(const QFont& font, const QString& text)
{
    QTextLayout layout(text, font);
    layout.beginLayout();
    layout.createLine();
    layout.endLayout();
    QStringList families;
    for (const QGlyphRun& run : layout.glyphRuns()) families << run.rawFont().familyName();
    return families;
}

void dump(const QFont& font, const QString& name)
{
    const QString dir = qEnvironmentVariable("UI_FONT_DUMP");
    if (dir.isEmpty()) return;
    QImage img(500, 30, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    p.setFont(font);
    p.setPen(Qt::black);
    p.drawText(QPoint(6, 20), QString::fromUtf8("Opacity Perspective — कोई क्लिप चुनी नहीं, अपारदर्शिता"));
    p.end();
    img.save(dir + "/" + name + ".png");
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("ui-font");
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    std::printf("checks the Linux font - skipped\n");
    return 77;
#endif
    if (!QFontDatabase::families().contains("Noto Sans Devanagari")) {
        std::printf("Noto Sans Devanagari not installed - skipped\n");
        return 77;
    }
    const QString word = QString::fromUtf8("अपारदर्शिता");

    I18n::install("en");
    Theme::apply(app);
    const QFont english = QApplication::font();
    dump(english, "en");
    CHECK_EQ(usedFamilies(english, word).join(", "), QString("Noto Sans Devanagari"));
    CHECK_EQ(english.pointSizeF(), 9.0);

    I18n::install("hi");
    Theme::apply(app);
    const QFont hindi = QApplication::font();
    dump(hindi, "hi");
    CHECK_EQ(usedFamilies(hindi, word).join(", "), QString("Noto Sans Devanagari"));
    CHECK(hindi.pointSizeF() >= 9.9);
    // Latin text stays with the UI font
    CHECK_EQ(usedFamilies(hindi, "Opacity").join(", "), usedFamilies(english, "Opacity").join(", "));
    return Check::result();
}
