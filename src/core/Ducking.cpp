// SPDX-License-Identifier: AGPL-3.0-only
#include "core/Ducking.h"

#include "core/Keyframes.h"

#include <algorithm>
#include <cmath>

namespace Ducking {

Params defaults(double fps)
{
    Params p;
    p.attack = int(std::lround(0.15 * fps));
    p.release = int(std::lround(0.4 * fps));
    p.hold = int(std::lround(0.5 * fps));
    return p;
}

Ranges soundRanges(const std::vector<float>& peaks, int first, const Params& p)
{
    const float limit = float(std::pow(10.0, p.thresholdDb / 20.0));
    const int bridge = p.hold + p.attack + p.release;
    Ranges out;
    for (int i = 0; i < int(peaks.size()); ++i) {
        if (peaks[size_t(i)] < limit) continue;
        const int f = first + i;
        if (!out.isEmpty() && f - out.last().second <= bridge) out.last().second = f + 1;
        else out.append({f, f + 1});
    }
    return out;
}

double gameThreshold(const std::vector<float>& peaks)
{
    const float floor = float(std::pow(10.0, -70 / 20.0));
    std::vector<float> sound;
    sound.reserve(peaks.size());
    for (float v : peaks)
        if (v >= floor) sound.push_back(v);
    if (sound.empty()) return -35;
    const size_t k = std::min(sound.size() - 1, size_t(0.9 * double(sound.size())));
    std::nth_element(sound.begin(), sound.begin() + std::ptrdiff_t(k), sound.end());
    const double p90 = 20 * std::log10(std::max(1e-10, double(sound[k])));
    return std::clamp(std::round((p90 - 2) * 10) / 10, -35.0, -6.0);
}

Ranges unite(const Ranges& a, const Ranges& b, int bridge)
{
    Ranges all = a + b;
    std::sort(all.begin(), all.end());
    Ranges out;
    for (const auto& r : all) {
        if (!out.isEmpty() && r.first - out.last().second <= bridge) out.last().second = std::max(out.last().second, r.second);
        else out << r;
    }
    return out;
}

bool apply(Clip& c, const Ranges& ranges, const Params& p)
{
    Keys::clear(c, AnimParam::Volume);
    const double base = c.volumeDb, low = std::max(kMinVolumeDb, base + p.amountDb);
    const int len = c.length();
    bool any = false;
    for (const auto& [from, to] : ranges) {
        const int a = from - c.start, b = to - c.start; // clip frames
        if (b + p.release <= 0 || a - p.attack >= len) continue;
        any = true;
        const auto key = [&](int t, double v) {
            if (t >= 0 && t < len) Keys::setKey(c, AnimParam::Volume, t, v);
        };
        key(a - p.attack, base);
        key(std::max(0, a), low);
        key(std::min(len - 1, b), low);
        key(b + p.release, base);
        if (a <= 0) Keys::setKey(c, AnimParam::Volume, 0, low);             // starts inside a sound
        if (b >= len - 1) Keys::setKey(c, AnimParam::Volume, len - 1, low); // ends inside one
    }
    return any;
}

} // namespace Ducking
