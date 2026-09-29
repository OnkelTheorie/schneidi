#include "core/Retime.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr double kInf = std::numeric_limits<double>::infinity();
}

double RetimeMap::Piece::sourceAt(double m) const
{
    const double x = m - m0;
    if (v0 == v1 || !std::isfinite(m1)) return s0 + v0 * x;
    return s0 + v0 * x + (v1 - v0) * x * x / (2.0 * (m1 - m0));
}

double RetimeMap::Piece::speedAt(double m) const
{
    if (v0 == v1 || !std::isfinite(m1)) return v0;
    return v0 + (v1 - v0) * std::clamp((m - m0) / (m1 - m0), 0.0, 1.0);
}

RetimeMap::RetimeMap(const Clip& c, int fileLength) : m_fileLength(fileLength), m_reverse(c.reverse)
{
    const double base = c.speed > 0 ? c.speed : 1.0;
    if (c.freeze || c.ramp.isEmpty()) {
        m_pieces << Piece{0, kInf, 0, base, base};
        return;
    }
    // Harte Abschnitte: Quell-Grenzen S, Tempo v, Material-Grenzen M
    QVector<double> S{0}, v{base};
    for (const SpeedPoint& p : c.ramp) {
        S << p.source;
        v << (p.speed > 0 ? p.speed : 1.0);
    }
    QVector<double> M{0};
    for (int i = 1; i < S.size(); ++i) M << M.last() + (S[i] - S[i - 1]) / v[i - 1];
    m_points = M.mid(1);
    // Übergang je Punkt j (zwischen Abschnitt j und j+1) höchstens so lang wie die halben Nachbarabschnitte je Seite
    const int n = c.ramp.size();
    QVector<double> W(n, 0.0);
    for (int j = 0; j < n; ++j) {
        const double left = M[j + 1] - M[j];
        const double right = j + 2 < M.size() ? M[j + 2] - M[j + 1] : kInf;
        const double leftRoom = j == 0 ? left * 2 : left; // Abschnitt 0 hat links keinen weiteren Übergang
        W[j] = std::clamp(double(c.ramp[j].smooth), 0.0, std::min(leftRoom, right));
    }
    double m = 0, s = 0;
    for (int i = 0; i < v.size(); ++i) {
        // konstanter Teil bis zum Beginn des nächsten Übergangs
        const bool last = i + 1 >= v.size();
        const double constEnd = last ? kInf : M[i + 1] - W[i] / 2;
        if (constEnd > m) {
            m_pieces << Piece{m, constEnd, s, v[i], v[i]};
            if (last) break;
            s = m_pieces.last().end();
            m = constEnd;
        }
        if (!last && W[i] > 0) {
            m_pieces << Piece{m, m + W[i], s, v[i], v[i + 1]};
            s = m_pieces.last().end();
            m += W[i];
        }
    }
    if (m_pieces.isEmpty() || std::isfinite(m_pieces.last().m1)) m_pieces << Piece{m, kInf, s, v.last(), v.last()};
}

const RetimeMap::Piece& RetimeMap::pieceAtMaterial(double m) const
{
    for (const Piece& p : m_pieces)
        if (m < p.m1) return p;
    return m_pieces.last();
}

double RetimeMap::sourceAt(double m) const
{
    return pieceAtMaterial(m).sourceAt(m);
}

double RetimeMap::speedAt(double m) const
{
    return pieceAtMaterial(m).speedAt(m);
}

double RetimeMap::materialAt(double s) const
{
    for (const Piece& p : m_pieces) {
        if (std::isfinite(p.m1) && s >= p.end()) continue;
        const double d = s - p.s0;
        if (p.v0 == p.v1 || !std::isfinite(p.m1)) return p.m0 + d / p.v0;
        // quadratisch: a·x² + v0·x − d = 0, a = (v1 − v0) / (2w); Tempo > 0 -> die Wurzel mit + ist die richtige
        const double a = (p.v1 - p.v0) / (2.0 * (p.m1 - p.m0));
        const double disc = std::max(0.0, p.v0 * p.v0 + 4.0 * a * d);
        const double x = 2.0 * d / (p.v0 + std::sqrt(disc)); // numerisch stabil (auch bei a ≈ 0)
        return p.m0 + x;
    }
    return 0;
}

double RetimeMap::fileFrameAt(double m) const
{
    const double s = sourceAt(m);
    return m_reverse ? m_fileLength - 1 - s : s;
}

double RetimeMap::materialAtFile(double f) const
{
    return materialAt(m_reverse ? m_fileLength - 1 - f : f);
}

int RetimeMap::length() const
{
    if (m_fileLength <= 0) return 0;
    return std::max(1, int(materialAt(m_fileLength)));
}

int Clip::retimedLength(int fileLength) const
{
    if (fileLength <= 0 || freeze) return 0;
    if (ramp.isEmpty()) return std::max(1, int(fileLength / speed));
    return RetimeMap(*this, fileLength).length();
}

namespace Retime {

void normalize(QVector<SpeedPoint>& ramp, int fileLength)
{
    std::sort(ramp.begin(), ramp.end(), [](const SpeedPoint& a, const SpeedPoint& b) { return a.source < b.source; });
    QVector<SpeedPoint> out;
    double prev = 0;
    for (SpeedPoint p : ramp) {
        if (p.source < prev + 1 || (fileLength > 0 && p.source > fileLength - 1)) continue;
        p.speed = std::clamp(p.speed > 0 ? p.speed : 1.0, kMinSpeed, kMaxSpeed);
        p.smooth = std::max(0, p.smooth);
        out << p;
        prev = p.source;
    }
    ramp = out;
}

double segmentSpeed(const Clip& c, int segment)
{
    if (segment <= 0 || c.ramp.isEmpty()) return c.speed;
    return c.ramp[std::min(segment, int(c.ramp.size())) - 1].speed;
}

void setSegmentSpeed(Clip& c, int segment, double speed)
{
    speed = std::clamp(speed, kMinSpeed, kMaxSpeed);
    if (segment <= 0) c.speed = speed;
    else if (segment - 1 < c.ramp.size()) c.ramp[segment - 1].speed = speed;
}

} // namespace Retime
