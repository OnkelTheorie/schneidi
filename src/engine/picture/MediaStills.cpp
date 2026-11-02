// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/picture/MediaStills.h"

#include <algorithm>

namespace Picture {

Timeline mediaTimeline(const QString& path, double aspect)
{
    Timeline tl;
    tl.video.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = path;
    c.pictureAspect = std::max(0.0, aspect);
    c.out = 1 << 28; // as long as the file: past its end the reader gives the last picture
    tl.video[0].clips << c;
    return tl;
}

MediaStills::MediaStills(const ProjectFormat& format, std::function<QString(const QString&)> decodeSource, int keep)
    : m_format(format), m_decodeSource(std::move(decodeSource)), m_keep(std::max(1, keep))
{
}

QImage MediaStills::frame(const QString& path, int frame, const QSize& size, double aspect)
{
    std::shared_ptr<Renderer> r;
    const std::pair key{path, std::max(0.0, aspect)};
    {
        std::lock_guard lock(m_mutex);
        auto it = std::find_if(m_renderers.begin(), m_renderers.end(), [&](const auto& e) { return e.first == key; });
        if (it != m_renderers.end()) {
            m_renderers.splice(m_renderers.begin(), m_renderers, it);
        } else {
            Options o;
            o.format = m_format;
            o.mode = Mode::Still;
            o.proxy = m_decodeSource;
            m_renderers.emplace_front(key, std::make_shared<Renderer>(mediaTimeline(path, key.second), o));
            while (int(m_renderers.size()) > m_keep) m_renderers.pop_back();
        }
        r = m_renderers.front().second;
    }
    r->setDirection(0);
    r->setOutputSize(size);
    const int decoded = r->stats().layersDecoded;
    const Frame f = r->render(std::max(0, frame));
    // Nothing decoded: no picture in the file (or not readable)
    if (f.isNull() || r->stats().layersDecoded == decoded) return {};
    return f.toImage();
}

} // namespace Picture
