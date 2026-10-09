#include "engine/Snapshot.h"

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

QImage media(const ProjectFormat& format, const QString& path, int frame, int maxEdge)
{
    auto profile = makeProfile(format);
    ProducerFactory factory(*profile);
    const std::unique_ptr<Mlt::Producer> producer = factory.open(path);
    return producer ? grab(*producer, format, frame, maxEdge) : QImage();
}

QImage timeline(const ProjectFormat& format, const Timeline& tl, int frame, int maxEdge)
{
    auto profile = makeProfile(format);
    TimelineBuilder builder(*profile);
    const std::unique_ptr<Mlt::Tractor> tractor = builder.build(tl);
    return tractor ? grab(*tractor, format, frame, maxEdge) : QImage();
}

} // namespace Snapshot
