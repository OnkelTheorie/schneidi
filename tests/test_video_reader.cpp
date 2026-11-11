// SPDX-License-Identifier: AGPL-3.0-only
// Own video decoder (engine/video/Reader) on many kinds of files, without a second decoder to compare against: the
// pictures read in order are the reference, and every other way of asking must give the same pictures -
// - jumps back and forth, small steps, the end and past it (seeks to key frames, B-frames, preseek)
// - several threads asking one reader a little out of order (export workers, read-ahead)
// - hardware decoding (if this machine has a device) = software decoding bit for bit (H.264, 10-bit HEVC)
// - unused readers close their decoder after a pause and give the same pictures when asked again
// - a decoder that cannot be opened again (after a hardware failure) answers nothing at once, for good
// - a gap in the timestamps of a variable-rate file (a picture held for 2 s) shows the held picture, not the next one
// plus the frame properties the file states (size, rotation, colour standard and range, interlacing, alpha).
// Replaces the comparison with MLT's producer (test_video_decoder, until 0.15).

#include "check.h"

#include "engine/ClipFormat.h"
#include "engine/MediaProbe.h"
#include "engine/video/Convert.h"
#include "engine/video/Reader.h"
#include "engine/video/Scan.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <mutex>
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

// Variable frame rate with a gap: picture N of the file shows its frame number as luma (16 + 2N), pictures 0..29
// stand at positions 0..29 (60 fps), picture 29 is held for 2 s, pictures 30..59 stand at 150..179. Every position in
// the gap shows the held picture 29 (it is on screen until the next one), not picture 30 two seconds too early - when
// playing in order, after a seek into the gap (fresh or used reader, preview seek window or none), stepping back.
int lumaOf(const std::shared_ptr<const Video::Picture>& p)
{
    if (!p || !p->frame) return -1;
    const AVFrame* f = p->frame;
    int64_t sum = 0;
    for (int y = 0; y < f->height; ++y)
        for (int x = 0; x < f->width; ++x) sum += f->data[0][y * f->linesize[0] + x];
    return int((sum + f->width * f->height / 2) / (int64_t(f->width) * f->height));
}

