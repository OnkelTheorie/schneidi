// SPDX-License-Identifier: AGPL-3.0-only
#include "core/PauseCut.h"

#include "core/ScriptAlign.h"

#include <algorithm>
#include <cmath>

namespace PauseCut {

double autoThresholdDb(const std::vector<float>& peaks)
{
    std::vector<float> heard;
    heard.reserve(peaks.size());
    for (float p : peaks)
        if (p > 0.f) heard.push_back(p); // digital silence (gaps, no clip) says nothing about the microphone
    if (heard.empty()) return kMaxThresholdDb;
    const size_t k = std::min(heard.size() - 1, size_t(double(heard.size()) * kFloorPercentile));
    std::nth_element(heard.begin(), heard.begin() + long(k), heard.end());
    const double floorDb = 20.0 * std::log10(std::max(1e-10, double(heard[k])));
    return std::clamp(floorDb + kAboveFloorDb, kMinThresholdDb, kMaxThresholdDb);
}

std::vector<bool> quietFrames(const std::vector<float>& peaks, double thresholdDb, int length)
{
    const float limit = float(std::pow(10.0, thresholdDb / 20.0));
    std::vector<bool> out(size_t(std::max(0, length)), true);
    const size_t n = std::min(out.size(), peaks.size());
    for (size_t i = 0; i < n; ++i) out[i] = peaks[i] < limit;
    return out;
}

QVector<Range> pauses(const QVector<std::vector<bool>>& quiet, const QVector<int>& tracks, int minPause, int pad)
{
    QVector<Range> out;
    if (quiet.isEmpty()) return out;
    size_t length = quiet.first().size();
    for (const auto& q : quiet) length = std::min(length, q.size());
    minPause = std::max(1, minPause);
    pad = std::max(0, pad);
    const auto allQuiet = [&](size_t i) {
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
        const int from = int(i) + pad, to = int(j) - pad;
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
        if (!ScriptAlign::isFiller(ScriptAlign::normalize(w.text))) continue;
        const int spoken = w.spokenTo > w.from ? std::min(w.spokenTo, w.to) : w.to;
        const int from = w.from, to = std::max(w.from + 1, spoken);
        bool othersQuiet = true;
        const int own = int(tracks.indexOf(w.track));
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
    QVector<Range> out = pauses;
    for (const Range& f : fillers) {
        const bool inPause = std::any_of(pauses.begin(), pauses.end(),
                                         [&](const Range& p) { return p.from < f.to && f.from < p.to; });
        if (!inPause) out << f;
    }
    std::sort(out.begin(), out.end(), [](const Range& a, const Range& b) { return a.from < b.from; });
    return out;
}

QString kindId(Kind k) { return k == Kind::Filler ? QStringLiteral("filler") : QStringLiteral("pause"); }

} // namespace PauseCut
