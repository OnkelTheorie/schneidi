// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Shape of a clip's picture for core code. Crop values are timeline pixels on the picture as it fits into the frame
// at zoom 1 (ClipTransform), so converting them (older project files, a new timeline resolution) needs the aspect
// ratio of the picture. Only the engine can open files: it installs the probe once MLT is up (Engine::initMlt; app
// and schneidi-cli). Without a probe, or for a file it cannot read, the aspect is unknown (0).

#include "core/Types.h"

#include <QSizeF>
#include <QString>

#include <functional>

namespace MediaAspect {

// Width / height of a media file's picture as displayed (pixel aspect and rotation included), 0 = unknown
using Probe = std::function<double(const QString& path)>;
void setProbe(Probe probe);
// Cached per file (path, size, modification time): each file is opened once per process
double of(const QString& path);

// A picture of `aspect` fitted into `frame` keeping its shape, as the engine places it at zoom 1 (a portrait clip in
// a landscape frame gets room at the sides); aspect <= 0 = it fills the frame (titles, compound clips)
QSizeF fit(double aspect, QSizeF frame);

} // namespace MediaAspect
