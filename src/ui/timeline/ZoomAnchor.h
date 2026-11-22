// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

// Where wheel (and pinch) zoom on a timeline-like view (Edit timeline, Studio lanes) stays put: the user's choice in
// Settings › General › Timeline (QSettings "timeline/zoomAnchor": "mouse" or "playhead"). Read on every zoom, so a
// change takes effect at once, no restart.
//  - Mouse (default): the frame under the pointer (or the pinch centre) stays under it.
//  - Playhead: the playhead keeps its x on screen. When it is scrolled out of the view, the pointer is used instead,
//    so the zoom happens where the user looks rather than around something they cannot see.
// Keyboard and button zoom (zoomBy) do not use it: they have no pointer and always anchor at the visible playhead,
// else the middle of the view.
namespace ZoomAnchor {

enum class Mode { Mouse, Playhead };

Mode mode(); // the stored choice, Mouse when none is stored
void setMode(Mode mode);

// The x (widget coordinates) to keep still: pointerX, or with Mode::Playhead playheadX while it lies in [left, right]
inline double anchorX(Mode mode, double pointerX, double playheadX, double left, double right)
{
    if (mode == Mode::Playhead && playheadX >= left && playheadX <= right) return playheadX;
    return pointerX;
}
inline double anchorX(double pointerX, double playheadX, double left, double right)
{
    return anchorX(mode(), pointerX, playheadX, left, right);
}

} // namespace ZoomAnchor
