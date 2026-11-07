// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/picture/TitleLook.h"

#include <algorithm>

namespace Picture {

Text::Look titleLook(const TitleLook& l, const QColor& base, double scale, const QImage& texture)
{
    Text::Look o;
    if (l.fill == TitleFill::Texture && !texture.isNull()) {
        o.texture = texture;
        o.texScale = std::max(0.01, l.texScale);
        o.texX = l.texX * scale;
        o.texY = l.texY * scale;
    } else if (l.fill == TitleFill::Gradient) {
        for (const ColorStop& s : l.gradientStops(base)) o.stops.append({s.at, s.color});
        o.gradAngle = l.gradAngle;
    }
    if (l.edgeOn && l.edgeDepth > 0) {
        o.edgeDepth = l.edgeDepth * scale;
        o.edgeAngle = l.edgeAngle;
        o.edgeColor = l.edgeColor;
        o.edgeLight = l.edgeLight;
    }
    if (l.shadowOn && l.shadowOpacity > 0) {
        o.shadow = l.shadowColor;
        o.shadow.setAlphaF(std::clamp(l.shadowColor.alphaF() * l.shadowOpacity, 0.0, 1.0));
        o.shadowX = l.shadowX * scale;
        o.shadowY = l.shadowY * scale;
        o.shadowSoft = std::max(0.0, l.shadowSoft) * scale;
    }
    if (l.glowOn && l.glowStrength > 0 && l.glowRadius > 0) {
        o.glow = l.glowColor;
        o.glow.setAlphaF(std::clamp(l.glowColor.alphaF() * l.glowStrength, 0.0, 1.0));
        o.glowRadius = l.glowRadius * scale;
    }
    return o;
}

} // namespace Picture
