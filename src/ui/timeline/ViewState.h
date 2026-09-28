#pragma once
#include <algorithm>

// KERNREGEL: Zoom und Scroll der Timeline sind ein eigener Zustand.
// Nur Nutzereingaben (Mausrad, Tasten, Scrollbar) ändern ihn – Änderungen am
// Projekt (Clip kürzen, löschen, …) fassen ihn nie an. Deshalb springt nichts.
struct ViewState {
    static constexpr double kMinPxPerFrame = 0.002; // ~ Stunden auf dem Bildschirm
    static constexpr double kMaxPxPerFrame = 60.0;  // einzelne Frames breit

    double pxPerFrame = 4.0; // Zoom
    double leftFrame = 0.0;  // Frame am linken Rand (Scroll X), nie < 0
    int scrollY = 0;         // Pixel (Scroll Y durch die Spuren)
    int videoTrackHeight = 64;
    int audioTrackHeight = 52;

    void setZoomAround(double newPxPerFrame, double anchorFrame, double anchorPx)
    {
        pxPerFrame = std::clamp(newPxPerFrame, kMinPxPerFrame, kMaxPxPerFrame);
        leftFrame = std::max(0.0, anchorFrame - anchorPx / pxPerFrame);
    }
    void scrollFrames(double frames) { leftFrame = std::max(0.0, leftFrame + frames); }
};
