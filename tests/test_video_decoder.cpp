// Own video decoder (engine/video, plan 0.11 point 7) against MLT's avformat producer: for many kinds of files the
// same position must give the same pixels and frame properties, in every image format MLT asks for, at the media
// size and scaled (the loader's normalizers), in order, with jumps back and forth, and from several threads at once.
// Hardware decoding (if this machine has a device) must give exactly the software pictures.
// SCHNEIDI_BENCH_DECODER=<media>: pictures per second in order, own decoder (software/hardware) vs MLT.

#include "check.h"

#include "engine/Profiles.h"
#include "engine/video/Producer.h"
#include "engine/video/Reader.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#include <QApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>
#include <random>
#include <thread>
#include <time.h>

namespace {

struct Picture {
    QByteArray image, alpha;
    int width = 0, height = 0;
    mlt_image_format format = mlt_image_none;
    int colorspace = 0, fullRange = 0, progressive = 0;
    bool ok = false;
};

Picture fetch(Mlt::Producer& p, int position, mlt_image_format want, int width, int height, const char* range = nullptr)
{
    Picture out;
    p.seek(position);
    std::unique_ptr<Mlt::Frame> f(p.get_frame());
    if (!f) return out;
    if (range) f->set("consumer.color_range", range);
    f->set("consumer.rescale", "bilinear");
    mlt_image_format fmt = want;
    int w = width, h = height;
    const uint8_t* data = f->get_image(fmt, w, h);
    if (!data) return out;
    const int size = mlt_image_format_size(fmt, w, h, nullptr);
    out.image = QByteArray(reinterpret_cast<const char*>(data), size);
    if (const uint8_t* a = mlt_frame_get_alpha(f->get_frame())) out.alpha = QByteArray(reinterpret_cast<const char*>(a), w * h);
    out.width = w;
    out.height = h;
    out.format = fmt;
    out.colorspace = f->get_int("colorspace");
    out.fullRange = f->get_int("full_range");
    out.progressive = f->get_int("progressive");
    out.ok = true;
    return out;
}

QString describe(const Picture& p)
{
    return QString("%1x%2 %3 cs %4 range %5 prog %6 md5 %7 alpha %8")
        .arg(p.width).arg(p.height).arg(mlt_image_format_name(p.format)).arg(p.colorspace).arg(p.fullRange)
        .arg(p.progressive)
        .arg(QString::fromLatin1(QCryptographicHash::hash(p.image, QCryptographicHash::Md5).toHex().left(8)))
        .arg(p.alpha.isEmpty() ? QString("-") : QString::fromLatin1(QCryptographicHash::hash(p.alpha, QCryptographicHash::Md5).toHex().left(8)));
}

bool same(const Picture& a, const Picture& b)
{
    return a.ok && b.ok && a.width == b.width && a.height == b.height && a.format == b.format
           && a.colorspace == b.colorspace && a.fullRange == b.fullRange && a.progressive == b.progressive
           && a.image == b.image && a.alpha == b.alpha;
}

// Largest difference of two equally sized images (to tell a near miss from garbage)
int maxDiff(const QByteArray& a, const QByteArray& b)
{
    if (a.size() != b.size()) return 9999;
    int m = 0;
    for (qsizetype i = 0; i < a.size(); ++i) m = std::max(m, std::abs(int(uint8_t(a[i])) - int(uint8_t(b[i]))));
    return m;
}

// Mean difference per byte (scaled pictures: the own decoder scales in one pass, MLT converts and then scales)
double meanDiff(const QByteArray& a, const QByteArray& b)
{
    if (a.size() != b.size() || a.isEmpty()) return 9999;
    double sum = 0;
    for (qsizetype i = 0; i < a.size(); ++i) sum += std::abs(int(uint8_t(a[i])) - int(uint8_t(b[i])));
    return sum / double(a.size());
}

// Same picture up to resampling: everything equal but the pixels, which differ little on average. pixels = false:
// only the rest (MLT's way shifted the colors there, testScaledColors checks them against FFmpeg)
bool close(const Picture& a, const Picture& b, bool pixels)
{
    return a.ok && b.ok && a.width == b.width && a.height == b.height && a.format == b.format
           && a.colorspace == b.colorspace && a.fullRange == b.fullRange && a.progressive == b.progressive
           && a.image.size() == b.image.size() && (!pixels || meanDiff(a.image, b.image) < 1.0)
           && a.alpha.size() == b.alpha.size() && (a.alpha.isEmpty() || meanDiff(a.alpha, b.alpha) < 1.0);
}

// FFmpeg's picture 0 of a file, scaled with swscale alone (one pass), as raw bytes
QByteArray ffmpegScaled(const QString& path, int w, int h, const char* pixFmt, const char* matrix)
{
    QProcess p;
    p.start("ffmpeg", {"-v", "error", "-i", path, "-frames:v", "1", "-vf",
                       QString("scale=%1:%2:flags=bilinear+accurate_rnd+full_chroma_int+full_chroma_inp:in_color_matrix=%3,"
                               "format=%4").arg(w).arg(h).arg(matrix, pixFmt),
                       "-f", "rawvideo", "-"});
    p.waitForFinished(60000);
    return p.readAllStandardOutput();
}

// Scaled pictures keep their colors: as FFmpeg scales them. MLT's way (RGBA converted at the source size, then its
// swscale filter) shifted a BT.601 source in a 709 profile by ~8 per channel: cyan 0,255,255 became 22,255,253.
void testScaledColors(const QString& sd601, Mlt::Profile& p1080)
{
    auto own = Video::openProducer(p1080, sd601, {});
    if (!CHECK(own && own->is_valid())) return;
    const Picture rgba = fetch(*own, 0, mlt_image_rgba, p1080.width(), p1080.height());
    const QByteArray ref = ffmpegScaled(sd601, p1080.width(), p1080.height(), "rgba", "bt601");
    if (!CHECK_EQ(rgba.image.size(), ref.size())) return;
    double sum = 0;
    qsizetype n = 0;
    for (qsizetype i = 0; i < ref.size(); i += 4)
        for (int k = 0; k < 3; ++k, ++n) sum += std::abs(int(uint8_t(rgba.image[i + k])) - int(uint8_t(ref[i + k])));
    if (!CHECK(sum / double(n) < 1.0)) std::printf("       scaled RGBA differs from FFmpeg by %.2f\n", sum / double(n));
}

// Preview (fastConvert): YUV -> RGBA without accurate rounding, at the media size and scaled. Same size and tags;
// unscaled within 3 steps of the exact picture, scaled close on average (the planes are scaled before the chroma is
// upsampled: testsrc2's hard color edges differ there, real footage shows no visible difference). YUV stays exact.
void testFastConvert(const QString& path, Mlt::Profile& profile, int width, int height)
{
    auto exact = Video::openProducer(profile, path, {});
    auto fast = Video::openProducer(profile, path, {0, 1, true});
    if (!CHECK(exact && fast)) return;
    const QString name = QFileInfo(path).fileName();
    for (const QSize size : {QSize(width, height), QSize(width / 2, height / 2), QSize(width * 3 / 2, height * 3 / 2)})
        for (int pos : {0, 7, 30}) {
            const Picture a = fetch(*exact, pos, mlt_image_rgba, size.width(), size.height());
            const Picture b = fetch(*fast, pos, mlt_image_rgba, size.width(), size.height());
            const double mean = meanDiff(a.image, b.image);
            const bool unscaled = size == QSize(width, height);
            if (!CHECK(close(a, b, false) && (unscaled ? maxDiff(a.image, b.image) <= 3 : mean < 4.0)))
                std::printf("       %s %dx%d @%d: %s vs %s, mean %.2f\n", qPrintable(name), size.width(), size.height(),
                            pos, qPrintable(describe(a)), qPrintable(describe(b)), mean);
        }
    CHECK(same(fetch(*exact, 3, mlt_image_yuv420p, width, height), fetch(*fast, 3, mlt_image_yuv420p, width, height)));
}

struct Media {
    QString name, path;
    int frames = 0; // frames in the project profile
};

std::unique_ptr<Mlt::Producer> mltProducer(Mlt::Profile& profile, const QString& path)
{
    auto p = std::make_unique<Mlt::Producer>(profile, path.toUtf8().constData());
    p->set("audio_index", -1);
    return p;
}

// Positions: in order, then jumps back, far forward, small steps forward (decode on), the end and past it
QVector<int> positions(int frames)
{
    QVector<int> out;
    for (int i = 0; i < std::min(frames, 40); ++i) out << i;
    out << 5 << 3 << frames / 2 << frames / 2 + 1 << frames / 2 + 7 << frames / 2 + 70 << 1 << frames - 2 << frames - 1
        << frames + 3 << 0;
    // Seeks (back from the end) to just before and onto a key frame of the 2 s GOPs: with B-frames the key frame is
    // decoded before the pictures shown ahead of it (the seek must not land on it)
    for (int k : {46, 47, 48, 49, 50, 51})
        if (k < frames - 1) out << frames - 1 << k;
    return out;
}

// Last project position whose source frame exists (MLT: position -> source frame rounded, see Video::Reader)
int lastPosition(const Media& m, Mlt::Profile& profile)
{
    auto reader = Video::Reader::open(m.path);
    if (!reader) return m.frames - 1;
    const auto end = reader->picture(int64_t(1) << 40); // past the end: the last picture
    const double fps = av_q2d(reader->info().frameRate);
    int last = m.frames - 1;
    while (last > 0 && int64_t(last / profile.fps() * fps + 0.5) > end->position) --last;
    return last;
}

// SCHNEIDI_DEBUG_DECODER=<name>: which MLT picture each position shows (md5 of every position in order)
void debugPositions(const Media& m, Mlt::Profile& profile)
{
    auto mlt = mltProducer(profile, m.path);
    auto own = Video::openProducer(profile, m.path, {});
    for (int pos = 0; pos < m.frames + 5; ++pos) {
        const Picture a = fetch(*mlt, pos, mlt_image_yuv420p, 0, 0), b = fetch(*own, pos, mlt_image_yuv420p, 0, 0);
        std::printf("%4d MLT %s | own %s\n", pos, qPrintable(describe(a)), qPrintable(describe(b)));
    }
}

void compare(const Media& m, Mlt::Profile& profile, const QVector<mlt_image_format>& formats, bool scaled = false,
             const char* range = nullptr)
{
    auto mlt = mltProducer(profile, m.path);
    auto own = Video::openProducer(profile, m.path, {});
    if (!CHECK(own && own->is_valid())) {
        std::printf("       %s: own decoder refused it\n", qPrintable(m.name));
        return;
    }
    CHECK_EQ(own->get_length(), mlt->get_length());
    for (const char* prop : {"width", "height", "video_index", "aspect_ratio"}) {
        if (!Check::equal(QByteArray(own->get(prop)), QByteArray(mlt->get(prop)),
                          qPrintable(QString("%1 %2").arg(m.name, prop)), __FILE__, __LINE__))
            break;
    }
    const int w = scaled ? profile.width() : 0, h = scaled ? profile.height() : 0;
    // Positions past the stream's last picture: MLT shows the last picture it delivered with the source's color tag
    // although it converted it (colors converted twice); the own decoder shows the stream's last picture -> compared
    // with that instead
    const int last = lastPosition(m, profile);
    for (mlt_image_format fmt : formats) {
        int failures = 0;
        // Fresh producers per format: MLT's image cache answers a request for mlt_image_none with the format cached
        // for that position before
        mlt = mltProducer(profile, m.path);
        own = Video::openProducer(profile, m.path, {});
        for (int pos : positions(m.frames)) {
            if (pos > last) {
                const Picture end = fetch(*own, last, fmt, w, h, range), b = fetch(*own, pos, fmt, w, h, range);
                if (!same(end, b) && ++failures <= 3)
                    Check::report(false, qPrintable(QString("%1 %2 past the end pos %3").arg(m.name, mlt_image_format_name(fmt)).arg(pos)),
                                  __FILE__, __LINE__);
                continue;
            }
            // Bit-exact to MLT when scaling like MLT (converted at the source size, then MLT's swscale filter)
            const Picture a = fetch(*mlt, pos, fmt, w, h, range);
            qputenv("SCHNEIDI_SCALE_LATE", "1");
            const Picture b = fetch(*own, pos, fmt, w, h, range);
            qunsetenv("SCHNEIDI_SCALE_LATE");
            if (!same(a, b) && ++failures <= 3) {
                Check::report(false, qPrintable(QString("%1 %2 %3 pos %4").arg(m.name, mlt_image_format_name(fmt))
                                                    .arg(scaled ? "scaled" : "native").arg(pos)),
                              __FILE__, __LINE__,
                              QString("MLT: %1\n       own: %2\n       max diff %3")
                                  .arg(describe(a), describe(b)).arg(maxDiff(a.image, b.image)));
            }
            // The default: a picture of another size converted and scaled in one swscale pass -> close to that
            const Picture c = fetch(*own, pos, fmt, w, h, range);
            const bool shifted = c.format == mlt_image_rgba && c.colorspace != profile.colorspace();
            if (!close(b, c, !shifted) && ++failures <= 3) {
                Check::report(false, qPrintable(QString("%1 %2 %3 pos %4 in one pass").arg(m.name, mlt_image_format_name(fmt))
                                                    .arg(scaled ? "scaled" : "native").arg(pos)),
                              __FILE__, __LINE__,
                              QString("late: %1\n       one pass: %2\n       mean diff %3")
                                  .arg(describe(b), describe(c)).arg(meanDiff(b.image, c.image), 0, 'f', 3));
            }
        }
        Check::report(failures == 0, qPrintable(QString("%1 %2 %3").arg(m.name, mlt_image_format_name(fmt))
                                                     .arg(scaled ? "scaled" : "native")),
                      __FILE__, __LINE__);
    }
    // MLT's meta.media.* after the first frame (the graph and sourceAspect read them)
    for (const char* prop : {"meta.media.width", "meta.media.height", "meta.media.color_range",
                             "meta.media.progressive", "meta.media.frame_rate_num", "meta.media.frame_rate_den",
                             "meta.media.colorspace"})
        Check::equal(QByteArray(own->get(prop)), QByteArray(mlt->get(prop)),
                     qPrintable(QString("%1 %2").arg(m.name, prop)), __FILE__, __LINE__);
}

// Several threads fetch the same producer at once, out of order (export workers): same pictures as one by one
void testThreads(const Media& m, Mlt::Profile& profile)
{
    auto own = Video::openProducer(profile, m.path, {0, 4});
    QVector<QByteArray> reference(m.frames);
    {
        auto single = Video::openProducer(profile, m.path, {});
        for (int i = 0; i < m.frames; ++i) reference[i] = fetch(*single, i, mlt_image_yuv420p, 0, 0).image;
    }
    std::atomic<int> next{0}, wrong{0};
    std::mutex lock; // seek + get_frame on one producer must not interleave (MLT's consumer does it in one thread)
    auto work = [&] {
        std::mt19937 rng(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        for (int i; (i = next++) < m.frames;) {
            const int pos = std::min(m.frames - 1, std::max(0, i + int(rng() % 5) - 2)); // a little out of order
            std::unique_ptr<Mlt::Frame> f;
            {
                std::lock_guard g(lock);
                own->seek(pos);
                f.reset(own->get_frame());
            }
            mlt_image_format fmt = mlt_image_yuv420p;
            int w = 0, h = 0;
            const uint8_t* data = f->get_image(fmt, w, h);
            const int size = mlt_image_format_size(fmt, w, h, nullptr);
            if (!data || QByteArray(reinterpret_cast<const char*>(data), size) != reference[pos]) ++wrong;
        }
    };
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) threads.emplace_back(work);
    for (auto& t : threads) t.join();
    CHECK_EQ(wrong.load(), 0);
}

// Hardware pictures = software pictures (H.264/HEVC decoding is bit exact)
void testHardware(const QString& name, const QString& path, int frames)
{
    Video::Reader::Options hw, sw;
    sw.hardware = false;
    auto a = Video::Reader::open(path, sw);
    auto b = Video::Reader::open(path, hw);
    if (!CHECK(a && b)) return;
    // A picture first: the decoder decides on the card at the first frame
    if (!b->picture(0) || !b->stats().hardware) {
        std::printf("       %s: no hardware decoding here (%d failures)\n", qPrintable(name), b->stats().hardwareFailures);
        return;
    }
    int failures = 0;
    for (int pos : positions(frames)) {
        const auto pa = a->picture(pos), pb = b->picture(pos);
        if (!pa || !pb || pa->position != pb->position || pa->frame->format != pb->frame->format) {
            ++failures;
            continue;
        }
        const auto fmt = AVPixelFormat(pa->frame->format);
        const int planes = av_pix_fmt_count_planes(fmt);
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
        for (int p = 0; p < planes; ++p) {
            const int h = p ? AV_CEIL_RSHIFT(pa->frame->height, desc->log2_chroma_h) : pa->frame->height;
            const int bytes = av_image_get_linesize(fmt, pa->frame->width, p);
            for (int y = 0; y < h; ++y)
                if (std::memcmp(pa->frame->data[p] + y * pa->frame->linesize[p],
                                pb->frame->data[p] + y * pb->frame->linesize[p], bytes) != 0) {
                    ++failures;
                    p = planes;
                    break;
                }
        }
    }
    Check::report(failures == 0 && b->stats().hardware, qPrintable(name + " hardware = software"), __FILE__, __LINE__,
                  QString("%1 differing pictures").arg(failures));
}

// Unused readers close their decoder after a pause (all but the 4 most recently used) and open again on demand
void testHibernate(const QString& path)
{
    std::vector<std::unique_ptr<Video::Reader>> readers;
    QVector<QByteArray> first;
    for (int i = 0; i < 6; ++i) {
        readers.push_back(Video::Reader::open(path));
        const auto p = readers.back()->picture(10 + i);
        first << (p ? QByteArray(reinterpret_cast<const char*>(p->frame->data[0]), p->frame->linesize[0] * 8) : QByteArray());
        QThread::msleep(5); // distinct last-use times
    }
    QThread::msleep(1500); // SCHNEIDI_DECODER_IDLE_MS = 300 (main), decoder threads check every 300 ms
    int asleep = 0;
    for (auto& r : readers) asleep += r->stats().hibernations;
    CHECK_EQ(asleep, 2); // the two least recently used
    CHECK_EQ(readers[0]->stats().hibernations, 1);
    CHECK_EQ(readers[5]->stats().hibernations, 0);
    for (int i = 0; i < 6; ++i) {
        const auto p = readers[i]->picture(10 + i);
        CHECK(p && QByteArray(reinterpret_cast<const char*>(p->frame->data[0]), p->frame->linesize[0] * 8) == first[i]);
    }
}

void bench(const QString& path, Mlt::Profile& profile)
{
    const int n = qEnvironmentVariableIntValue("SCHNEIDI_BENCH_FRAMES") > 0 ? qEnvironmentVariableIntValue("SCHNEIDI_BENCH_FRAMES") : 600;
    auto cpu = [] {
        timespec ts{};
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
        return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
    };
    auto run = [&](const char* label, Mlt::Producer& p) {
        fetch(p, 0, mlt_image_yuv420p, 0, 0); // opening and the first seek do not count
        QElapsedTimer t;
        t.start();
        const double c0 = cpu();
        for (int i = 1; i <= n; ++i) fetch(p, i, mlt_image_yuv420p, 0, 0);
        const double ms = std::max<qint64>(1, t.elapsed());
        std::printf("%-22s %7.1f Bilder/s, CPU %5.1f ms je Bild (%.1f Kerne)\n", label, n * 1000.0 / ms,
                    (cpu() - c0) / n, (cpu() - c0) / ms);
    };
    {
        auto p = mltProducer(profile, path);
        run("MLT", *p);
    }
    qputenv("SCHNEIDI_HWDEC", "off");
    {
        auto p = Video::openProducer(profile, path, {});
        run("eigen (Software)", *p);
    }
    qunsetenv("SCHNEIDI_HWDEC");
    {
        auto p = Video::openProducer(profile, path, {});
        run("eigen (Hardware?)", *p);
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    qputenv("SCHNEIDI_DECODER_IDLE_MS", "300");
    QApplication app(argc, argv);
    Check::initApp("video_decoder");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    Check::initMlt();

    const ProjectFormat fmt25{640, 360, {25, 1}};
    if (const QString b = qEnvironmentVariable("SCHNEIDI_BENCH_DECODER"); !b.isEmpty()) {
        const ClipFormat cf = detectClipFormat(b);
        auto profile = makeProfile({cf.width, cf.height, cf.suggested});
        bench(b, *profile);
        return 0;
    }

    QTemporaryDir dir;
    auto file = [&](const char* name) { return dir.filePath(name); };
    const QString src2 = "testsrc2=size=640x360:rate=25";
    QVector<Media> media;
    auto add = [&](const char* name, const QString& path, int frames) {
        if (!path.isEmpty()) media << Media{name, path, frames};
        else Check::report(false, name, __FILE__, __LINE__, "ffmpeg konnte die Datei nicht erzeugen");
    };
    // H.264 with B-frames and a long GOP (seeks back to key frames, preseek)
    add("h264", Check::makeMedia(file("h264.mp4"), {"-f", "lavfi", "-i", src2, "-t", "4", "-c:v", "libx264", "-g", "50",
                                                     "-bf", "3", "-pix_fmt", "yuv420p"}), 100);
    // 30 fps in a 25 fps project (frames skipped) and 12.5 fps (frames repeated)
    add("h264-30fps", Check::makeMedia(file("h264-30.mp4"), {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30",
                                                             "-t", "4", "-c:v", "libx264", "-g", "60", "-pix_fmt", "yuv420p"}), 100);
    add("h264-12.5fps", Check::makeMedia(file("h264-12.mp4"), {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=12.5",
                                                               "-t", "4", "-c:v", "libx264", "-pix_fmt", "yuv420p"}), 100);
    // Matroska with NTSC rate (1 ms timestamps)
    add("mkv-29.97", Check::makeMedia(file("ntsc.mkv"), {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30000/1001",
                                                         "-t", "4", "-c:v", "libx264", "-pix_fmt", "yuv420p"}), 100);
    // Full range (MJPEG, yuvj420p)
    add("mjpeg-full", Check::makeMedia(file("mjpeg.avi"), {"-f", "lavfi", "-i", src2, "-t", "2", "-c:v", "mjpeg",
                                                           "-pix_fmt", "yuvj420p", "-q:v", "3"}), 50);
    // 4:2:2 10 bit intra (ProRes): MLT's sliced conversion to packed 4:2:2
    add("prores", Check::makeMedia(file("prores.mov"), {"-f", "lavfi", "-i", src2, "-t", "2", "-c:v", "prores_ks",
                                                        "-profile:v", "2"}), 50);
    // 10-bit HEVC
    add("hevc-10bit", Check::makeMedia(file("hevc10.mp4"), {"-f", "lavfi", "-i", src2, "-t", "2", "-c:v", "libx265",
                                                            "-pix_fmt", "yuv420p10le", "-x265-params", "log-level=error"}), 50);
    // Phone portrait: landscape pictures with a 90° rotation in the metadata
    add("rotated-90", Check::makeMedia(file("rotated.mp4"), {"-display_rotation", "-90", "-i", file("h264.mp4"),
                                                             "-c", "copy"}), 100);
    add("rotated-180", Check::makeMedia(file("rotated180.mp4"), {"-display_rotation", "180", "-i", file("h264.mp4"),
                                                                 "-c", "copy"}), 100);
    // Interlaced (top field first)
    add("interlaced", Check::makeMedia(file("interlaced.mp4"), {"-f", "lavfi", "-i", src2, "-t", "2", "-c:v", "libx264",
                                                                "-flags", "+ildct+ilme", "-x264opts", "tff=1",
                                                                "-pix_fmt", "yuv420p", "-field_order", "tt"}), 50);
    // SD with BT.601 tag
    add("sd-601", Check::makeMedia(file("sd.mp4"), {"-f", "lavfi", "-i", "testsrc2=size=720x576:rate=25", "-t", "2",
                                                    "-c:v", "libx264", "-pix_fmt", "yuv420p", "-colorspace", "bt470bg",
                                                    "-color_primaries", "bt470bg", "-color_trc", "gamma28"}), 50);
    // RGBA (PNG in MOV) and VP9 with alpha
    add("png-rgba", Check::makeMedia(file("rgba.mov"), {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25,format=rgba",
                                                        "-t", "1", "-c:v", "png"}), 25);
    add("vp9-alpha", Check::makeMedia(file("alpha.webm"), {"-f", "lavfi", "-i",
                                                           "testsrc2=size=320x180:rate=25,format=yuva420p", "-t", "1",
                                                           "-c:v", "libvpx-vp9", "-auto-alt-ref", "0"}), 25);

    auto p25 = makeProfile(fmt25);
    if (const QString dbg = qEnvironmentVariable("SCHNEIDI_DEBUG_DECODER"); !dbg.isEmpty()) {
        for (const Media& m : media)
            if (m.name == dbg) debugPositions(m, *p25);
        return 0;
    }
    const QVector<mlt_image_format> all{mlt_image_yuv420p, mlt_image_yuv422, mlt_image_rgba, mlt_image_none};
    for (const Media& m : media) {
        compare(m, *p25, all);
        compare(m, *p25, {mlt_image_rgba, mlt_image_yuv422}, true); // through swscale/resize
    }
    // Consumer asking for full range (MLT keeps a full-range source full)
    for (const Media& m : media)
        if (m.name == "mjpeg-full" || m.name == "h264") compare(m, *p25, {mlt_image_yuv420p}, false, "full");
    // Another project size and rate
    {
        auto p1080 = makeProfile(ProjectFormat{1920, 1080, {50, 1}});
        for (const Media& m : media)
            if (m.name == "h264" || m.name == "rotated-90") compare(Media{m.name + "@1080p50", m.path, 200}, *p1080,
                                                                     {mlt_image_rgba, mlt_image_yuv420p}, true);
    }
    {
        auto p1080 = makeProfile(ProjectFormat{1920, 1080, {50, 1}});
        testScaledColors(file("h264.mp4"), *p1080);
    }
    testFastConvert(file("h264.mp4"), *p25, 640, 360);
    testFastConvert(file("mjpeg.avi"), *p25, 640, 360);
    testFastConvert(file("sd.mp4"), *p25, 720, 576);
    testThreads(media.value(0), *p25);
    testHibernate(media.value(0).path);

    // Through the factory: a BT.601 source moves to a profile of its own, like MLT's producer
    {
        ProducerFactory factory(*p25);
        auto p = factory.open(file("sd.mp4"));
        CHECK_EQ(QByteArray(p->get("mlt_service")), QByteArray("schneidi_video"));
        CHECK_EQ(mlt_service_profile(MLT_PRODUCER_SERVICE(p->get_producer()))->colorspace, 601);
        auto still = factory.open(file("h264.mp4") + ".png"); // missing file: MLT's error producer path
        CHECK(QByteArray(still->get("mlt_service")) != "schneidi_video");
    }

    // Hardware decoding: 720p H.264 and 10-bit HEVC
    const QString hd = Check::makeMedia(file("hd.mp4"), {"-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30", "-t",
                                                         "3", "-c:v", "libx264", "-g", "60", "-bf", "2", "-pix_fmt",
                                                         "yuv420p"});
    const QString hd10 = Check::makeMedia(file("hd10.mp4"), {"-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30",
                                                             "-t", "2", "-c:v", "libx265", "-pix_fmt", "yuv420p10le",
                                                             "-x265-params", "log-level=error"});
    if (!hd.isEmpty()) testHardware("h264 720p", hd, 90);
    if (!hd10.isEmpty()) testHardware("hevc 10 bit 720p", hd10, 60);
    // And through MLT's graph with hardware: same as MLT's producer
    if (!hd.isEmpty()) {
        auto p720 = makeProfile(ProjectFormat{1280, 720, {30, 1}});
        compare(Media{"h264 720p (hw)", hd, 90}, *p720, {mlt_image_rgba, mlt_image_yuv420p});
    }
    return Check::result();
}
