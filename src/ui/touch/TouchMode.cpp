// SPDX-License-Identifier: AGPL-3.0-only
// Touch mode (see Touch.h): the setting, auto detection from the input devices, the flag for this run.
#include "ui/touch/Touch.h"

#include <QInputDevice>
#include <QSettings>

namespace Touch {

namespace {
bool g_active = false;
}

Mode savedMode()
{
    const QString v = QSettings().value(kModeKey).toString();
    return v == "on" ? Mode::On : v == "off" ? Mode::Off : Mode::Auto;
}

void setSavedMode(Mode mode)
{
    QSettings s;
    if (mode == Mode::Auto) s.remove(kModeKey);
    else s.setValue(kModeKey, mode == Mode::On ? "on" : "off");
}

bool autoOn(const QList<int>& deviceTypes)
{
    using T = QInputDevice::DeviceType;
    const bool touchscreen = deviceTypes.contains(int(T::TouchScreen));
    const bool pointer = deviceTypes.contains(int(T::Mouse)) || deviceTypes.contains(int(T::TouchPad));
    return touchscreen && !pointer;
}

bool decideMode()
{
    switch (savedMode()) {
    case Mode::On: return true;
    case Mode::Off: return false;
    case Mode::Auto: break;
    }
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
    return true; // a phone or tablet (its platform may list a generic mouse device as well)
#endif
    // No input device at all (the offscreen platform: --screenshot, tests): off. Also the way around a wrong
    // optimisation: GCC 14.2 at -O3 passed an empty list with its size never written to autoOn (the Release build
    // of the .deb crashed here at start without a screen)
    const QList<const QInputDevice*> devices = QInputDevice::devices();
    if (devices.isEmpty()) return false;
    QList<int> types;
    types.reserve(devices.size());
    for (const QInputDevice* d : devices) types << int(d->type());
    return autoOn(types);
}

void initMode() { g_active = decideMode(); }
bool modeActive() { return g_active; }
void setModeActive(bool on) { g_active = on; }

} // namespace Touch
