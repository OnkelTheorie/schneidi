#include "engine/ScrubFrames.h"

#include "engine/Background.h"
#include "engine/video/Reader.h"

#include <QThread>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

constexpr int kCacheKB = 24 * 1024; // ~500 pictures of 144×81
constexpr size_t kMaxJobs = 2 * ScrubFrames::kSlots;

// Upright software picture of the reader -> RGB image fitted into `box`, square pixels
QImage toImage(const Video::Picture& pic, const Video::Info& info, const QSize& box)
{
    const AVFrame* f = pic.frame;
    if (!f || f->width <= 0 || f->height <= 0) return {};
    // Turned by a quarter (phone video): the stored pixel aspect is across the other axis
    const bool turned = std::lround(info.rotation / 90.0) % 2 != 0;
    const double sar = info.sar.num > 0 && info.sar.den > 0
                           ? (turned ? double(info.sar.den) / info.sar.num : double(info.sar.num) / info.sar.den)
                           : 1.0;
    const QSize shown = QSize(std::max(1, int(std::lround(f->width * sar))), f->height).scaled(box, Qt::KeepAspectRatio);
    const int w = std::max(2, shown.width()), h = std::max(2, shown.height());
    SwsContext* ctx = sws_getContext(f->width, f->height, AVPixelFormat(f->format), w, h, AV_PIX_FMT_RGB32,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!ctx) return {};
    // Colors of the stream (matrix, video or full range); swscale's RGB side is full range
    const int space = info.colorspace == 709 ? SWS_CS_ITU709 : info.colorspace == 2020 ? SWS_CS_BT2020 : SWS_CS_ITU601;
    sws_setColorspaceDetails(ctx, sws_getCoefficients(space), info.fullRange ? 1 : 0, sws_getCoefficients(SWS_CS_DEFAULT),
                             1, 0, 1 << 16, 1 << 16);
    QImage img(w, h, QImage::Format_RGB32); // AV_PIX_FMT_RGB32 = native 0xAARRGGBB, like QImage
    uint8_t* dst[4] = {img.bits(), nullptr, nullptr, nullptr};
    int dstStride[4] = {int(img.bytesPerLine()), 0, 0, 0};
    sws_scale(ctx, f->data, f->linesize, 0, f->height, dst, dstStride);
    sws_freeContext(ctx);
    return img;
}

} // namespace

ScrubFrames::ScrubFrames(const QSize& size, QObject* parent) : QObject(parent), m_size(size)
{
    m_pictures.setMaxCost(kCacheKB);
    m_thread = QThread::create([this] {
        Background::lowerThisThread(Background::Level::Low); // also the reader's decoder thread started from here
        run();
    });
    m_thread->start(QThread::LowPriority);
}

ScrubFrames::~ScrubFrames()
{
    {
        QMutexLocker lock(&m_mutex);
        m_quit = true;
        m_jobs.clear();
    }
    m_wake.wakeAll();
    m_thread->wait();
    delete m_thread;
}

QString ScrubFrames::key(const QString& path, int slot)
{
    return path + '#' + QString::number(slot);
}

int ScrubFrames::slotAt(double fraction)
{
    return std::clamp(int(fraction * kSlots), 0, kSlots - 1);
}

QImage ScrubFrames::picture(const QString& path, int slot)
{
    slot = std::clamp(slot, 0, kSlots - 1);
    QMutexLocker lock(&m_mutex);
    if (const QImage* img = m_pictures.object(key(path, slot))) return *img;
    if (m_failed.contains(path)) return {};
    const QString k = key(path, slot);
    if (!m_pending.contains(k)) {
        m_pending.insert(k);
        m_jobs.push_front({path, slot});
        while (m_jobs.size() > kMaxJobs) { // the mouse has moved on long ago
            m_pending.remove(key(m_jobs.back().path, m_jobs.back().slot));
            m_jobs.pop_back();
        }
        m_wake.wakeOne();
    }
    // Until it is there: the nearest picture of the file
    for (int d = 1; d < kSlots; ++d)
        for (const int s : {slot - d, slot + d})
            if (s >= 0 && s < kSlots)
                if (const QImage* img = m_pictures.object(key(path, s))) return *img;
    return {};
}

bool ScrubFrames::has(const QString& path, int slot)
{
    QMutexLocker lock(&m_mutex);
    return m_pictures.contains(key(path, slot));
}

bool ScrubFrames::failed(const QString& path)
{
    QMutexLocker lock(&m_mutex);
    return m_failed.contains(path);
}

int ScrubFrames::decoded() const
{
    QMutexLocker lock(&m_mutex);
    return m_decoded;
}

void ScrubFrames::run()
{
    // The file under the mouse stays open: its next slots only seek
    std::unique_ptr<Video::Reader> reader;
    QString open;
    double fps = 0, seconds = 0;
    for (;;) {
        Job job;
        {
            QMutexLocker lock(&m_mutex);
            while (!m_quit && m_jobs.empty()) m_wake.wait(&m_mutex);
            if (m_quit) return;
            job = m_jobs.front();
            m_jobs.pop_front();
        }
        Background::yieldToForeground(); // playback and opening files first
        if (job.path != open) {
            Video::Reader::Options options;
            options.threads = 2;
            options.hardware = false; // one picture now and then: setting up the graphics card costs more
            options.ahead = 0;
            options.behind = 0;
            reader = Video::Reader::open(job.path, options);
            open = job.path;
            fps = reader ? av_q2d(reader->info().frameRate) : 0;
            seconds = reader ? reader->info().duration / double(AV_TIME_BASE) : 0;
            if (fps <= 0) fps = 25;
        }
        QImage img;
        if (reader && seconds > 0) {
            const int64_t position = int64_t((job.slot + 0.5) / kSlots * seconds * fps);
            if (const auto pic = reader->picture(position)) img = toImage(*pic, reader->info(), m_size);
        }
        {
            QMutexLocker lock(&m_mutex);
            m_pending.remove(key(job.path, job.slot));
            if (img.isNull()) {
                // Nothing to show for the whole file: no more requests for it
                m_failed.insert(job.path);
                for (auto it = m_jobs.begin(); it != m_jobs.end();) {
                    if (it->path == job.path) {
                        m_pending.remove(key(it->path, it->slot));
                        it = m_jobs.erase(it);
                    } else {
                        ++it;
                    }
                }
            } else {
                ++m_decoded;
                m_pictures.insert(key(job.path, job.slot), new QImage(img), int(img.sizeInBytes() / 1024) + 1);
            }
        }
        emit ready(job.path, job.slot);
    }
}
