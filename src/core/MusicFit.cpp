// SPDX-License-Identifier: AGPL-3.0-only
#include "core/MusicFit.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace MusicFit {

namespace {

constexpr double kEps = 0.002;         // seconds: a start this close to a window counts as in it
constexpr double kShortFade = 0.04;    // seconds: crossfade where the sound around the cut is alike
constexpr double kChangeDb = 3;        // above this the sound changes at the cut: a crossfade of two beats
constexpr double kBad = std::numeric_limits<double>::infinity();

struct Grid {
    const Beats::Result& r;
    double songLength = 0;
    int bars = 0;               // whole bars from the first bar one
    std::vector<double> start;  // bar b starts at start[b], b = 0..bars
    double bar = 0, lead = 0;   // bar length; time before the first bar one
    int lastPhrase = 0;         // start of the last phrase (the song's end part)

    explicit Grid(const Beats::Result& res, double len) : r(res), songLength(len)
    {
        bars = int(r.barEnergyDb.size());
        bar = 4 * r.period;
        for (int b = 0; b <= bars; ++b) start.push_back(Beats::beatTime(r, r.downbeat + 4 * b));
        lead = std::max(0.0, start.empty() ? 0.0 : start[0]);
        lastPhrase = 0;
        for (int b = 0; b < bars; ++b)
            if (phrase(b) && (bars - b) * 2 >= r.phraseBars) lastPhrase = b;
    }
    bool phrase(int b) const
    {
        return b == 0 || (b >= r.phraseOffset && (b - r.phraseOffset) % std::max(1, r.phraseBars) == 0);
    }
    double energy(int b) const { return r.barEnergyDb[size_t(std::clamp(b, 0, bars - 1))]; }
    double bass(int b) const { return r.barBassDb[size_t(std::clamp(b, 0, bars - 1))]; }
    // How far the sound around a cut from bar a (the part before it ends before bar a) to bar b differs
    double level(int a, int b) const
    {
        return 0.5 * (std::abs(energy(a) - energy(b)) + std::abs(energy(a - 1) - energy(b - 1)));
    }
    double seamCost(int a, int b) const
    {
        const double bassDiff = 0.5 * (std::abs(bass(a) - bass(b)) + std::abs(bass(a - 1) - bass(b - 1)));
        return level(a, b) + 0.5 * bassDiff + (phrase(a) ? 0 : 4) + (phrase(b) ? 0 : 4);
    }
    // Where the song may start: before the first bar one (cost 0), up to `lead` before a later bar one (intro bars
    // dropped), else inside a bar (fade-in)
    double startCost(double s0, bool* inBar) const
    {
        *inBar = false;
        if (s0 < -kEps) return kBad;
        if (s0 <= lead + kEps) return 0;
        for (int m = 1; m < bars; ++m)
            if (start[size_t(m)] >= s0 - kEps) {
                if (start[size_t(m)] - s0 <= lead + kEps) return 3 + m;
                break;
            }
        *inBar = true;
        return 20 + s0 / std::max(bar, 0.1);
    }
};

struct Candidate {
    std::vector<int> from, to; // per cut: bar where the part before ends, bar where the next starts
    double s0 = 0;
    double cost = kBad;
    bool inBar = false;
};

// Song time -> result time through the parts; -1 = cut out
double resultTime(const std::vector<Segment>& parts, double t)
{
    double at = 0;
    for (const Segment& s : parts) {
        if (t >= s.from && t < s.to) return at + (t - s.from);
        at += s.to - s.from;
    }
    return -1;
}

std::vector<Segment> partsOf(const Grid& g, const Candidate& c)
{
    std::vector<Segment> parts;
    double from = c.s0;
    for (size_t i = 0; i < c.from.size(); ++i) {
        parts.push_back({from, g.start[size_t(c.from[i])]});
        from = g.start[size_t(c.to[i])];
    }
    parts.push_back({from, g.songLength});
    return parts;
}

} // namespace

