// SPDX-License-Identifier: AGPL-3.0-only
#include "core/JumpCut.h"

#include "core/Keyframes.h"

#include <cmath>
#include <cstdlib>

namespace JumpCut {

QVector<Jump> find(const Track& track, int minSkip)
{
    QVector<Jump> out;
    const auto plain = [](const Clip& c) {
        return c.kind == ClipKind::Media && c.enabled && !c.mediaPath.isEmpty() && !c.isRetimed();
    };
    for (int i = 0; i + 1 < track.clips.size(); ++i) {
        const Clip& a = track.clips[i];
        const Clip& b = track.clips[i + 1];
        if (a.end() != b.start || !plain(a) || !plain(b) || a.mediaPath != b.mediaPath || a.audioStream != b.audioStream)
            continue;
        const int skipped = b.in - (a.out + 1);
        if (skipped == 0 || std::abs(skipped) < std::max(1, minSkip)) continue;
        out << Jump{a.id, b.id, b.start, skipped};
    }
    return out;
}

bool sameFraming(const Clip& a, const Clip& b)
{
    const auto at = [](const Clip& c, AnimParam p, bool end) { return Keys::valueAt(c, p, end ? c.length() - 1 : 0); };
    for (AnimParam p : {AnimParam::ZoomX, AnimParam::ZoomY, AnimParam::PosX, AnimParam::PosY, AnimParam::Rotation})
        if (std::abs(at(a, p, true) - at(b, p, false)) > 1e-3) return false;
    return a.transform.transformOn == b.transform.transformOn;
}

} // namespace JumpCut
