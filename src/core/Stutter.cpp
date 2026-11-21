// SPDX-License-Identifier: AGPL-3.0-only
#include "core/Stutter.h"

#include <algorithm>
#include <cmath>

namespace Stutter {

Scan fromTimestamps(std::vector<double> times)
{
    Scan s;
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    s.frames = int(times.size());
    if (times.size() < 3) return s;
    std::vector<double> deltas(times.size() - 1);
    for (size_t i = 1; i < times.size(); ++i) deltas[i - 1] = times[i] - times[i - 1];
    std::vector<double> sorted = deltas;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const double d = sorted[sorted.size() / 2];
    if (d <= 0) return s;
    s.frameDuration = d;
    s.duration = times.back() - times.front() + d;
    for (size_t i = 0; i < deltas.size(); ++i)
        if (deltas[i] > 2 * d + 1e-6) // the tolerance only absorbs rounding of the time stamps
            s.jumps << Jump{times[i], deltas[i], std::max(1, int(std::lround(deltas[i] / d)) - 1)};
    return s;
}

Scan fromPackets(std::vector<Packet> packets, int pixels)
{
    std::sort(packets.begin(), packets.end(), [](const Packet& a, const Packet& b) { return a.time < b.time; });
    packets.erase(std::unique(packets.begin(), packets.end(), [](const Packet& a, const Packet& b) { return a.time == b.time; }),
                  packets.end());
    std::vector<double> times(packets.size());
    for (size_t i = 0; i < packets.size(); ++i) times[i] = packets[i].time;
    Scan s = fromTimestamps(times);
    if (pixels <= 0 || s.jumps.isEmpty()) return s;
    const double limit = kUnchangedBytesPerPixel * pixels;
    const auto unchanged = [&](size_t k) { return k < packets.size() && !packets[k].key && packets[k].size <= limit; };
    QVector<Jump> seen;
    for (const Jump& j : s.jumps) {
        const size_t before = size_t(std::lower_bound(times.begin(), times.end(), j.at) - times.begin()), after = before + 1;
        const bool repeated = after < packets.size()
                              && (unchanged(after) || (packets[after].key && unchanged(before) && unchanged(after + 1)));
        if (!repeated) seen << j;
    }
    s.jumps = seen;
    return s;
}

QVector<Jump> inRange(const Scan& s, double from, double to)
{
    // The frozen picture lies between the two frames; half a frame of tolerance for rounding to frames
    const double tol = s.frameDuration / 2;
    QVector<Jump> out;
    for (const Jump& j : s.jumps)
        if (j.at + tol < to && j.end() - tol > from) out << j;
    return out;
}

QVector<Stretch> stretches(const QVector<Jump>& jumps, double join)
{
    QVector<Stretch> out;
    for (const Jump& j : jumps) {
        if (!out.isEmpty() && j.at - out.last().to <= join) {
            Stretch& s = out.last();
            s.to = std::max(s.to, j.end());
            ++s.jumps;
            s.missing += j.missing;
            s.longest = std::max(s.longest, j.length);
        } else {
            out << Stretch{j.at, j.end(), 1, j.missing, j.length};
        }
    }
    return out;
}

QVector<QPair<double, double>> cleanPieces(const Scan& s, double from, double to, double margin, double minLength)
{
    QVector<QPair<double, double>> out;
    double at = from;
    const auto piece = [&](double a, double b) {
        if (b - a >= minLength) out.append({a, b});
    };
    for (const Stretch& st : stretches(inRange(s, from, to))) {
        piece(at, std::min(to, st.from + s.frameDuration));
        at = std::max(at, st.to + margin);
    }
    piece(at, to);
    return out;
}

} // namespace Stutter