Plan plan(const Beats::Result& r, double songLength, const Options& o)
{
    Plan p;
    const double T = o.length;
    if (T <= 0) {
        p.errorCode = "BAD_ARGUMENT";
        p.error = "the length must be more than 0";
        return p;
    }
    if (songLength + kEps < T) {
        p.errorCode = "TOO_SHORT";
        p.error = QString("the song is %1 s long, shorter than the length %2 s (a longer song, or loop it by placing it "
                          "twice)")
                      .arg(songLength, 0, 'f', 2)
                      .arg(T, 0, 'f', 2);
        return p;
    }
    const Grid g(r, songLength);
    const bool haveDrop = !r.drops.empty();
    const double drop = haveDrop ? Beats::beatTime(r, r.drops.front().beat) : -1;

    // Drop and climax: kept, and where it lands
    const auto dropCost = [&](const std::vector<Segment>& parts) {
        if (!haveDrop) return o.keepDrop || o.climaxAt >= 0 ? kBad : 0.0;
        const double at = resultTime(parts, drop);
        if (at < 0) return o.keepDrop || o.climaxAt >= 0 ? kBad : 3.0;
        return o.climaxAt >= 0 ? 2 * std::abs(at - o.climaxAt) : 0.0;
    };

    if (o.end == End::Fade) {
        // No cut: the song from its start (or from where the drop lands on the climax) faded out at the length
        double s0 = 0;
        if (o.climaxAt >= 0 && haveDrop) s0 = std::clamp(drop - o.climaxAt, 0.0, songLength - T);
        bool inBar = false;
        if (s0 > 0) {
            // On the nearest start window (before a bar one) at or before that
            double best = 0;
            for (int m = 0; m < g.bars; ++m) {
                const double w = g.start[size_t(m)];
                if (w - g.lead <= s0 + kEps) best = std::max(best, std::min(s0, w));
            }
            s0 = best;
            g.startCost(s0, &inBar);
        }
        p.segments.push_back({s0, s0 + T});
        p.startTrim = s0;
        p.startInBar = inBar;
        p.fadeOut = std::min({2 * g.bar > 0 ? 2 * g.bar : 4.0, T / 4, 5.0});
    } else {
        Candidate best;
        const auto consider = [&](Candidate c) {
            const std::vector<Segment> parts = partsOf(g, c);
            for (const Segment& s : parts)
                if (s.to - s.from < g.bar * 0.99) return; // every part at least a bar
            bool inBar = false;
            double cost = g.startCost(c.s0, &inBar);
            if (cost == kBad) return;
            cost += dropCost(parts);
            if (cost == kBad) return;
            for (size_t i = 0; i < c.from.size(); ++i) cost += 1 + g.seamCost(c.from[i], c.to[i]);
            // The end part: the last phrase and what rings out after it
            if (!c.to.empty() && c.to.back() > g.lastPhrase) cost += 5.0 * (c.to.back() - g.lastPhrase);
            // A part in the middle shorter than a phrase
            for (size_t i = 1; i < c.from.size(); ++i)
                if (c.from[i] - c.to[i - 1] < r.phraseBars) cost += 2;
            if (cost < best.cost) {
                c.cost = cost;
                c.inBar = inBar;
                best = c;
            }
        };
        const double E = songLength;
        consider({{}, {}, E - T});
        if (g.bars >= 3 && r.period > 0) {
            for (int a = 1; a < g.bars; ++a)
                for (int b = a + 1; b < g.bars; ++b) consider({{a}, {b}, g.start[size_t(a)] + E - g.start[size_t(b)] - T});
            // Two cuts, when one is not good (a climax far from where one cut can put the drop, sounds that differ)
            if (best.cost > 6 || o.climaxAt >= 0) {
                for (int a1 = 1; a1 < g.bars; ++a1)
                    for (int b1 = a1 + 1; b1 < g.bars; ++b1)
                        for (int a2 = b1 + 1; a2 < g.bars; ++a2) {
                            const double rest = g.start[size_t(a1)] + g.start[size_t(a2)] - g.start[size_t(b1)] + E - T;
                            // The start of the last part that leaves the least to take off the song's start, and
                            // one bar earlier (a later bar one to start from)
                            int b2 = int(std::floor((rest + kEps - g.start[0]) / g.bar));
                            for (int k = 0; k < 2; ++k, --b2)
                                if (b2 > a2 && b2 < g.bars) consider({{a1, a2}, {b1, b2}, rest - g.start[size_t(b2)]});
                        }
            }
        }
        if (best.cost == kBad) {
            p.errorCode = "NO_FIT";
            p.error = o.keepDrop || o.climaxAt >= 0
                          ? QString("no cut of the song keeps its strongest drop%1 at this length")
                                .arg(o.climaxAt >= 0 ? " on the climax" : "")
                          : QString("the song cannot be cut to this length on its bars");
            return p;
        }
        p.segments = partsOf(g, best);
        p.startTrim = std::max(0.0, best.s0);
        p.segments.front().from = p.startTrim;
        p.startInBar = best.inBar;
        double at = 0;
        for (size_t i = 0; i < best.from.size(); ++i) {
            at += p.segments[i].to - p.segments[i].from;
            Cut c;
            c.at = at;
            c.from = g.start[size_t(best.from[i])];
            c.to = g.start[size_t(best.to[i])];
            c.fromBar = best.from[i];
            c.toBar = best.to[i];
            c.phrase = g.phrase(c.fromBar) && g.phrase(c.toBar);
            c.levelDb = g.level(c.fromBar, c.toBar);
            c.crossfade = o.crossfade >= 0 ? o.crossfade : c.levelDb > kChangeDb ? 2 * r.period : kShortFade;
            p.cuts.push_back(c);
        }
    }
    if (p.startInBar) p.fadeIn = std::min(r.period > 0 ? r.period / 2 : 0.25, 0.25);
    p.length = 0;
    for (const Segment& s : p.segments) p.length += s.to - s.from;
    if (haveDrop) {
        p.dropAt = resultTime(p.segments, drop);
        if (p.dropAt >= 0) p.dropSong = drop;
    }
    p.ok = true;
    return p;
}

