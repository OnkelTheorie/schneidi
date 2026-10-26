#include "engine/Snapshot.h"

#include "core/ContactSheet.h"
#include "core/Timecode.h"

#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QPainter>

#include <cstring>
#include <memory>

namespace Snapshot {

namespace {

QImage grab(Mlt::Producer& producer, const ProjectFormat& format, int frame, int maxEdge)
{
    if (!producer.is_valid()) return {};
    producer.seek(frame);
    std::unique_ptr<Mlt::Frame> f(producer.get_frame());
    if (!f) return {};
    QSize size = format.size();
    if (maxEdge > 0 && std::max(size.width(), size.height()) > maxEdge)
        size = size.scaled(maxEdge, maxEdge, Qt::KeepAspectRatio);
    mlt_image_format fmt = mlt_image_rgba;
    int w = std::max(2, size.width() & ~1), h = std::max(2, size.height() & ~1);
    const uint8_t* data = f->get_image(fmt, w, h);
    if (!data || w <= 0 || h <= 0) return {};
    QImage img(w, h, QImage::Format_RGBA8888);
    std::memcpy(img.bits(), data, size_t(w) * h * 4);
    QImage out(img.size(), QImage::Format_RGB32);
    out.fill(Qt::black);
    QPainter painter(&out);
    painter.drawImage(0, 0, img);
    return out;
}

} // namespace

Source::Source() = default;
Source::~Source()
{
    m_producer.reset(); // before the builder/factory that own its parts
}

std::unique_ptr<Source> Source::media(const ProjectFormat& format, const QString& path)
{
    std::unique_ptr<Source> s(new Source);
    s->m_format = format;
    s->m_profile = makeProfile(format);
    s->m_factory = std::make_unique<ProducerFactory>(*s->m_profile);
    s->m_producer = s->m_factory->open(path);
    if (s->m_producer) s->m_producer->set("audio_index", -1);
    return s;
}

std::unique_ptr<Source> Source::timeline(const ProjectFormat& format, const Timeline& tl)
{
    std::unique_ptr<Source> s(new Source);
    s->m_format = format;
    s->m_profile = makeProfile(format);
    s->m_builder = std::make_unique<TimelineBuilder>(*s->m_profile);
    Timeline picture = tl;
    for (Track& t : picture.audio) t.clips.clear();
    s->m_producer = s->m_builder->build(picture);
    return s;
}

QImage Source::frame(int frame, int maxEdge)
{
    return m_producer ? grab(*m_producer, m_format, frame, maxEdge) : QImage();
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
