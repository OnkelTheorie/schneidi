// SPDX-License-Identifier: AGPL-3.0-only
#include "core/SourceBitrate.h"

#include "core/I18n.h"
#include "core/TimelineOps.h"

#include <QHash>
#include <QLocale>
#include <QSet>

#include <algorithm>
#include <climits>
#include <cmath>

namespace SourceBitrate {

namespace {

struct Sum {
    double weighted = 0; // kbit/s x frames
    qint64 frames = 0;
    QSet<QString> files;
};

struct Output {
    double pixels = 0, fps = 0; // 0 = unknown: nothing scaled
};

constexpr int kMaxDepth = 8; // nested sequences inside nested sequences (a cycle cannot happen, but stay safe)

// Video tracks of `tl` over [from, to) (timeline frames of `tl`); `onlyTrack` >= 0: that track alone (a multicam angle)
void collect(const Timeline& tl, const NestedTimelines* nested, int from, int to, int onlyTrack, const Output& out,
             const Lookup& lookup, QHash<QString, std::optional<Video>>& cache, Sum& sum, int depth)
{
    if (from >= to || depth > kMaxDepth) return;
    for (int t = 0; t < tl.video.size(); ++t) {
        if (onlyTrack >= 0 && t != onlyTrack) continue;
        const Track& track = tl.video[t];
        if (track.hidden) continue;
        for (const Clip& c : track.clips) {
            if (!c.enabled || c.freeze) continue;
            const int a = std::max(from, c.start), b = std::min(to, c.end());
            if (a >= b) continue;
            if (c.isNested()) {
                if (!nested || !nested->contains(c.sequenceId)) continue;
                const Timeline* inner = &*nested->constFind(c.sequenceId);
                int angleTrack = -1;
                if (c.isMulticam()) {
                    if (c.angle < 0 || c.angle >= inner->angles.size()) continue;
                    angleTrack = inner->angles[c.angle].videoTrack;
                    if (angleTrack < 0) continue; // a sound-only angle
                }
                // The nested sequence's frames the clip shows (speed changes are left aside: rough is enough here)
                const int innerFrom = c.in + (a - c.start);
                collect(*inner, nested, innerFrom, innerFrom + (b - a), angleTrack, out, lookup, cache, sum,
                        depth + 1);
                continue;
            }
            if (c.kind != ClipKind::Media || c.mediaPath.isEmpty()) continue; // titles, generated pictures
            auto it = cache.find(c.mediaPath);
            if (it == cache.end()) it = cache.insert(c.mediaPath, lookup ? lookup(c.mediaPath) : std::nullopt);
            const std::optional<Video>& v = *it;
            if (!v || !(v->kbit > 0)) continue;
            // The same bits per pixel as the source at the output's pixel rate; what the source does not know (its
            // size or frame rate) counts as the output's, so it does not scale
            double kbit = v->kbit;
            if (out.pixels > 0 && out.fps > 0) {
                const double pixels = v->size.isEmpty() ? out.pixels : double(v->size.width()) * v->size.height();
                const double fps = v->fps > 0 ? v->fps : out.fps;
                // Scaled up, a picture has no more detail than its source: only the square root of the factor
                // (1080p -> 4K: 2x, not 4x); down (smaller, fewer frames) linear
                const double factor = (out.pixels * out.fps) / (pixels * fps);
                kbit *= factor > 1 ? std::sqrt(factor) : factor;
            }
            sum.weighted += kbit * (b - a);
            sum.frames += b - a;
            sum.files.insert(c.mediaPath);
        }
    }
}

} // namespace

Result target(const Timeline& tl, int from, int to, QSize output, FrameRate outputRate, const Lookup& lookup)
{
    Result r;
    const int end = TimelineOps::endFrame(tl);
    const int a = std::max(0, from);
    const int b = to < 0 ? end : std::min(end, to + 1);
    if (a >= b) return r;
    Output out;
    if (!output.isEmpty() && outputRate.num > 0 && outputRate.den > 0) {
        out.pixels = double(output.width()) * output.height();
        out.fps = outputRate.fps();
    }
    QHash<QString, std::optional<Video>> cache;
    Sum sum;
    collect(tl, tl.nested.get(), a, b, -1, out, lookup, cache, sum, 0);
    if (sum.frames <= 0) return r;
    // At least 100 kbit/s (a tiny output of a tiny source), at most 800 Mbit/s (beyond any delivery codec's level)
    r.targetK = int(std::lround(std::clamp(sum.weighted / double(sum.frames), 100.0, 800000.0)));
    r.frames = int(std::min<qint64>(sum.frames, INT_MAX));
    r.files = int(sum.files.size());
    return r;
}

QString label(int kbit)
{
    if (kbit <= 0) return {};
    if (kbit < 1000) return QStringLiteral("≈ ") + T("%1 kbit/s").arg(kbit);
    const double mbit = kbit / 1000.0;
    // 2.5 / 12 / 130: one decimal below 10
    const QString n = mbit < 10 ? QLocale().toString(std::round(mbit * 10) / 10, 'f', 1)
                                : QLocale().toString(qint64(std::lround(mbit)));
    return QStringLiteral("≈ ") + T("%1 Mbit/s").arg(n);
}

} // namespace SourceBitrate
