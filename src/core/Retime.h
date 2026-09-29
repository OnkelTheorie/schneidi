#pragma once

#include "core/Types.h"

// Zeitverlauf eines Clips (Geschwindigkeit, Rückwärts, Speed Ramp): Material-Frame m (darin zählen Clip::in/out und
// die Keyframes) <-> Quell-Position s in Datei-Frames, gezählt in Abspielrichtung (rückwärts: s = 0 am Dateiende).
// Harte Speed-Punkte: s(m) stückweise linear. Weicher Übergang: das Tempo läuft über `smooth` Frames linear vom alten
// zum neuen Wert, mittig um den harten Wechsel -> außerhalb des Übergangs bleibt die Zuordnung gleich.
class RetimeMap {
public:
    // fileLength <= 0: unbekannt (für Material-Länge unbegrenzt, rückwärts nicht spiegelbar)
    RetimeMap(const Clip& c, int fileLength);

    // Abschnitt: s(m) = s0 + v0·x + (v1 − v0)·x² / (2·(m1 − m0)), x = m − m0 (v0 == v1: konstantes Tempo)
    struct Piece {
        double m0, m1, s0, v0, v1;
        double sourceAt(double m) const;
        double speedAt(double m) const;
        double end() const { return sourceAt(m1); }
    };

    double sourceAt(double m) const;
    double materialAt(double s) const;
    double speedAt(double m) const; // Tempo an Material-Frame m (Betrag, ohne Richtung)
    // Datei-Frame (mit Richtung) <-> Material
    double fileFrameAt(double m) const;
    double materialAtFile(double f) const;
    // Material-Länge bis zum Ende der Datei (0 = unbegrenzt/unbekannt)
    int length() const;
    // Harte Wechselstellen der Speed-Punkte in Material-Frames (Index wie Clip::ramp)
    QVector<double> pointMaterial() const { return m_points; }
    const QVector<Piece>& pieces() const { return m_pieces; }

private:
    const Piece& pieceAtMaterial(double m) const;
    QVector<Piece> m_pieces; // lückenlos ab m = 0, letzter reicht unbegrenzt weit
    QVector<double> m_points;
    int m_fileLength = 0;
    bool m_reverse = false;
};

namespace Retime {
// Punkte aufräumen: sortiert, innerhalb (0, fileLength), Abstand >= 1 Frame, Tempo > 0, Übergang >= 0
void normalize(QVector<SpeedPoint>& ramp, int fileLength);
// Tempo-Bereich wie DaVinci (0 % = Standbild ist kein Abschnittstempo)
inline constexpr double kMinSpeed = 0.01, kMaxSpeed = 100.0;
// Tempo des Abschnitts i (0 = vor dem ersten Punkt)
double segmentSpeed(const Clip& c, int segment);
void setSegmentSpeed(Clip& c, int segment, double speed);
} // namespace Retime
