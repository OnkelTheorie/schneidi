#pragma once
// MLT-Profil aus den Projekteinstellungen (Vorschau, Export, Cache) und Format eines Clips erkennen
// (für "Projekt an den ersten Clip anpassen" wie DaVinci).

#include "core/ProjectFormat.h"

#include <QString>

#include <map>
#include <memory>

namespace Mlt {
class Producer;
class Profile;
}

// Quadratische Pixel, progressiv, BT.709
std::unique_ptr<Mlt::Profile> makeProfile(const ProjectFormat& format);

// Öffnet Mediendateien für Bilder. Umweg um einen MLT-Fehler (7.30, filter_swscale): skaliert MLT eine BT.601-Quelle
// im 709-Profil, rechnet es die Farben nach 709 um, lässt die Kennzeichnung aber auf 601 -> in jeder anderen als der
// Originalgröße (Vorschau, 720p/4K-Export, Vorschaubilder) falsche Farben. Der Skalierer nimmt die Zielnorm aus dem
// Profil des Producers, also bekommen solche Quellen ein sonst gleiches Profil mit ihrer eigenen Farbnorm.
// Die Profile gehören der Fabrik: sie muss alle mit ihr geöffneten Producer überleben.
class ProducerFactory {
public:
    explicit ProducerFactory(Mlt::Profile& base);
    ~ProducerFactory();
    std::unique_ptr<Mlt::Producer> open(const QString& resource);

private:
    Mlt::Profile& m_base;
    std::map<int, std::unique_ptr<Mlt::Profile>> m_profiles; // Farbnorm -> Profil
};

struct ClipFormat {
    bool ok = false;      // Video (kein Standbild/Ton) mit lesbarem Format
    int width = 0, height = 0; // wie angezeigt (Drehung aus den Metadaten berücksichtigt)
    FrameRate rate;       // Framerate laut Datei (bei variabler Framerate die höchste)
    double averageFps = 0; // mittlere Framerate (0 = unbekannt)
    bool variable = false; // variable Framerate (Handy)
    FrameRate suggested;  // nächstliegende Standard-Framerate fürs Projekt
};
ClipFormat detectClipFormat(const QString& path);
