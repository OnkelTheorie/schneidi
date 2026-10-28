// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <algorithm>

// CORE RULE: zoom and scroll of the timeline are their own state.
// Only user input (mouse wheel, keys, scrollbar) changes it – changes to the
// project (shortening a clip, deleting, …) never touch it. That is why nothing jumps.
struct ViewState {
    static constexpr double kMinPxPerFrame = 0.002; // ~ hours on screen
    static constexpr double kMaxPxPerFrame = 60.0;  // individual frames wide

    double pxPerFrame = 4.0; // Zoom
    double leftFrame = 0.0;  // frame at the left edge (scroll X), never < 0
    int scrollY = 0;         // pixels (scroll Y through the tracks)
    int videoTrackHeight = 64;
    int audioTrackHeight = 52;

    void setZoomAround(double newPxPerFrame, double anchorFrame, double anchorPx)
    {
        pxPerFrame = std::clamp(newPxPerFrame, kMinPxPerFrame, kMaxPxPerFrame);
        leftFrame = std::max(0.0, anchorFrame - anchorPx / pxPerFrame);
    }
    void scrollFrames(double frames) { leftFrame = std::max(0.0, leftFrame + frames); }
};
