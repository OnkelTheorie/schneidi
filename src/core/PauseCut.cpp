// SPDX-License-Identifier: AGPL-3.0-only
#include "core/PauseCut.h"

#include "core/ScriptAlign.h"

#include <algorithm>
#include <cmath>

namespace PauseCut {

double noiseFloorDb(const std::vector<float>& peaks)
{
    std::vector<float> heard;
    heard.reserve(peaks.size());
    for (float p : peaks)
        if (p > 0.f) heard.push_back(p); // digital silence (gaps, no clip) says nothing about the microphone
    if (heard.empty()) return -200;
    const size_t k = std::min(heard.size() - 1, size_t(double(heard.size()) * kFloorPercentile));
    std::nth_element(heard.begin(), heard.begin() + long(k), heard.end());
    return 20.0 * std::log10(std::max(1e-10, double(heard[k])));
}

double quietShare(const std::vector<float>& peaks, double thresholdDb)
{
    if (peaks.empty()) return 1;
    const float limit = float(std::pow(10.0, thresholdDb / 20.0));
    size_t n = 0;
    for (float p : peaks)
        if (p < limit) ++n;
    return double(n) / double(peaks.size());
}

Threshold autoThreshold(const std::vector<float>& peaks)
{
    Threshold t;
    t.floorDb = noiseFloorDb(peaks);
    if (t.floorDb <= -200) return t;
    t.db = std::clamp(t.floorDb + kAboveFloorDb, kMinThresholdDb, kMaxThresholdDb);
    t.quietShare = quietShare(peaks, t.db);
    if (t.floorDb > kNoisyFloorDb && t.quietShare < kNoisyQuietShare) {
        t.db = t.floorDb + kNoisyAboveFloorDb;
        t.quietShare = quietShare(peaks, t.db);
        t.raised = true;
    }
    return t;
}

double autoThresholdDb(const std::vector<float>& peaks) { return autoThreshold(peaks).db; }

std::vector<bool> quietFrames(const std::vector<float>& peaks, double thresholdDb, int length)
{
    const float limit = float(std::pow(10.0, thresholdDb / 20.0));
    std::vector<bool> out(size_t(std::max(0, length)), true);
    const size_t n = std::min(out.size(), peaks.size());
    for (size_t i = 0; i < n; ++i) out[i] = peaks[i] < limit;
    return out;
}

QVector<Range> pauses(const QVector<std::vector<bool>>& quiet, const QVector<int>& tracks, int minPause, int pad,
                      const std::vector<bool>* present)
{
    QVector<Range> out;
    if (quiet.isEmpty()) return out;
    size_t length = quiet.first().size();
    for (const auto& q : quiet) length = std::min(length, q.size());
    if (present) length = std::min(length, present->size());
    minPause = std::max(1, minPause);
    pad = std::max(0, pad);
    const auto allQuiet = [&](size_t i) {
        if (present && !(*present)[i]) return false;
        for (const auto& q : quiet)
            if (!q[i]) return false;
        return true;
    };
    size_t i = 0;
    while (i < length) {
        if (!allQuiet(i)) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < length && allQuiet(j)) ++j;
        // A pad keeps air next to speech; at the end of what was measured no speech follows - a pad there would only
        // leave a sliver of silence behind the last cut (videotest 21.11.: 9 frames at the end)
        const int from = int(i) + pad, to = j >= length ? int(j) : int(j) - pad;
        if (int(j - i) >= minPause && to > from) out << Range{from, to, Kind::Pause, tracks, {}};
        i = j;
    }
    return out;
}

QVector<Range> fillers(const QVector<TranscriptCut::Word>& words, const QVector<std::vector<float>>& peaks,
                       const QVector<double>& thresholdsDb, const QVector<int>& tracks)
{
    QVector<Range> out;
    const auto loudest = [&](int k, int from, int to) {
        float m = 0.f;
        const std::vector<float>& p = peaks[k];
        for (int f = std::max(0, from); f < to && size_t(f) < p.size(); ++f) m = std::max(m, p[size_t(f)]);
        return 20.0 * std::log10(std::max(1e-10, double(m)));
    };
    for (const TranscriptCut::Word& w : words) {
        if (!tracks.contains(w.track)) continue;
        if (!ScriptAlign::isFiller(ScriptAlign::normalize(w.text), w.language)) continue;
        const int spoken = w.spokenTo > w.from ? std::min(w.spokenTo, w.to) : w.to;
        int from = w.from, to = std::max(w.from + 1, spoken);
        const int own = int(tracks.indexOf(w.track));
        if (own < peaks.size()) {
            // The edges onto the nearest quiet frames of its own track: the cut starts and ends in its quiet
            const std::vector<float>& p = peaks[own];
            const float limit = float(std::pow(10.0, thresholdsDb.value(own, kMaxThresholdDb) / 20.0));
            const auto quietAt = [&](int f) { return f >= 0 && (size_t(f) >= p.size() || p[size_t(f)] < limit); };
            const auto snap = [&](int at) {
                for (int d = 0; d <= kSnapFrames; ++d) {
                    if (quietAt(at - d)) return at - d;
                    if (quietAt(at + d)) return at + d;
                }
                return -1;
            };
            const int a = snap(from), b = snap(to);
            if (a < 0 || b < 0 || b <= a) continue;
            from = a;
            to = b;
        }
        bool othersQuiet = true;
        const double ownDb = own < peaks.size() ? loudest(own, from, to) : 0.0;
        for (int k = 0; k < peaks.size() && othersQuiet; ++k) {
            if (k == own) continue;
            const double db = loudest(k, from, to);
            if (db >= thresholdsDb.value(k, kMaxThresholdDb) && db > ownDb - kBleedDb) othersQuiet = false;
        }
        if (othersQuiet) out << Range{from, to, Kind::Filler, {w.track}, w.text.trimmed()};
    }
    return out;
}

QVector<Range> combined(const QVector<Range>& pauses, const QVector<Range>& fillers)
{
    QVector<Range> all = pauses + fillers;
    std::sort(all.begin(), all.end(), [](const Range& a, const Range& b) { return a.from < b.from; });
    QVector<Range> out;
    for (const Range& r : all) {
        if (out.isEmpty() || r.from > out.last().to) {
            out << r;
            continue;
        }
        Range& m = out.last();
        m.to = std::max(m.to, r.to);
        if (r.kind == Kind::Pause || m.kind == Kind::Pause) {
            m.kind = Kind::Pause;
            m.text.clear();
        } else if (!r.text.isEmpty()) {
            m.text = m.text.isEmpty() ? r.text : m.text + ' ' + r.text;
        }
        for (int t : r.tracks)
            if (!m.tracks.contains(t)) m.tracks << t;
        std::sort(m.tracks.begin(), m.tracks.end());
    }
    return out;
}

QString kindId(Kind k) { return k == Kind::Filler ? QStringLiteral("filler") : QStringLiteral("pause"); }

} // namespace PauseCut
