#pragma once

#include "core/Types.h"

// Time course of a clip (speed, reverse, speed ramp): material frame m (clip in/out and
// the keyframes count in it) <-> source position s in file frames, counted in playback direction (reversed: s = 0 at the file end).
// Hard speed points: s(m) piecewise linear. Soft transition: the speed runs linearly over `smooth` frames from the old
// to the new value, centered around the hard change -> outside the transition the mapping stays the same.
class RetimeMap {
public:
    // fileLength <= 0: unknown (unlimited for material length, reversed cannot be mirrored)
    RetimeMap(const Clip& c, int fileLength);

    // Section: s(m) = s0 + v0*x + (v1 - v0)*x^2 / (2*(m1 - m0)), x = m - m0 (v0 == v1: constant speed)
    struct Piece {
        double m0, m1, s0, v0, v1;
        double sourceAt(double m) const;
        double speedAt(double m) const;
        double end() const { return sourceAt(m1); }
    };

    double sourceAt(double m) const;
    double materialAt(double s) const;
    double speedAt(double m) const; // speed at material frame m (magnitude, without direction)
    // File frame (with direction) <-> material
    double fileFrameAt(double m) const;
    double materialAtFile(double f) const;
    // Material length up to the end of the file (0 = unlimited/unknown)
    int length() const;
    // Hard change points of the speed points in material frames (index like Clip::ramp)
    QVector<double> pointMaterial() const { return m_points; }
    const QVector<Piece>& pieces() const { return m_pieces; }

private:
    const Piece& pieceAtMaterial(double m) const;
    QVector<Piece> m_pieces; // gapless from m = 0, the last one extends indefinitely
    QVector<double> m_points;
    int m_fileLength = 0;
    bool m_reverse = false;
};

namespace Retime {
// Tidy up points: sorted, within (0, fileLength), distance >= 1 frame, speed > 0, transition >= 0
void normalize(QVector<SpeedPoint>& ramp, int fileLength);
// Speed range like DaVinci (0 % = freeze frame is not a section speed)
inline constexpr double kMinSpeed = 0.01, kMaxSpeed = 100.0;
// Speed of section i (0 = before the first point)
double segmentSpeed(const Clip& c, int segment);
void setSegmentSpeed(Clip& c, int segment, double speed);
} // namespace Retime
