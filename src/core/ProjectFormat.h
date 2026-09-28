#pragma once
// Projekteinstellungen wie DaVinci (Project Settings → Master Settings): Timeline-Auflösung und -Framerate.
// Alle Frame-Angaben im Modell zählen in dieser Framerate; Pixelwerte (Transform, Titel …) beziehen sich
// auf diese Auflösung.

#include "core/I18n.h"

#include <QSize>
#include <QString>
#include <algorithm>
#include <cmath>

struct Timeline;

struct FrameRate {
    int num = 25, den = 1;

    double fps() const { return double(num) / den; }
    // Ganze Frames pro Timecode-Sekunde (29,97 -> 30, Non-Drop-Frame wie DaVinci ohne Drop Frame);
    // gilt auch für Standardlängen wie "1 s" (Übergänge) und "5 s" (Titel, Standbilder)
    int timebase() const { return std::max(1, int(std::lround(fps()))); }
    bool isInteger() const { return den == 1; }
    QString label() const; // "25", "29.97" (deutsch "29,97")
    bool operator==(const FrameRate& o) const { return qint64(num) * o.den == qint64(o.num) * den; }
    bool operator!=(const FrameRate& o) const { return !(*this == o); }
};

// Auswahl wie in DaVinci ("Timeline frame rate")
inline constexpr FrameRate kFrameRates[] = {
    {24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {50, 1}, {60000, 1001}, {60, 1},
};

// Nächstliegende Standard-Framerate; integerOnly = nur 24/25/30/50/60 (Handy-Clips mit variabler Framerate
// sind nominell 30 oder 60, ihr Mittelwert liegt oft knapp darunter)
FrameRate nearestFrameRate(double fps, bool integerOnly = false);

struct ProjectFormat {
    int width = 1920, height = 1080;
    FrameRate rate;

    QSize size() const { return {width, height}; }
    bool operator==(const ProjectFormat& o) const { return width == o.width && height == o.height && rate == o.rate; }
    bool operator!=(const ProjectFormat& o) const { return !(*this == o); }
};

// Vorlagen für "Timeline resolution" (Reihenfolge = Anzeige); name wird mit T() übersetzt
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

// "1920 × 1080 HD" bzw. "1000 × 800" für eigene Größen
QString resolutionLabel(int width, int height);

// Auflösung geändert: Pixelwerte im Modell (Position, Beschneiden, Titel, Wischblenden-Rand) so umrechnen,
// dass alles relativ zum Bild an derselben Stelle und gleich groß bleibt. Positionen/Beschneiden getrennt
// nach Breite und Höhe, Größen (Schrift, Umrandung, Rand) mit dem kleineren Faktor (Text passt weiter ins Bild).
void scaleTimeline(Timeline& tl, QSize from, QSize to);
