// SPDX-License-Identifier: AGPL-3.0-only
// Own video decoder (engine/video/Reader) on many kinds of files, without a second decoder to compare against: the
// pictures read in order are the reference, and every other way of asking must give the same pictures -
// - jumps back and forth, small steps, the end and past it (seeks to key frames, B-frames, preseek)
// - several threads asking one reader a little out of order (export workers, read-ahead)
// - hardware decoding (if this machine has a device) = software decoding bit for bit (H.264, 10-bit HEVC)
// - unused readers close their decoder after a pause and give the same pictures when asked again
// plus the frame properties the file states (size, rotation, colour standard and range, interlacing, alpha).
// Replaces the comparison with MLT's producer (test_video_decoder, until 0.15).

#include "check.h"

#include "engine/ClipFormat.h"
#include "engine/MediaProbe.h"
#include "engine/video/Reader.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <cstring>
#include <random>
#include <thread>

namespace {

struct Media {
    QString name, path;
    int frames = 0; // stream frames
};

// The planes of a picture as bytes (only the visible part of every row)
QByteArray bytesOf(const AVFrame* f)
{
    QByteArray out;
    const auto fmt = AVPixelFormat(f->format);
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    const int planes = av_pix_fmt_count_planes(fmt);
    for (int p = 0; p < planes; ++p) {
        const bool chroma = (p == 1 || p == 2) && !(desc->flags & AV_PIX_FMT_FLAG_RGB);
        const int h = chroma ? AV_CEIL_RSHIFT(f->height, desc->log2_chroma_h) : f->height;
        const int bytes = av_image_get_linesize(fmt, f->width, p);
        for (int y = 0; y < h; ++y) out.append(reinterpret_cast<const char*>(f->data[p] + y * f->linesize[p]), bytes);
    }
    return out;
}

// Positions: in order, then jumps back, far forward, small steps forward (decode on), the end and past it
QVector<int> positions(int frames)
{
    QVector<int> out;
    for (int i = 0; i < std::min(frames, 40); ++i) out << i;
    out << 5 << 3 << frames / 2 << frames / 2 + 1 << frames / 2 + 7 << frames - 2 << 1 << frames - 1 << frames + 3 << 0;
    // Seeks (back from the end) to just before and onto a key frame of the 2 s GOPs: with B-frames the key frame is
    // decoded before the pictures shown ahead of it (the seek must not land on it)
    for (int k : {46, 47, 48, 49, 50, 51})
        if (k < frames - 1) out << frames - 1 << k;
    // Into the middle of a GOP and into the last one, then backwards picture by picture (a reversed clip)
    out << std::max(0, frames / 2 - 20) << std::max(0, frames - 15) << frames / 2 + 20;
    for (int i = frames - 1; i >= frames - 12 && i >= 0; --i) out << i;
    return out.toList().toVector();
}

// In order from a fresh software reader: the reference
QVector<QByteArray> reference(const Media& m)
{
    Video::Reader::Options o;
    o.hardware = false;
    auto r = Video::Reader::open(m.path, o);
    QVector<QByteArray> out;
    if (!r) return out;
    for (int i = 0; i < m.frames; ++i) {
        const auto p = r->picture(i);
        if (!p || p->position != i) break;
        out << bytesOf(p->frame);
    }
    return out;
}

void testSeeks(const Media& m, const QVector<QByteArray>& ref)
{
    Video::Reader::Options o;
    o.hardware = false;
    auto r = Video::Reader::open(m.path, o);
    if (!CHECK(r)) return;
    int wrong = 0;
    QStringList where;
    for (int pos : positions(m.frames)) {
        const auto p = r->picture(pos);
        const int want = std::min(pos, m.frames - 1); // past the end: the last picture
        if (!p || p->position != want || bytesOf(p->frame) != ref.value(want)) {
            ++wrong;
            if (where.size() < 5) where << QString("%1->%2").arg(pos).arg(p ? int(p->position) : -1);
        }
    }
    Check::report(wrong == 0, qPrintable(m.name + ": jumps = in order"), __FILE__, __LINE__,
                  QString("%1 wrong pictures (%2)").arg(wrong).arg(where.join(", ")));
    // Files with an index land on a key frame before the target: never a second seek
    if (!m.name.startsWith("mpeg"))
        Check::report(r->stats().reseeks == 0, qPrintable(m.name + ": no repeated seeks"), __FILE__, __LINE__,
                      QString("%1 of %2 seeks repeated").arg(r->stats().reseeks).arg(r->stats().seeks));
}

// Four threads on one reader, a little out of order
void testThreads(const Media& m, const QVector<QByteArray>& ref)
{
    Video::Reader::Options o;
    o.hardware = false;
    o.threads = 4;
    auto r = Video::Reader::open(m.path, o);
    if (!CHECK(r)) return;
    std::atomic<int> next{0}, wrong{0};
    auto work = [&] {
        std::mt19937 rng(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        for (int i; (i = next++) < m.frames;) {
            const int pos = std::clamp(i + int(rng() % 5) - 2, 0, m.frames - 1);
            const auto p = r->picture(pos);
            if (!p || p->position != pos || bytesOf(p->frame) != ref.value(pos)) ++wrong;
        }
    };
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) threads.emplace_back(work);
    for (auto& t : threads) t.join();
    Check::report(wrong == 0, qPrintable(m.name + ": 4 threads = in order"), __FILE__, __LINE__,
                  QString("%1 wrong pictures").arg(wrong.load()));
}

// Hardware pictures = software pictures (H.264/HEVC decoding is bit exact)
void testHardware(const Media& m)
{
    Video::Reader::Options sw;
    sw.hardware = false;
    auto a = Video::Reader::open(m.path, sw);
    auto b = Video::Reader::open(m.path, Video::Reader::Options{});
    if (!CHECK(a && b)) return;
    // A picture first: the decoder decides on the card at the first frame
    if (!b->picture(0) || !b->stats().hardware) {
        std::printf("       %s: no hardware decoding here (%d failures), not compared\n", qPrintable(m.name),
                    b->stats().hardwareFailures);
        return;
    }
    int failures = 0;
    for (int pos : positions(m.frames)) {
        const auto pa = a->picture(pos), pb = b->picture(pos);
        if (!pa || !pb || pa->position != pb->position || pa->frame->format != pb->frame->format
            || bytesOf(pa->frame) != bytesOf(pb->frame))
            ++failures;
    }
    Check::report(failures == 0 && b->stats().hardware, qPrintable(m.name + ": hardware = software"), __FILE__, __LINE__,
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
        first << (p ? bytesOf(p->frame) : QByteArray());
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
        CHECK(p && bytesOf(p->frame) == first[i]);
    }
}

// One sample of a picture for comparing positions: the luma (8 or 16 bit), the palette colour, the bit (1-bit black
// and white)
uint32_t sampleAt(const AVFrame* f, int x, int y)
{
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(AVPixelFormat(f->format));
    const uint8_t* row = f->data[0] + ptrdiff_t(y) * f->linesize[0];
    if (desc->flags & AV_PIX_FMT_FLAG_PAL) return reinterpret_cast<const uint32_t*>(f->data[1])[row[x]];
    if (desc->flags & AV_PIX_FMT_FLAG_BITSTREAM) return (row[x / 8] >> (7 - x % 8)) & 1;
    if (desc->comp[0].depth > 8) return reinterpret_cast<const uint16_t*>(row)[x];
    return row[x * desc->comp[0].step];
}

// A file with a rotation tag comes upright: the plain file's picture turned by a quarter (the tag's direction)
void testRotation(const QString& plain, const QString& rotated, double degrees, const QString& name = {})
{
    auto a = Video::Reader::open(plain), b = Video::Reader::open(rotated);
    if (!CHECK(a && b)) return;
    CHECK(std::abs(b->info().rotation - degrees) < 0.5);
    const auto pa = a->picture(12), pb = b->picture(12);
    if (!CHECK(pa && pb)) return;
    // The picture's format is the one the reader states (turned sideways, 4:2:2 comes as 4:4:4)
    CHECK_EQ(pb->frame->format, int(b->info().format));
    const int w = pa->frame->width, h = pa->frame->height;
    if (degrees == 180) {
        bool same = pb->frame->width == w && pb->frame->height == h;
        for (int y = 0; same && y < h; ++y)
            for (int x = 0; x < w && same; ++x) same = sampleAt(pa->frame, x, y) == sampleAt(pb->frame, w - 1 - x, h - 1 - y);
        Check::report(same, qPrintable(name + "rotated 180: upside down"), __FILE__, __LINE__);
        return;
    }
    if (!Check::report(pb->frame->width == h && pb->frame->height == w, qPrintable(name + "rotated: sideways"), __FILE__,
                       __LINE__, QString("%1x%2").arg(pb->frame->width).arg(pb->frame->height)))
        return;
    // 90 clockwise: source (x, y) -> (h - 1 - y, x)
    bool same = true;
    for (int y = 0; same && y < h; ++y)
        for (int x = 0; x < w && same; ++x) same = sampleAt(pa->frame, x, y) == sampleAt(pb->frame, h - 1 - y, x);
    // The colour (first chroma plane of YUV) goes along, also where the subsampling changes (4:2:2 -> 4:4:4)
    const AVPixFmtDescriptor* da = av_pix_fmt_desc_get(AVPixelFormat(pa->frame->format));
    const AVPixFmtDescriptor* db = av_pix_fmt_desc_get(AVPixelFormat(pb->frame->format));
    if (da->nb_components >= 3 && !(da->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL)) && da->comp[1].plane == 1
        && db->comp[1].plane == 1) {
        const int bytes = da->comp[1].depth > 8 ? 2 : 1;
        auto at = [&](const AVFrame* f, const AVPixFmtDescriptor* d, int x, int y) {
            const uint8_t* row = f->data[1] + ptrdiff_t(y >> d->log2_chroma_h) * f->linesize[1];
            return bytes == 2 ? reinterpret_cast<const uint16_t*>(row)[x >> d->log2_chroma_w] : row[x >> d->log2_chroma_w];
        };
        for (int y = 0; same && y < h; ++y)
            for (int x = 0; x < w && same; ++x) same = at(pa->frame, da, x, y) == at(pb->frame, db, h - 1 - y, x);
    }
    Check::report(same, qPrintable(QString("%1rotated %2: turned clockwise").arg(name).arg(degrees)), __FILE__, __LINE__);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    qputenv("SCHNEIDI_DECODER_IDLE_MS", "300");
    QCoreApplication app(argc, argv);
    Check::initApp("video_reader");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");

