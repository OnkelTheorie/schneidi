// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// On-screen overlays (Color page windows, Comp page handles) over a clip tilted in depth: its flat transform with the
// same perspective the picture engine draws it with (engine/picture/Renderer: Compose::tilted around the clip's center)

#include "core/Keyframes.h"
#include "core/Types.h"
#include "engine/video/Compose.h"

#include <QPointF>
#include <QTransform>

#include <algorithm>
#include <cmath>

namespace TiltTransform {

// flat: picture pixels (w x h) -> project pixels of a pw x ph frame, the picture's center landing on the clip's pivot.
// Returns it tilted by the clip's Kippen X/Y at clip frame t (unchanged when not tilted or when the tilt hides it)
inline QTransform apply(const QTransform& flat, const Clip& c, double t, double w, double h, double pw, double ph)
{
    if (!Keys::hasTilt(c)) return flat;
    const double tx = std::clamp(Keys::valueAt(c, AnimParam::TiltX, t), -89.0, 89.0);
    const double ty = std::clamp(Keys::valueAt(c, AnimParam::TiltY, t), -89.0, 89.0);
    if (std::abs(tx) < 1e-9 && std::abs(ty) < 1e-9) return flat;
    const double fov = std::clamp(Keys::valueAt(c, AnimParam::Fov, t), 5.0, 120.0);
    const int sw = std::max(1, int(std::lround(w))), sh = std::max(1, int(std::lround(h)));
    // Unit source pixels: the picture scaled to whole pixels first (the homography maps its corners)
    QTransform unit;
    unit.scale(w / sw, h / sh);
    const QTransform m0 = unit * flat;
    Compose::Affine m;
    m.a = m0.m11(), m.b = m0.m21(), m.c = m0.dx(), m.d = m0.m12(), m.e = m0.m22(), m.f = m0.dy();
    const QPointF pivot = flat.map(QPointF(w / 2, h / 2));
    Compose::Homography hm;
    if (!Compose::tilted(m, sw, sh, pivot.x(), pivot.y(), tx, ty, fov, int(std::lround(pw)), int(std::lround(ph)), &hm))
        return flat;
    const double* k = hm.m;
    return unit.inverted() * QTransform(k[0], k[3], k[6], k[1], k[4], k[7], k[2], k[5], k[8]);
}

} // namespace TiltTransform