QVector<Clip> clips(const Plan& p, const Clip& like, double fps, int at, int fileLength)
{
    QVector<Clip> out;
    if (!p.ok || p.segments.empty() || fileLength <= 0) return out;
    const int target = int(std::lround(p.length * fps));
    const auto build = [&](int shift) {
        QVector<Clip> list;
        int start = at;
        double tau = at; // exact timeline frame where the current part's first sound plays
        for (size_t i = 0; i < p.segments.size(); ++i) {
            const Segment& s = p.segments[i];
            const bool last = i + 1 == p.segments.size();
            Clip c = like;
            c.id = 0;
            c.linkId = 0;
            c.keys.clear();
            c.transIn = c.transOut = c.fadeIn = c.fadeOut = 0;
            c.transInAlone = c.transOutAlone = false;
            c.transInStyle = c.transOutStyle = TransitionStyle{};
            c.start = start;
            const double from = s.from * fps;
            if (i == 0) {
                c.in = std::clamp(int(std::lround(from)) + shift, 0, fileLength - 1);
                c.subframe = 0;
                tau = start + (from - c.in);
            } else {
                // Source frame `in` plays at start + subframe/100: on the grid of the part before
                const double x = from - tau + start;
                c.in = int(std::ceil(x - 1e-9));
                int sf = int(std::lround(100 * (c.in - x)));
                if (sf >= 100) {
                    c.in -= 1;
                    sf = 0;
                }
                c.subframe = std::clamp(sf, 0, 99);
                c.in = std::clamp(c.in, 0, fileLength - 1);
            }
            const double tauEnd = tau + (s.to - s.from) * fps;
            int length = 0;
            if (last && p.fadeOut <= 0 && s.to >= double(fileLength) / fps - 1e-6) length = fileLength - c.in;
            else length = int(std::lround(tauEnd)) - start;
            length = std::clamp(length, 1, fileLength - c.in);
            c.out = c.in + length - 1;
            list << c;
            start += length;
            tau = tauEnd;
        }
        return list;
    };
    out = build(0);
    for (int pass = 0; pass < 2 && !out.isEmpty(); ++pass) {
        const int diff = out.last().end() - at - target; // whole frames off the length: start that much later/earlier
        if (diff == 0) break;
        const QVector<Clip> again = build(out.first().in - int(std::lround(p.segments.front().from * fps)) + diff);
        if (again.isEmpty() || std::abs(again.last().end() - at - target) >= std::abs(diff)) break;
        out = again;
    }
    // Crossfades centered on the cuts (handles: the song on both sides), fades at the ends
    for (int i = 0; i + 1 < out.size(); ++i) {
        const Cut& c = p.cuts[size_t(i)];
        const int len = std::clamp(int(std::lround(c.crossfade * fps)), 1,
                                   std::max(1, std::min(out[i].length(), out[i + 1].length()) - 1));
        out[i].transOut = out[i + 1].transIn = len;
    }
    if (p.fadeIn > 0) out.first().fadeIn = std::max(1, int(std::lround(p.fadeIn * fps)));
    if (p.fadeOut > 0) out.last().fadeOut = std::max(1, int(std::lround(p.fadeOut * fps)));
    return out;
}

} // namespace MusicFit
