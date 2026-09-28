#pragma once
// MLT-Profil aus den Projekteinstellungen (Vorschau, Export, Cache) und Format eines Clips erkennen
// (für "Projekt an den ersten Clip anpassen" wie DaVinci).

#include "core/ProjectFormat.h"

#include <memory>

namespace Mlt {
class Profile;
}

// Quadratische Pixel, progressiv, BT.709
std::unique_ptr<Mlt::Profile> makeProfile(const ProjectFormat& format);

struct ClipFormat {
    bool ok = false;      // Video (kein Standbild/Ton) mit lesbarem Format
    int width = 0, height = 0; // wie angezeigt (Drehung aus den Metadaten berücksichtigt)
    FrameRate rate;       // Framerate laut Datei (bei variabler Framerate die höchste)
    double averageFps = 0; // mittlere Framerate (0 = unbekannt)
    bool variable = false; // variable Framerate (Handy)
    FrameRate suggested;  // nächstliegende Standard-Framerate fürs Projekt
};
ClipFormat detectClipFormat(const QString& path);
