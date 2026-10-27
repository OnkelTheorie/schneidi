#include "engine/video/Still.h"

#include <Mlt.h>
#include <QImage>
#include <QImageReader>
#include <cstring>
#include <mutex>

namespace Still {

namespace {

struct Data {
    QString path;
    QSize size; // as shown (EXIF orientation applied)
    std::mutex mutex;
    QImage full; // decoded on first use, RGBA8888 (straight alpha)
    bool failed = false;

    // The picture; null = unreadable. Implicitly shared copy: usable unlocked.
    QImage picture()
    {
        std::lock_guard lock(mutex);
        if (full.isNull() && !failed) {
            QImageReader reader(path);
            reader.setAutoTransform(true);
            QImage img = reader.read();
            failed = img.isNull();
            if (!failed) full = img.convertToFormat(QImage::Format_RGBA8888);
        }
        return full;
    }
};

int getImage(mlt_frame frame, uint8_t** buffer, mlt_image_format* format, int* width, int* height, int)
{
    auto producer = static_cast<mlt_producer>(mlt_frame_pop_service(frame));
    auto* d = static_cast<Data*>(producer->child);
    mlt_properties fp = MLT_FRAME_PROPERTIES(frame);
    // At its own size: the loader's rescaler scales it like any picture (scaling here with Qt instead gave other
    // pixels than MLT's qimage, tests/test_compose)
    const QImage img = d->picture();
    if (img.isNull()) return 1;
    const int iw = img.width(), ih = img.height();
    const int size = mlt_image_format_size(mlt_image_rgba, iw, ih, nullptr);
    auto* out = static_cast<uint8_t*>(mlt_pool_alloc(size));
    if (!out) return 1;
    for (int y = 0; y < ih; ++y) std::memcpy(out + size_t(y) * iw * 4, img.constScanLine(y), size_t(iw) * 4);
    mlt_frame_set_image(frame, out, size, mlt_pool_release);
    mlt_properties_set_int(fp, "format", mlt_image_rgba);
    mlt_properties_set_int(fp, "progressive", 1);
    *buffer = out;
    *format = mlt_image_rgba;
    *width = iw;
    *height = ih;
    return 0;
}

int getFrame(mlt_producer self, mlt_frame_ptr frame, int)
{
    auto* d = static_cast<Data*>(self->child);
    *frame = mlt_frame_init(MLT_PRODUCER_SERVICE(self));
    if (!*frame) return 1;
    mlt_properties fp = MLT_FRAME_PROPERTIES(*frame);
    mlt_frame_set_position(*frame, mlt_producer_position(self));
    mlt_properties_set_int(fp, "width", d->size.width());
    mlt_properties_set_int(fp, "height", d->size.height());
    mlt_properties_set_double(fp, "aspect_ratio", 1.0);
    mlt_properties_set_int(fp, "format", mlt_image_rgba);
    mlt_properties_set_int(fp, "progressive", 1);
    mlt_properties_set_int(fp, "meta.media.width", d->size.width());
    mlt_properties_set_int(fp, "meta.media.height", d->size.height());
    mlt_frame_push_service(*frame, self);
    mlt_frame_push_get_image(*frame, getImage);
    mlt_producer_prepare_next(self);
    return 0;
}

void closeProducer(mlt_producer self)
{
    delete static_cast<Data*>(self->child);
    self->child = nullptr;
    self->close = nullptr;
    mlt_producer_close(self);
    free(self);
}

// The raw producer (reference held by the caller), nullptr = no picture
mlt_producer create(mlt_profile profile, const QString& path)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (!reader.canRead()) return nullptr;
    QSize size = reader.size();
    if (size.isValid() && (reader.transformation() & QImageIOHandler::TransformationRotate90)) size.transpose();
    if (!size.isValid()) { // the format does not tell without decoding
        const QImage img = reader.read();
        if (img.isNull()) return nullptr;
        size = img.size();
    }
    if (size.isEmpty()) return nullptr;
    mlt_producer raw = mlt_producer_new(profile);
    if (!raw) return nullptr;
    auto* d = new Data;
    d->path = path;
    d->size = size;
    raw->child = d;
    raw->get_frame = getFrame;
    raw->close = reinterpret_cast<mlt_destructor>(closeProducer);
    mlt_properties p = MLT_PRODUCER_PROPERTIES(raw);
    const QByteArray resource = path.toUtf8();
    mlt_properties_set(p, "resource", resource.constData());
    mlt_properties_set(p, "mlt_service", kService);
    // Like a still: long enough for any clip (TimelineBuilder sets the clip's length itself)
    const int length = 15000;
    mlt_properties_set_int(p, "length", length);
    mlt_properties_set_int(p, "out", length - 1);
    mlt_properties_set_int(p, "in", 0);
    mlt_properties_set_int(p, "seekable", 1);
    mlt_properties_set_int(p, "progressive", 1);
    mlt_properties_set_int(p, "video_index", 0);
    mlt_properties_set_int(p, "audio_index", -1);
    mlt_properties_set_int(p, "width", size.width());
    mlt_properties_set_int(p, "height", size.height());
    mlt_properties_set_int(p, "meta.media.width", size.width());
    mlt_properties_set_int(p, "meta.media.height", size.height());
    mlt_properties_set_int(p, "meta.media.nb_streams", 1);
    mlt_properties_set(p, "meta.media.0.stream.type", "video");
    mlt_properties_set_int(p, "meta.media.0.codec.width", size.width());
    mlt_properties_set_int(p, "meta.media.0.codec.height", size.height());
    mlt_properties_set_double(p, "aspect_ratio", 1.0);
    mlt_properties_set_int(p, "meta.media.sample_aspect_num", 1);
    mlt_properties_set_int(p, "meta.media.sample_aspect_den", 1);
    return raw;
}

// Factory entry: arg = file path
void* createService(mlt_profile profile, mlt_service_type, const char*, const void* arg)
{
    if (!arg) return nullptr;
    return create(profile, QString::fromUtf8(static_cast<const char*>(arg)));
}

} // namespace

bool isStillService(const char* service)
{
    return service && (std::strcmp(service, kService) == 0 || std::strcmp(service, "qimage") == 0
                       || std::strcmp(service, "pixbuf") == 0);
}

void registerService()
{
    mlt_repository repo = mlt_factory_repository();
    if (!repo) return;
    mlt_repository_register(repo, mlt_service_producer_type, kService, createService);
    // Without MLT's Qt module (denied, Bundle::prepareMltEnvironment) the loader's "qimage" is this one
    if (!mlt_properties_get_data(mlt_repository_producers(repo), "qimage", nullptr))
        mlt_repository_register(repo, mlt_service_producer_type, "qimage", createService);
}

} // namespace Still
