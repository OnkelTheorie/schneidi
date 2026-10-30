// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The format of a clip (for "adapt project to the first clip")

#include "core/ProjectFormat.h"

#include <QString>

struct ClipFormat {
    bool ok = false;      // video (no still image/audio) with a readable format
    int width = 0, height = 0; // as displayed (rotation from the metadata taken into account)
    FrameRate rate;       // frame rate according to the file (the highest with variable frame rate)
    double averageFps = 0; // average frame rate (0 = unknown)
    bool variable = false; // variable frame rate (phone)
    FrameRate suggested;  // nearest standard frame rate for the project
};
ClipFormat detectClipFormat(const QString& path);
