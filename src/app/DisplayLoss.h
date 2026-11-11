// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The display connection breaks under the running app (X server gone: logging out, a killed Xvfb; Wayland compositor
// died): Qt says so ("The X11 connection broke …") and ends the process with exit(1) from inside the event loop.
// exit() runs the static destructors (ours, Qt's, the GPU driver's) while playback, render and decoder threads still
// work - they then crashed in what was already destroyed (FileStamp's store in the renderer, the CUDA context under a
// decoder). Without a display nothing can be shown or asked any more: install() makes such an end immediate
// (std::_Exit(1) right after the message is logged), with the same exit code and no static destructors.

#include <QString>

namespace DisplayLoss {

// Is this Qt message the platform plugin's "the display connection is gone, I end the process"?
bool isLossMessage(const QString& message);
// After Log::install (the message is still logged); the GUI program only
void install();

} // namespace DisplayLoss
