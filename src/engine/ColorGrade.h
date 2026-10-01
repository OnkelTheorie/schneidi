#pragma once
// Farbkorrektur der Color-Seite (wie DaVinci „Primaries – Color Wheels“ + LUT), Effekt "grade" am Clip.
// Rechnung selbst (kein avfilter): Lift darf negativ werden, LUT-Pfade brauchen kein FFmpeg-Escaping (Windows
// „C:\…“), Keyframes/Vorher-Nachher ohne Neuaufbau. Die Pixelrechnung ist eine reine Funktion (testbar ohne MLT).

#include "core/Types.h"

#include <QString>
#include <QVector>
#include <atomic>
#include <cstdint>
#include <memory>

namespace Mlt {
class Producer;
}

namespace ColorGrade {

inline constexpr const char* EffectId = "grade";

// Werte wie in DaVinci; Räder: [0] = Master (Y), [1..3] = R/G/B
struct Params {
    double lift[4] = {0, 0, 0, 0};
    double gamma[4] = {0, 0, 0, 0};
    double gain[4] = {1, 1, 1, 1};
    double offset[4] = {25, 25, 25, 25};
    double contrast = 1.0, pivot = 0.435;
    double saturation = 50;  // 0 = grau, 50 = unverändert, 100 = doppelt
    double temperature = 0;  // -4000..4000, + = wärmer
    double tint = 0;         // -100..100, + = Magenta
    double exposure = 0;     // Blendenstufen
    bool isNeutral() const;
};

// Animierbare Parameter in der Reihenfolge von Params (Räder je Y/R/G/B, dann die Regler)
const QVector<AnimParam>& animParams();

// Werte am Clip-Frame t (Keyframes interpoliert); Clip ohne "grade" = neutral
Params at(const Clip& c, double t);
// Hat der Clip eine eingeschaltete Korrektur, die etwas tut (Werte, Keyframes oder LUT)?
bool active(const Clip& c);
QString lutPath(const Clip& c); // leer = keine LUT

// 3D-LUT bzw. 1D-LUT; Werte R schnellster Index
struct Lut {
    int size = 0;
    bool is3d = true;
    float domainMin[3] = {0, 0, 0}, domainMax[3] = {1, 1, 1};
    QVector<float> data; // size^3 * 3 (3D) bzw. size * 3 (1D)
    // Vorab-Kurve je Kanal (.csp „preLUT“): Stützstellen Eingang -> Ausgang 0..1, leer = keine
    QVector<float> shaperIn[3], shaperOut[3];
};
// Formate: .cube (Adobe/Resolve), .3dl (Lustre/Flame/Nuke), .csp (cineSpace), Hald-CLUT als Bild (.png/.tif/.bmp),
// Endungen siehe EffectFolders::lutSuffixes.
// Liest eine LUT-Datei (Format nach Endung, auch Qt-Ressourcen ":/…") ohne Zwischenspeicher; nullptr + error bei Fehlern
std::shared_ptr<const Lut> parseLut(const QString& path, QString* error = nullptr);
// Wie parseLut, aber zwischengespeichert nach Pfad + Änderungszeit (Vorschau/Export)
std::shared_ptr<const Lut> loadLut(const QString& path, QString* error = nullptr);

// Korrektur auf ein RGBA-Bild (8 Bit, Zeilen ohne Lücke) anwenden; Alpha bleibt. lut darf nullptr sein.
void apply(uint8_t* rgba, int width, int height, const Params& p, const Lut* lut);

// Filter an einen Ausschnitt hängen (a = Clip-Frame am Anfang, für Keyframes). bypass: Vorschau-Schalter
// „Vorher/Nachher“ (true = ohne Korrektur), nullptr = immer anwenden (Export). Nichts zu tun -> kein Filter.
void attach(Mlt::Producer& cut, const Clip& c, int a, const std::shared_ptr<std::atomic<bool>>& bypass);

} // namespace ColorGrade
