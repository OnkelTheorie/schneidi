#pragma once
// Project settings like DaVinci (Project Settings -> Master Settings): timeline resolution and frame rate.
// All frame counts in the model count in this frame rate; pixel values (transform, titles ...) refer
// to this resolution.

#include "core/I18n.h"

#include <QSize>
#include <QString>
#include <algorithm>
#include <cmath>

struct Timeline;

struct FrameRate {
    int num = 25, den = 1;

    double fps() const { return double(num) / den; }
    // Whole frames per timecode second (29.97 -> 30, non-drop-frame like DaVinci without drop frame);
    // also applies to default lengths like "1 s" (transitions) and "5 s" (titles, stills)
    int timebase() const { return std::max(1, int(std::lround(fps()))); }
    bool isInteger() const { return den == 1; }
    QString label() const; // "25", "29.97" (deutsch "29,97")
    bool operator==(const FrameRate& o) const { return qint64(num) * o.den == qint64(o.num) * den; }
    bool operator!=(const FrameRate& o) const { return !(*this == o); }
};

// Selection as in DaVinci ("Timeline frame rate")
inline constexpr FrameRate kFrameRates[] = {
    {24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {50, 1}, {60000, 1001}, {60, 1},
};

// Nearest standard frame rate; integerOnly = only 24/25/30/50/60 (phone clips with variable frame rate
// are nominally 30 or 60, their mean is often slightly below)
FrameRate nearestFrameRate(double fps, bool integerOnly = false);

struct ProjectFormat {
    int width = 1920, height = 1080;
    FrameRate rate;

    QSize size() const { return {width, height}; }
    bool operator==(const ProjectFormat& o) const { return width == o.width && height == o.height && rate == o.rate; }
    bool operator!=(const ProjectFormat& o) const { return !(*this == o); }
};

// What a project's format may be (settings dialog, `new`/`format` op): a format from a file (edited by hand, another
// program's timeline) is brought inside - sizes clamped, a frame rate below 1 or above 1000 fps becomes the nearest
// standard one (other rates, e.g. 48 fps from an imported timeline, stay)
inline constexpr int kMinFrameSize = 16, kMaxFrameSize = 8192;
ProjectFormat withinLimits(ProjectFormat f);

// Presets for "Timeline resolution" (order = display order); name is translated with T()
struct ResolutionPreset { int width, height; const char* name; };
inline constexpr ResolutionPreset kResolutionPresets[] = {
    {1280, 720, "HD"},
    {1920, 1080, "HD"},
    {2560, 1440, "Quad HD"},
    {3840, 2160, "Ultra HD"},
    {1080, 1920, N_("Hochformat HD")},
    {2160, 3840, N_("Hochformat Ultra HD")},
    {1080, 1080, N_("Quadrat")},
};

// "1920 x 1080 HD" or "1000 x 800" for custom sizes
QString resolutionLabel(int width, int height);

// Resolution changed: convert pixel values in the model (position, crop, title, wipe border) so that
// everything stays at the same place and the same size relative to the picture. Positions/crop separately
// by width and height, sizes (font, outline, border) with the smaller factor (text still fits in the picture).
void scaleTimeline(Timeline& tl, QSize from, QSize to);
