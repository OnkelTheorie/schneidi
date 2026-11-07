// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// A title's look (core TitleLook: fill, edge, shadow, glow; project pixels) as Text::Look in pixels of the picture
// it is drawn on. Shared by title clips (Renderer) and the compositing node "text".

#include "core/Types.h"
#include "engine/video/Text.h"

#include <QImage>

namespace Picture {

// scale = picture pixels per project pixel; texture = the picture of look.texture (null = none: plain colour)
Text::Look titleLook(const TitleLook& look, const QColor& base, double scale, const QImage& texture);

} // namespace Picture
