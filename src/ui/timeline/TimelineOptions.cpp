// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/timeline/TimelineOptions.h"

#include <QSettings>
#include <QString>

namespace TimelineOptions {

namespace {

// A choice stored by name; the default is not stored (a later change of the default reaches it)
void store(const char* key, const char* value, bool isDefault)
{
    if (isDefault) QSettings().remove(key);
    else QSettings().setValue(key, value);
}

QString stored(const char* key) { return QSettings().value(key).toString(); }

} // namespace

Follow follow()
{
    const QString v = stored("timeline/followPlayback");
    return v == "page" ? Follow::Page : v == "center" ? Follow::Center : Follow::Off;
}

void setFollow(Follow follow)
{
    store("timeline/followPlayback", follow == Follow::Page ? "page" : "center", follow == Follow::Off);
}

} // namespace TimelineOptions