    QTemporaryDir dir;
    if (!CHECK(dir.isValid())) return Check::result();
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
    add("h264-30fps", Check::makeMedia(file("h264-30.mp4"), {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30",
                                                             "-t", "4", "-c:v", "libx264", "-g", "60", "-pix_fmt", "yuv420p"}), 120);
    // Matroska with NTSC rate (1 ms timestamps)
    add("mkv-29.97", Check::makeMedia(file("ntsc.mkv"), {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30000/1001",
                                                         "-t", "4", "-c:v", "libx264", "-pix_fmt", "yuv420p"}), 120);
    // Full range (MJPEG, yuvj420p)
    add("mjpeg-full", Check::makeMedia(file("mjpeg.avi"), {"-f", "lavfi", "-i", src2, "-t", "2", "-c:v", "mjpeg",
                                                           "-pix_fmt", "yuvj420p", "-q:v", "3"}), 50);
    // 4:2:2 10 bit intra (ProRes)
    add("prores", Check::makeMedia(file("prores.mov"), {"-f", "lavfi", "-i", src2, "-t", "2", "-c:v", "prores_ks",
                                                        "-profile:v", "2"}), 50);
    // 10-bit HEVC
    add("hevc-10bit", Check::makeMedia(file("hevc10.mp4"), {"-f", "lavfi", "-i", src2, "-t", "2", "-c:v", "libx265",
                                                            "-pix_fmt", "yuv420p10le", "-x265-params", "log-level=error"}), 50);
    // Phone portrait: landscape pictures with a rotation in the metadata
    add("rotated-90", Check::makeMedia(file("rotated.mp4"), {"-display_rotation", "-90", "-i", file("h264.mp4"),
                                                             "-c", "copy"}), 100);
    // iPhone-like: HEVC 10 bit HLG/BT.2020 in MOV (hvc1, Apple's time scale 600)
    add("iphone-hlg", Check::makeMedia(file("iphone-hlg.mov"), {"-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30", "-t",
                                                                 "2", "-c:v", "libx265", "-pix_fmt", "yuv420p10le",
                                                                 "-tag:v", "hvc1", "-color_primaries", "bt2020",
                                                                 "-color_trc", "arib-std-b67", "-colorspace", "bt2020nc",
                                                                 "-x265-params", "log-level=error",
                                                                 "-video_track_timescale", "600"}), 60);
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
    // MPEG-TS (AVCHD camcorders, DVB) and MPEG-PS (DVD, MPEG-2): no index, seeks land where the bytes are, not on a
    // key frame
    add("mpeg-ts", Check::makeMedia(file("h264.ts"), {"-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25", "-t", "10",
                                                      "-c:v", "libx264", "-g", "25", "-bf", "0"}), 250);
    add("mpeg-ps", Check::makeMedia(file("mpeg2.mpg"), {"-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25", "-t", "10",
                                                        "-c:v", "mpeg2video"}), 250);
    add("vp9-alpha", Check::makeMedia(file("alpha.webm"), {"-f", "lavfi", "-i",
                                                           "testsrc2=size=320x180:rate=25,format=yuva420p", "-t", "1",
                                                           "-c:v", "libvpx-vp9", "-auto-alt-ref", "0"}), 25);

