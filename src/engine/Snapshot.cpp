// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/Snapshot.h"
#include "engine/picture/MediaStills.h"

#include "core/ContactSheet.h"
#include "core/Timecode.h"

#include "engine/picture/Renderer.h"

#include <memory>

namespace Snapshot {

namespace {

QSize grabSize(const ProjectFormat& format, int maxEdge)
{
    QSize size = format.size();
    if (maxEdge > 0 && std::max(size.width(), size.height()) > maxEdge)
        size = size.scaled(maxEdge, maxEdge, Qt::KeepAspectRatio);
    return {std::max(2, size.width() & ~1), std::max(2, size.height() & ~1)};
}

} // namespace

Source::Source() = default;
Source::~Source() = default;

std::unique_ptr<Source> Source::media(const ProjectFormat& format, const QString& path)
{
    std::unique_ptr<Source> s(new Source);
    s->m_format = format;
    Picture::Options o;
    o.format = format;
    o.mode = Picture::Mode::Still;
    s->m_picture = std::make_unique<Picture::Renderer>(Picture::mediaTimeline(path), o);
    s->m_media = true;
    return s;
}

std::unique_ptr<Source> Source::timeline(const ProjectFormat& format, const Timeline& tl)
{
    std::unique_ptr<Source> s(new Source);
    s->m_format = format;
    Timeline picture = tl;
    for (Track& t : picture.audio) t.clips.clear();
    Picture::Options o;
    o.format = format;
    o.mode = Picture::Mode::Still;
    s->m_picture = std::make_unique<Picture::Renderer>(picture, o);
    return s;
}

QImage Source::frame(int frame, int maxEdge)
{
    m_picture->setOutputSize(grabSize(m_format, maxEdge));
    const int decoded = m_picture->stats().layersDecoded;
    const Picture::Frame f = m_picture->render(frame);
    if (f.isNull() || (m_media && m_picture->stats().layersDecoded == decoded)) return {};
    return f.view().convertToFormat(QImage::Format_RGB32);
}

QImage media(const ProjectFormat& format, const QString& path, int frame, int maxEdge)
{
    return Source::media(format, path)->frame(frame, maxEdge);
}

QImage timeline(const ProjectFormat& format, const Timeline& tl, int frame, int maxEdge)
{
    return Source::timeline(format, tl)->frame(frame, maxEdge);
}

QImage contactSheet(const ProjectFormat& format, const Timeline& tl, const QVector<int>& frames, int tileWidth,
                    int columns, const std::function<bool(int)>& progress, int* failed)
{
    if (failed) *failed = -1;
    const auto source = Source::timeline(format, tl);
    QVector<QImage> images;
    QStringList labels;
    for (int i = 0; i < frames.size(); ++i) {
        if (progress && !progress(i)) return {};
        // Decoded large enough for portrait formats too, then every tile exactly tileWidth wide
        const QImage img = source->frame(frames[i], tileWidth * 2);
        if (img.isNull()) {
            if (failed) *failed = frames[i];
            return {};
        }
        images << img.scaledToWidth(tileWidth, Qt::SmoothTransformation);
        labels << Timecode::format(frames[i], format.rate.timebase());
    }
    if (progress) progress(int(frames.size()));
    return ContactSheet::compose(images, labels, columns);
}

} // namespace Snapshot