void testVfrGap(const QString& path)
{
    const auto expected = [](int position) {
        const int picture = position < 30 ? position : position < 150 ? 29 : std::min(59, position - 120);
        return 16 + 2 * picture;
    };
    int wrong = 0;
    QString first;
    const auto check = [&](Video::Reader& r, int position, const char* how) {
        const int got = lumaOf(r.picture(position));
        if (std::abs(got - expected(position)) <= 1) return;
        if (++wrong == 1)
            first = QString("%1 at %2: luma %3, expected %4").arg(how).arg(position).arg(got).arg(expected(position));
    };
    for (const int64_t keep : {int64_t(0), int64_t(96) * 1024 * 1024}) {
        Video::Reader::Options o;
        o.hardware = false;
        o.keepBytes = keep;
        {
            auto r = Video::Reader::open(path, o);
            if (!CHECK(r)) return;
            CHECK(r->info().frameRate.num == 60 && r->info().frameRate.den == 1);
            for (int p = 0; p < 180; ++p) check(*r, p, "in order");
        }
        for (int p : {30, 60, 100, 149, 150, 151}) { // a fresh reader, straight into the gap
            auto r = Video::Reader::open(path, o);
            check(*r, p, "fresh seek");
        }
        {
            auto r = Video::Reader::open(path, o);
            for (int p : {170, 60, 40, 120, 160, 31, 149, 28, 29, 30, 150, 148}) check(*r, p, "jumps");
            for (int p = 155; p >= 25; --p) check(*r, p, "backwards");
        }
    }
    // No pictures kept behind and none ahead (hover-scrub thumbnails, ScrubFrames): a request in the gap is answered,
    // not seeked for forever (evict() dropped the held picture, the request seeked again - 100 % CPU). A cancel after
    // 10 s turns a hang into a failure.
    const auto guarded = [&](Video::Reader& r, int p, const char* how) {
        std::atomic<bool> cancel{false}, answered{false};
        std::thread timer([&] {
            for (int i = 0; i < 1000 && !answered; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            cancel = true;
        });
        Video::Reader::Fetch fetch;
        fetch.cancel = &cancel;
        const int got = lumaOf(r.picture(p, fetch));
        answered = true;
        timer.join();
        if (std::abs(got - expected(p)) > 1 && ++wrong == 1)
            first = QString("%1 at %2: luma %3, expected %4").arg(how).arg(p).arg(got).arg(expected(p));
    };
    Video::Reader::Options none;
    none.hardware = false;
    none.ahead = 0;
    none.behind = 0;
    for (int p : {100, 60, 149, 30, 120})
        if (auto r = Video::Reader::open(path, none)) guarded(*r, p, "behind 0, fresh");
    if (auto r = Video::Reader::open(path, none))
        for (int p = 0; p < 180; p += 7) guarded(*r, p, "behind 0, in order");
    Check::report(wrong == 0, "vfr gap: the held picture", __FILE__, __LINE__,
                  QString("%1 wrong pictures, first %2").arg(wrong).arg(first));
}

// The hardware decoder failed and the software one does not open either (out of memory): every request is answered
// with nothing at once and failed() says so. The decoder thread used to go on with the unopened decoder (a crash in
// avcodec_flush_buffers, or requests waiting forever).
void testDecoderLost(const QString& path)
{
    auto r = Video::Reader::open(path);
    if (!CHECK(r)) return;
    CHECK(r->picture(5) != nullptr);
    CHECK(!r->failed());
    Video::Reader::loseDecoderForTests(true);
    auto asked = std::async(std::launch::async, [&] { return r->picture(60) == nullptr; });
    const bool answered = asked.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    Video::Reader::loseDecoderForTests(false);
    if (!CHECK(answered)) return;
    CHECK(asked.get());
    CHECK(r->failed());
    CHECK(r->picture(6) == nullptr); // at once, no thread left to answer
    CHECK_EQ(r->stats().hardwareFailures, 1);
}

// A reversed clip in a forward export (or forward playback): the export's small windows (4 behind, 6 ahead), four
// workers asking a little out of order, going backwards through a long GOP. Seeking from the key frame for every few
// pictures decoded the GOP quadratically; after a few steps back the reader keeps a large window behind.
void testBackwards(const Media& m, const QVector<QByteArray>& ref, int from, int count)
{
    Video::Reader::Options o;
    o.hardware = false;
    o.behind = 4;
    o.ahead = 6;
    auto r = Video::Reader::open(m.path, o);
    if (!CHECK(r)) return;
    QElapsedTimer t;
    t.start();
    std::atomic<int> next{0}, wrong{0};
    auto work = [&] {
        for (int i; (i = next++) < count;) {
            const int pos = from - i;
            const auto p = r->picture(pos);
            if (!p || p->position != pos || bytesOf(p->frame) != ref.value(pos)) ++wrong;
        }
    };
    std::vector<std::thread> threads;
    for (int k = 0; k < 4; ++k) threads.emplace_back(work);
    for (auto& th : threads) th.join();
    // Two seeks from the key frame before the reader notices, then one window: three passes through the GOP at most
    const Video::Reader::Stats st = r->stats();
    std::printf("       %s backwards %d pictures: %lld ms, %d seeks, %d decoded\n", qPrintable(m.name), count,
                static_cast<long long>(t.elapsed()), st.seeks, st.decoded);
    CHECK_EQ(wrong.load(), 0);
    Check::report(st.decoded <= 3 * (from + 1) && st.seeks <= 4,
                  qPrintable(m.name + ": backwards without decoding the GOP again and again"),
                  __FILE__, __LINE__, QString("%1 decoded for %2 pictures, %3 seeks").arg(st.decoded).arg(count).arg(st.seeks));
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
// Interlaced 4:2:0 converted field by field (chroma row k belongs to field k % 2) in limited and in full range
// (yuvj420p, or yuv420p marked full): full range went the progressive way before 0.21 - each field's lines took the
// other field's colour. Top field red, bottom field blue
void testInterlacedFields()
{
    struct Case {
        AVPixelFormat fmt;
        bool full;
    };
    for (const Case c : {Case{AV_PIX_FMT_YUV420P, false}, Case{AV_PIX_FMT_YUV420P, true}, Case{AV_PIX_FMT_YUVJ420P, true}}) {
        const int w = 64, h = 32;
        Video::Picture pic;
        pic.frame = av_frame_alloc();
        pic.frame->format = c.fmt;
        pic.frame->width = w;
        pic.frame->height = h;
        if (!CHECK(av_frame_get_buffer(pic.frame, 32) == 0)) return;
        pic.progressive = false;
        pic.topFieldFirst = true;
        // BT.601 red / blue as Y, Cb, Cr
        const int red[3] = {c.full ? 76 : 81, c.full ? 85 : 90, c.full ? 255 : 240};
        const int blue[3] = {c.full ? 29 : 41, c.full ? 255 : 240, c.full ? 107 : 110};
        for (int y = 0; y < h; ++y) std::memset(pic.frame->data[0] + y * pic.frame->linesize[0], (y % 2 ? blue : red)[0], w);
        for (int y = 0; y < h / 2; ++y)
            for (int p = 1; p <= 2; ++p) std::memset(pic.frame->data[p] + y * pic.frame->linesize[p], (y % 2 ? blue : red)[p], w / 2);
        Video::Info info;
        info.width = w;
        info.height = h;
        info.format = c.fmt;
        info.colorspace = 601;
        info.fullRange = c.full;
        std::vector<uint8_t> out(size_t(w) * h * 4);
        if (!CHECK(Video::toRgba(pic, info, out.data(), w, h, false))) continue;
        int wrong = 0;
        for (int y = 4; y < h - 4; ++y) {
            const uint8_t* px = &out[(size_t(y) * w + w / 2) * 4];
            const bool isRed = px[0] > 200 && px[2] < 60, isBlue = px[2] > 200 && px[0] < 60;
            wrong += y % 2 ? !isBlue : !isRed;
        }
        std::printf("  interlaced %s %s: %d of %d lines in the wrong colour\n", av_get_pix_fmt_name(c.fmt),
                    c.full ? "full" : "limited", wrong, h - 8);
        CHECK_EQ(wrong, 0);
    }
}

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
    // One GOP of 8 s (screen recordings, some cameras): a reversed clip goes backwards through all of it
    add("long-gop", Check::makeMedia(file("long-gop.mp4"), {"-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25", "-t",
                                                            "8", "-c:v", "libx264", "-g", "200", "-pix_fmt", "yuv420p"}),
        200);
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
    add("vp9", Check::makeMedia(file("plain.webm"), {"-f", "lavfi", "-i", src2, "-t", "2", "-c:v", "libvpx-vp9",
                                                     "-deadline", "realtime", "-cpu-used", "8"}), 50);
    add("vp8", Check::makeMedia(file("plain-vp8.webm"), {"-f", "lavfi", "-i", src2, "-t", "2", "-c:v", "libvpx",
                                                         "-deadline", "realtime", "-cpu-used", "8"}), 50);
    add("vp9-alpha", Check::makeMedia(file("alpha.webm"), {"-f", "lavfi", "-i",
                                                           "testsrc2=size=320x180:rate=25,format=yuva420p", "-t", "1",
                                                           "-c:v", "libvpx-vp9", "-auto-alt-ref", "0"}), 25);
    // The same without the alpha_mode tag (another muxer, a remux): the alpha is in the blocks' BlockAdditional
    add("vp9-alpha-untagged", Check::makeMedia(file("alpha-untagged.webm"), {"-i", file("alpha.webm"), "-c", "copy",
                                                                             "-metadata:s:v:0", "alpha_mode="}), 25);

    for (const Media& m : media) {
        const QVector<QByteArray> ref = reference(m);
        if (!Check::report(ref.size() == m.frames, qPrintable(m.name + ": every picture in order"), __FILE__, __LINE__,
                           QString("%1 of %2").arg(ref.size()).arg(m.frames)))
            continue;
        testSeeks(m, ref);
        if (m.name == "h264" || m.name == "hevc-10bit" || m.name == "prores") testThreads(m, ref);
        if (m.name == "long-gop") testBackwards(m, ref, m.frames - 1, 100);
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
    // VP8/VP9: libvpx only for alpha (FFmpeg's own decoders drop it); else FFmpeg's (recovers after damage, hardware)
    CHECK_EQ(info("plain.webm").codec, QString("vp9"));
    CHECK_EQ(info("plain-vp8.webm").codec, QString("vp8"));
    CHECK_EQ(info("alpha.webm").codec, QString("libvpx-vp9"));
    CHECK_EQ(info("alpha.webm").format, AV_PIX_FMT_YUVA420P);
    CHECK_EQ(info("alpha-untagged.webm").codec, QString("libvpx-vp9"));
    CHECK_EQ(info("alpha-untagged.webm").format, AV_PIX_FMT_YUVA420P);
    if (auto r = Video::Reader::open(file("interlaced.mp4"))) {
        const auto p = r->picture(3);
        CHECK(p && !p->progressive && p->topFieldFirst);
    }
    if (auto r = Video::Reader::open(file("h264.mp4"))) {
        const auto p = r->picture(3);
        CHECK(p && p->progressive);
    }
    testInterlacedFields();
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

    // A rotation that is no quarter turn (45 degrees) is not applied - by the reader, the analyses' scan and the clip
    // format alike (the scan used to round it to a quarter turn)
    if (const QString odd = Check::makeMedia(file("rot45.mp4"), {"-display_rotation", "-45", "-i", file("h264.mp4"),
                                                                  "-c", "copy"});
        CHECK(!odd.isEmpty())) {
        auto r = Video::Reader::open(odd);
        const auto p = r ? r->picture(3) : nullptr;
        CHECK(r && std::abs(r->info().rotation - 45) < 0.5 && p && p->frame->width == 640 && p->frame->height == 360);
        Video::Scan::Options so;
        so.width = 0;
        so.threads = 1;
        std::atomic<int> scanW{0}, scanH{0};
        const auto scanned = Video::Scan::run(odd, so, [&](const Video::Scan::Frame& f, const Video::Scan::Frame*) {
            scanW = f.width;
            scanH = f.height;
        });
        Check::report(scanned.ok && scanW == 640 && scanH == 360, "rotated 45: scan not turned", __FILE__, __LINE__,
                      QString("%1x%2 %3").arg(scanW.load()).arg(scanH.load()).arg(scanned.error));
        const ClipFormat cf = detectClipFormat(odd);
        CHECK(cf.ok && cf.width == 640 && cf.height == 360);
    }
    // MPEG-2 program stream: its last picture has no timestamp; the scan gave it the time AV_NOPTS_VALUE - first (a
    // signed overflow) instead of one after the picture before
    if (const QString mpg = Check::makeMedia(file("program.mpg"), {"-f", "lavfi", "-i", "testsrc2=size=320x240:rate=30",
                                                                    "-t", "2", "-c:v", "mpeg2video"});
        CHECK(!mpg.isEmpty())) {
        Video::Scan::Options so;
        so.threads = 1;
        std::mutex lock;
        double lowest = 0, highest = 0;
        int pictures = 0;
        const auto scanned = Video::Scan::run(mpg, so, [&](const Video::Scan::Frame& f, const Video::Scan::Frame*) {
            std::lock_guard g(lock);
            lowest = std::min(lowest, f.seconds);
            highest = std::max(highest, f.seconds);
            ++pictures;
        });
        Check::report(scanned.ok && lowest >= 0 && highest < 2.5 && pictures >= 59, "mpeg-2: scan times", __FILE__,
                      __LINE__, QString("%1 pictures, %2 .. %3 s").arg(pictures).arg(lowest).arg(highest));
    }
    // A 1x1 video still has a clip format (even sizes, at least 2x2). Made in RGB: FFmpeg 6.1's color source cannot
    // make 1x1 in its default 4:2:0 ("Picture size 0x0 is invalid" for the chroma planes)
    if (const QString tiny = Check::makeMedia(file("tiny.mkv"), {"-f", "lavfi", "-i", "color=c=red:s=2x2:r=25,format=gbrp,scale=1:1",
                                                                  "-t", "1", "-c:v", "ffv1"});
        CHECK(!tiny.isEmpty())) {
        const ClipFormat cf = detectClipFormat(tiny);
        Check::report(cf.ok && cf.width == 2 && cf.height == 2, "1x1 video: clip format 2x2", __FILE__, __LINE__,
                      QString("%1 %2x%3").arg(cf.ok).arg(cf.width).arg(cf.height));
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
        // The long analysis runs once per file and session: opened again, it is not repeated
        const int runs = Video::Reader::longAnalyses();
        CHECK(Video::Reader::open(late) != nullptr);
        CHECK_EQ(Video::Reader::longAnalyses(), runs);
    }
    // Video later than even the long analysis reads (70 s of sound first): refused, and on the next open at once
    // (it read 60 s on every open)
    if (const QString later = Check::makeMedia(file("start70.ts"), {"-f", "lavfi", "-i", "sine=r=8000:d=72", "-itsoffset",
                                                                    "70", "-f", "lavfi", "-i",
                                                                    "testsrc2=size=160x120:rate=25:d=1", "-map", "1:v",
                                                                    "-map", "0:a", "-c:v", "libx264", "-c:a", "aac",
                                                                    "-b:a", "16k"});
        CHECK(!later.isEmpty())) {
        const int runs = Video::Reader::longAnalyses();
        CHECK(Video::Reader::open(later) == nullptr);
        CHECK_EQ(Video::Reader::longAnalyses(), runs + 1);
        CHECK(Video::Reader::open(later) == nullptr);
        CHECK_EQ(Video::Reader::longAnalyses(), runs + 1);
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
    testDecoderLost(file("h264.mp4"));
    // Picture 29 held for 2 s (lossless: the luma is the picture's number), Matroska (1 ms timestamps)
    if (const QString vfr = Check::makeMedia(file("vfr-gap.mkv"),
                                             {"-f", "lavfi", "-i", "testsrc2=size=320x240:rate=60,format=yuv420p,geq=lum='16+2*N':cb=128:cr=128",
                                              "-frames:v", "60", "-vf", "setpts='if(gte(N,30),(N+120)/60/TB,N/60/TB)'",
                                              "-fps_mode", "vfr", "-c:v", "libx264", "-g", "60", "-qp", "0", "-pix_fmt",
                                              "yuv420p"});
        CHECK(!vfr.isEmpty()))
        testVfrGap(vfr);
    return Check::result();
}