    for (const Media& m : media) {
        const QVector<QByteArray> ref = reference(m);
        if (!Check::report(ref.size() == m.frames, qPrintable(m.name + ": every picture in order"), __FILE__, __LINE__,
                           QString("%1 of %2").arg(ref.size()).arg(m.frames)))
            continue;
        testSeeks(m, ref);
        if (m.name == "h264" || m.name == "hevc-10bit" || m.name == "prores") testThreads(m, ref);
    }

    // What the files state
    auto info = [&](const char* name) {
        auto r = Video::Reader::open(file(name));
        return r ? r->info() : Video::Info{};
    };
    CHECK(info("mjpeg.avi").fullRange && !info("h264.mp4").fullRange);
    CHECK_EQ(info("sd.mp4").colorspace, 601);
    CHECK_EQ(info("h264.mp4").colorspace, 601); // untagged and SD-sized (640x360): BT.601, as players guess
    CHECK_EQ(info("iphone-hlg.mov").colorspace, 2020);
    CHECK_EQ(info("prores.mov").format, AV_PIX_FMT_YUV422P10LE);
    CHECK(av_pix_fmt_desc_get(info("rgba.mov").format)->flags & AV_PIX_FMT_FLAG_ALPHA);
    CHECK(info("h264.mp4").hasBFrames);
    if (auto r = Video::Reader::open(file("interlaced.mp4"))) {
        const auto p = r->picture(3);
        CHECK(p && !p->progressive && p->topFieldFirst);
    }
    if (auto r = Video::Reader::open(file("h264.mp4"))) {
        const auto p = r->picture(3);
        CHECK(p && p->progressive);
    }
    testRotation(file("h264.mp4"), file("rotated.mp4"), 90);
    // Formats a quarter turn cannot keep: 4:2:2 (camera files, iPhone ProRes 422 shot in portrait), palette, 1 bit
    struct Turn {
        const char* name;
        QStringList encode;
    };
    const Turn turns[] = {
        {"yuv422p.mp4", {"-pix_fmt", "yuv422p", "-c:v", "libx264"}},
        {"prores422.mov", {"-c:v", "prores_ks", "-profile:v", "2"}},
        {"pal8.mov", {"-pix_fmt", "pal8", "-c:v", "png"}},
        {"monob.mov", {"-pix_fmt", "monob", "-c:v", "png"}},
    };
    for (const Turn& t : turns) {
        const QString plain = Check::makeMedia(file(t.name), QStringList{"-f", "lavfi", "-i",
                                                                          "testsrc2=size=320x180:rate=25", "-t", "1"}
                                                                 + t.encode);
        const QString turned = Check::makeMedia(dir.filePath(QString("rot-") + t.name),
                                                {"-display_rotation", "-90", "-i", plain, "-c", "copy"});
        if (!CHECK(!plain.isEmpty() && !turned.isEmpty())) continue;
        testRotation(plain, turned, 90, QString(t.name) + ": ");
    }

