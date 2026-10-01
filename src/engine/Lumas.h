#pragma once
// Verlaufsblenden (TransitionType::Luma): Graustufenbild, dunkle Stellen wechseln zuerst (wie MLT „luma“,
// Kdenlive/Shotcut/OpenShot). Mitgelieferte Verläufe werden nach id berechnet (":/lumas/<id>", Namen in
// core/EffectFolders), eigene Bilder aus dem Effekte-Ordner werden eingelesen. Für MLT landet jedes Bild als
// Graustufen-PNG in Projektgröße im Cache (auch Umkehren), so liest luma nie fremde Formate oder Pfade mit Umlauten.

#include <QImage>
#include <QString>

namespace Lumas {

// Verlaufsbild in Graustufen (w x h); leer = unbekannt/unlesbar (error gesetzt)
QImage image(const QString& path, int w, int h, bool invert = false, QString* error = nullptr);
// PNG für die MLT-Transition „luma“ (zwischengespeichert nach Pfad, Änderungszeit, Größe, Umkehren); leer = Fehler
QString file(const QString& path, int w, int h, bool invert);
// Vorschausymbol: Clip A (blau) wechselt halb zu B (orange)
QImage preview(const QImage& luma, double progress = 0.5);

} // namespace Lumas
