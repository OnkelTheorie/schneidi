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