    // Video that starts late in the file (10 s of sound first, beyond what the stream analysis reads by default): its
    // format, size and rate are found, every picture reads, and the probe and clip format agree
    for (const char* name : {"start10.mkv", "start10.ts"}) {
        const QString late = Check::makeMedia(file(name), {"-f", "lavfi", "-i", "sine=r=48000:d=13", "-itsoffset", "10",
                                                            "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25:d=3",
                                                            "-map", "1:v", "-map", "0:a", "-c:v", "libx264", "-c:a",
                                                            "aac"});
        if (!CHECK(!late.isEmpty())) continue;
        QString error;
        auto r = Video::Reader::open(late, Video::Reader::Options{}, &error);
        if (!Check::report(bool(r), qPrintable(QString(name) + ": opens"), __FILE__, __LINE__, error)) continue;
        CHECK(r->info().width == 320 && r->info().height == 240 && r->info().format == AV_PIX_FMT_YUV420P);
        CHECK(av_cmp_q(r->info().frameRate, AVRational{25, 1}) == 0);
        int read = 0;
        for (int i : {0, 1, 40, 74, 20})
            if (const auto p = r->picture(i); p && p->position == i) ++read;
        Check::report(read == 5, qPrintable(QString(name) + ": pictures"), __FILE__, __LINE__,
                      QString("%1 of 5").arg(read));
        const ClipFormat cf = detectClipFormat(late);
        CHECK(cf.ok && cf.width == 320 && cf.height == 240);
        const auto probed = MediaProbe::read(late, FrameRate{25, 1});
        CHECK(probed && probed->hasVideo && probed->hasAudio);
    }

    // Hardware decoding: 720p H.264 and 10-bit HEVC
    const QString hd = Check::makeMedia(file("hd.mp4"), {"-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30", "-t",
                                                         "3", "-c:v", "libx264", "-g", "60", "-bf", "2", "-pix_fmt",
                                                         "yuv420p"});
    const QString hd10 = Check::makeMedia(file("hd10.mp4"), {"-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30",
                                                             "-t", "2", "-c:v", "libx265", "-pix_fmt", "yuv420p10le",
                                                             "-x265-params", "log-level=error"});
    if (!hd.isEmpty()) {
        auto r = Video::Reader::open(hd);
        CHECK(r && r->info().colorspace == 709); // untagged HD: BT.709
        testHardware(Media{"h264 720p", hd, 90});
    }
    if (!hd10.isEmpty()) testHardware(Media{"hevc 10 bit 720p", hd10, 60});

    testHibernate(file("h264.mp4"));
    return Check::result();
}
