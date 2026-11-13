// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The size of the whole interface (Settings › General › Size): a factor on top of the system's scaling, applied as
// QT_SCALE_FACTOR before the QApplication exists (takes effect after a restart, like language and design). Nothing
// saved = the system's size. The factor scales everything (fonts, columns, icons), not only the font: at 4K with 100 %
// scaling the panels keep their pixel widths and a larger font alone would not fit them.
// The offer: on a screen with many pixels shown at 100 % (a 4K monitor without scaling) everything is tiny; the first
// time schneidi starts there it asks once in a bar in the window (MainWindow::offerUiScale) whether to make it larger.
// The answer is kept (kAskedKey), nothing changes without a yes.

#include <QList>
#include <QString>

class QScreen;

namespace UiScale {

inline constexpr const char* kSettingsKey = "ui/scale"; // factor, missing = the system's size
inline constexpr const char* kAskedKey = "ui/scaleAsked"; // the 4K offer was answered (yes or no)

// The factors offered in the settings (1 = 100 %)
const QList<double>& steps();
// Saved factor, 0 = none (the system's size)
double saved();
// 0 removes it (the system's size); takes effect at the next start
void setSaved(double factor);

// Before the QApplication: a saved factor other than 1 becomes QT_SCALE_FACTOR, unless the user started schneidi with
// one of their own. True = set by us; takeBackEnvironment() after the QApplication removes it again (a restart or a
// program schneidi starts must not inherit it as if the user had set it)
bool applyBeforeApp();
void takeBackEnvironment();
// The user's own QT_SCALE_FACTOR (started with it), empty = none: it wins over the saved size (Settings says so)
QString userFactor();

// The factor to offer for a screen: logical width >= 3200 px at a device pixel ratio of 1 (no scaling) and a physical
// density of a high-resolution monitor (>= 120 dpi; a 4K TV far away has less). The density over 96 dpi in 25 % steps,
// 1.25 to 2. 0 = no offer (scaled already, an ordinary screen, an unknown physical size)
double suggestion(int logicalWidth, double devicePixelRatio, double physicalDpi);
double suggestion(const QScreen* screen);
// The offer is due: not answered yet, nothing saved, no QT_SCALE_FACTOR of the user's
bool offerDue();
void setAsked();

} // namespace UiScale
