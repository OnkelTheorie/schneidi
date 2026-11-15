// SPDX-License-Identifier: AGPL-3.0-only
// File › Settings never cuts a page off at the right: every tab (General, Start templates, AI with its chat and Claude
// sections) fits the dialog's width, also in a long language (Russian) with a large UI font (QT_FONT_DPI=144 or a large
// system font). Before, the AI page in Russian at a large font was wider than its scroll area: hints, the "load models"
// button and the key row's button ran out of the dialog with only a vertical scrollbar. Long labels now put their field
// below them, choice boxes give way, hints wrap. The dialog keeps its size: 640 px at the normal font, scaled with a
// reference text for another font (like test_inspector_layout): it may grow up to that, never cut off.
// SETTINGS_LAYOUT_DUMP=<folder>: a picture per failing case.

#include "check.h"

#include "app/AiKeys.h"
#include "app/AiSettings.h"
#include "app/AiSettingsPage.h"
#include "app/SettingsDialog.h"
#include "app/Theme.h"
#include "core/I18n.h"

#include <QApplication>
#include <QFontMetricsF>
#include <QJsonObject>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabWidget>
#include <QTest>

#include <cmath>

namespace {

void settle()
{
    for (int i = 0; i < 3; ++i) {
        QTest::qWait(20);
        QApplication::processEvents();
    }
}

// The widest the dialog may get for the font set now: 640 px with Noto Sans 9 pt, scaled by the width of a reference text in the
// language's script (a larger or wider font gets a wider dialog, as a user would make it)
int widthFor(const QString& lang)
{
    const char* latin = "abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    const char* cyrillic = "абвгдежзийклмнопрстуфхцчшщъыьэюя АБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ";
    const double noto = lang == "ru" ? 515.4 : 366.1;
    const double now = QFontMetricsF(QApplication::font()).horizontalAdvance(QString::fromUtf8(lang == "ru" ? cyrillic : latin));
    // Only grows with the font (a large font), never below the dialog's own 640 px
    return std::max(640, int(std::ceil(640 * now / noto)));
}

void checkDialog(const QString& font, const QString& lang)
{
    Ai::Keys keys(nullptr, Ai::settingsKeyStore());
    SettingsDialog dlg(AiSettings{});
    dlg.aiPage()->setClaudeSetup([](bool) { return QJsonObject{}; });
    dlg.aiPage()->addChatSection(&keys);
    const int width = widthFor(lang);
    dlg.resize(640, 680); // its own size; it grows only as far as its pages need (at most `width`)
    dlg.show();
    settle();
    auto* tabs = dlg.findChild<QTabWidget*>();
    if (!CHECK(tabs)) return;
    for (int i = 0; i < tabs->count(); ++i) {
        tabs->setCurrentIndex(i);
        settle();
        auto* scroll = qobject_cast<QScrollArea*>(tabs->currentWidget());
        if (!CHECK(scroll)) continue;
        const QString where = QString("%1 font, %2, %3 px, tab %4").arg(font, lang).arg(width).arg(tabs->tabText(i));
        const int page = scroll->widget()->width(), viewport = scroll->viewport()->width();
        // Nothing reaches past the visible part: the page and every widget on it
        int right = page;
        QString widest;
        for (QWidget* w : scroll->widget()->findChildren<QWidget*>()) {
            if (!w->isVisibleTo(scroll->widget())) continue;
            const int r = w->mapTo(scroll->widget(), QPoint(w->width(), 0)).x();
            if (r > right) right = r, widest = w->metaObject()->className() + QString(" ") + w->objectName();
        }
        std::printf("%s: page %d px, widest right edge %d px, viewport %d px\n", qPrintable(where), page, right, viewport);
        const bool cut = page > viewport || right > viewport;
        if (cut) {
            const QString dir = qEnvironmentVariable("SETTINGS_LAYOUT_DUMP");
            if (!dir.isEmpty()) dlg.grab().save(QString("%1/%2-%3-%4.png").arg(dir, font, lang).arg(i));
        }
        Check::report(!scroll->horizontalScrollBar()->isVisible(), "no horizontal scrollbar", __FILE__, __LINE__, where);
        Check::report(!cut, "page fits the dialog", __FILE__, __LINE__,
                      QString("%1: page %2 px, right edge %3 px (%4), viewport %5 px")
                          .arg(where)
                          .arg(page)
                          .arg(right)
                          .arg(widest)
                          .arg(viewport));
        Check::report(dlg.width() <= width + 2, "dialog not much wider", __FILE__, __LINE__,
                      QString("%1: %2 px").arg(where).arg(dlg.width()));
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("settings-layout");
    Theme::apply(app);
    const QFont normal = QApplication::font();
    QFont big = normal;
    big.setPointSizeF(normal.pointSizeF() * 1.5); // like QT_FONT_DPI=144 or a large system font
    for (const char* lang : {"de", "ru"}) {
        I18n::install(lang);
        QApplication::setFont(normal);
        checkDialog("normal", lang);
        QApplication::setFont(big);
        checkDialog("large", lang);
    }
    QApplication::setFont(normal);
    return Check::result();
}
