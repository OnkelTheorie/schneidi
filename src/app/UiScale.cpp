// SPDX-License-Identifier: AGPL-3.0-only
#include "app/UiScale.h"

#include <QScreen>
#include <QSettings>
#include <QString>
#include <algorithm>
#include <cmath>

namespace UiScale {

namespace {
constexpr const char* kEnv = "QT_SCALE_FACTOR";
bool g_setByUs = false;
} // namespace

const QList<double>& steps()
{
    static const QList<double> s{1.0, 1.25, 1.5, 1.75, 2.0};
    return s;
}

double saved()
{
    const double f = QSettings().value(kSettingsKey, 0.0).toDouble();
    return f >= 0.5 && f <= 4.0 ? f : 0.0;
}

void setSaved(double factor)
{
    if (factor <= 0) QSettings().remove(kSettingsKey);
    else QSettings().setValue(kSettingsKey, factor);
}

bool applyBeforeApp()
{
    if (!qEnvironmentVariableIsEmpty(kEnv)) return false; // the user's own: theirs wins
    const double f = saved();
    if (f <= 0 || std::abs(f - 1.0) < 0.01) return false;
    qputenv(kEnv, QByteArray::number(f));
    g_setByUs = true;
    return true;
}

void takeBackEnvironment()
{
    if (!g_setByUs) return;
    qunsetenv(kEnv); // Qt read it in the QApplication's constructor
    g_setByUs = false;
}

double suggestion(int logicalWidth, double devicePixelRatio, double physicalDpi)
{
    if (logicalWidth < 3200 || devicePixelRatio > 1.05 || !std::isfinite(physicalDpi) || physicalDpi < 120) return 0;
    return std::clamp(std::round(physicalDpi / 96.0 * 4.0) / 4.0, 1.25, 2.0);
}

double suggestion(const QScreen* screen)
{
    if (!screen) return 0;
    return suggestion(screen->geometry().width(), screen->devicePixelRatio(), screen->physicalDotsPerInch());
}

bool offerDue()
{
    const QSettings s;
    return !s.value(kAskedKey, false).toBool() && !s.contains(kSettingsKey) && (g_setByUs || qEnvironmentVariableIsEmpty(kEnv));
}

void setAsked()
{
    QSettings().setValue(kAskedKey, true);
}

} // namespace UiScale
