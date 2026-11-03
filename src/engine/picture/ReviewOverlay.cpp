// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/picture/ReviewOverlay.h"

#include <QImage>
#include <QPainter>

namespace Picture {

std::function<void(uint8_t* rgba, int width, int height, int64_t frame)>
reviewOverlay(std::shared_ptr<const ReviewSheet::Data> data)
{
    return [data = std::move(data)](uint8_t* rgba, int width, int height, int64_t frame) {
        if (!data || !rgba || width <= 0 || height <= 0) return;
        // A view on the frame's buffer (straight alpha; an export frame is opaque, so painting over it is exact)
        QImage view(rgba, width, height, width * 4, QImage::Format_RGBA8888);
        QPainter p(&view);
        ReviewSheet::drawOverlay(p, QSize(width, height), *data, int(frame));
    };
}

} // namespace Picture
