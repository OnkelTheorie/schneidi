// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/timeline/ZoomAnchor.h"

#include <QSettings>

namespace ZoomAnchor {

namespace {
constexpr const char* kKey = "timeline/zoomAnchor";
}

Mode mode() { return QSettings().value(kKey).toString() == "playhead" ? Mode::Playhead : Mode::Mouse; }

void setMode(Mode mode) { QSettings().setValue(kKey, mode == Mode::Playhead ? "playhead" : "mouse"); }

} // namespace ZoomAnchor
