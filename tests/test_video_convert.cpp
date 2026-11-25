// SPDX-License-Identifier: AGPL-3.0-only
// Video::toRgba sets the colour matrix for every conversion: the swscale contexts are cached per thread, and a
// colourspace without a table of its own (BT.2020) once took whatever the previous conversion had set – a 601 picture
// converted after a 2020 one came out different (mean 2.5 levels) from the same picture converted alone. Exact and
// fast (preview) conversions, unscaled and scaled.
// The range: a yuvj picture is full range even when the stream is not tagged so (MJPEG in AVI), a 10-bit picture tagged
// full range (no yuvj twin) is not read as video range.
#include "check.h"

#include "engine/video/Convert.h"
#include "engine/video/Reader.h"

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <QCoreApplication>
#include <QTemporaryDir>

#include <cmath>
#include <thread>
#include <vector>

namespace {

std::vector<uint8_t> convert(Video::Reader& r, int w, int h, bool fast)
{
    std::vector<uint8_t> out(size_t(w) * h * 4);
    const auto pic = r.picture(5);
    if (!pic || !Video::toRgba(*pic, r.info(), out.data(), w, h, fast)) return {};
    return out;
}

double meanDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    if (a.size() != b.size() || a.empty()) return 999;
    double s = 0;
    for (size_t i = 0; i < a.size(); ++i) s += std::abs(int(a[i]) - int(b[i]));
    return s / double(a.size());
}

// A flat grey picture: luma `y`, neutral chroma (8 or 10 bit planar 4:2:0)
Video::Picture flat(AVPixelFormat fmt, int y, int w = 640, int h = 360)
{
    Video::Picture p;
    p.frame = av_frame_alloc();
    p.frame->format = fmt;
    p.frame->width = w;
    p.frame->height = h;
    if (av_frame_get_buffer(p.frame, 0) < 0) return p;
    const bool ten = fmt == AV_PIX_FMT_YUV420P10LE;
    for (int plane = 0; plane < 3; ++plane) {
        const int pw = plane ? (w + 1) / 2 : w, ph = plane ? (h + 1) / 2 : h;
        const int v = plane ? (ten ? 512 : 128) : y;
        for (int r = 0; r < ph; ++r) {
            uint8_t* row = p.frame->data[plane] + size_t(r) * p.frame->linesize[plane];
            for (int c = 0; c < pw; ++c) {
                if (ten) reinterpret_cast<uint16_t*>(row)[c] = uint16_t(v);
                else row[c] = uint8_t(v);
            }
        }
    }
    return p;
}

// Mean red of the converted picture
double meanRed(const Video::Picture& p, const Video::Info& info, int w, int h, bool fast)
{
    std::vector<uint8_t> out(size_t(w) * h * 4);
    if (!Video::toRgba(p, info, out.data(), w, h, fast)) return -1;
    double s = 0;
    for (size_t i = 0; i < out.size(); i += 4) s += out[i];
    return s / double(size_t(w) * h);
}

void testRange()
{
    // Luma 200 full range = 200; read as video range it would be (200 - 16) * 255 / 219 = 214
    Video::Info untagged;
    untagged.colorspace = 709;
    untagged.fullRange = false;
    const Video::Picture jpeg = flat(AV_PIX_FMT_YUVJ420P, 200);
    Video::Info tagged = untagged;
    tagged.fullRange = true;
    const Video::Picture ten = flat(AV_PIX_FMT_YUV420P10LE, 800); // 800 / 1023 * 255 = 199.4
    const Video::Picture limited = flat(AV_PIX_FMT_YUV420P10LE, 800); // (800 - 64) * 255 / 876 = 214.2
    for (const bool fast : {false, true})
        for (const QSize size : {QSize(640, 360), QSize(320, 180)}) {
            const double j = meanRed(jpeg, untagged, size.width(), size.height(), fast);
            const double t = meanRed(ten, tagged, size.width(), size.height(), fast);
            const double l = meanRed(limited, untagged, size.width(), size.height(), fast);
            if (!CHECK(std::abs(j - 200) <= 1.5 && std::abs(t - 199.4) <= 1.5 && std::abs(l - 214.2) <= 1.5))
                std::printf("       range %s %dx%d: yuvj untagged %.1f (200), 10 bit full %.1f (199.4), 10 bit video "
                            "%.1f (214.2)\n",
                            fast ? "fast" : "exact", size.width(), size.height(), j, t, l);
        }
}

// A 4:1:0 picture (old codecs: SVQ1, Indeo) converted in bands over threads: each band starts on a whole chroma row
// (bands rounded to 4 lines, not 2: the colour of every band after the first was two lines off). Chroma changes every
// chroma row; the banded result equals one conversion of the whole picture
void testBands410()
{
    const int w = 320, h = 480;
    Video::Picture p;
    p.frame = av_frame_alloc();
    p.frame->format = AV_PIX_FMT_YUV410P;
    p.frame->width = w;
    p.frame->height = h;
    if (!CHECK(av_frame_get_buffer(p.frame, 0) >= 0)) return;
    for (int plane = 0; plane < 3; ++plane) {
        const int pw = plane ? (w + 3) / 4 : w, ph = plane ? (h + 3) / 4 : h;
        for (int r = 0; r < ph; ++r) {
            uint8_t* row = p.frame->data[plane] + size_t(r) * p.frame->linesize[plane];
            for (int c = 0; c < pw; ++c) row[c] = uint8_t(plane == 0 ? 120 : plane == 1 ? 128 : (r % 2 ? 200 : 60));
        }
    }
    Video::Info info;
    info.colorspace = 601;
    std::vector<uint8_t> banded(size_t(w) * h * 4), whole(size_t(w) * h * 4);
    CHECK(Video::toRgba(p, info, banded.data(), w, h, false));
    SwsContext* ctx = sws_getContext(w, h, AV_PIX_FMT_YUV410P, w, h, AV_PIX_FMT_RGBA,
                                     SWS_POINT | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT, nullptr, nullptr, nullptr);
    if (!CHECK(ctx)) return;
    uint8_t* o[4] = {whole.data()};
    const int os[4] = {w * 4};
    sws_scale(ctx, p.frame->data, p.frame->linesize, 0, h, o, os);
    sws_freeContext(ctx);
    int wrongRows = 0;
    for (int y = 0; y < h; ++y)
        if (std::abs(int(banded[size_t(y) * w * 4]) - int(whole[size_t(y) * w * 4])) > 2) ++wrongRows;
    if (!CHECK_EQ(wrongRows, 0)) std::printf("       4:1:0 bands: %d rows with another colour\n", wrongRows);
}

// Interlaced 4:2:0 converted field by field only at heights of whole chroma row pairs: an odd height (the last row was
// never written) and 4k+2 lines (the second field read a chroma row past the plane) go the progressive way
void testFieldHeights()
{
    for (const int h : {361, 362, 360}) {
        Video::Picture p = flat(AV_PIX_FMT_YUV420P, 200, 640, h);
        p.progressive = false;
        Video::Info info;
        info.colorspace = 709;
        std::vector<uint8_t> out(size_t(640) * h * 4, 7);
        CHECK(Video::toRgba(p, info, out.data(), 640, h, false));
        int stale = 0;
        for (int x = 0; x < 640; ++x) stale += out[(size_t(h - 1) * 640 + x) * 4] == 7;
        if (!CHECK_EQ(stale, 0)) std::printf("       interlaced %d lines: last row not written\n", h);
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("video_convert");
    testRange();
    testBands410();
    testFieldHeights();
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    const QStringList src{"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=1", "-c:v", "libx264",
                          "-preset", "ultrafast", "-pix_fmt", "yuv420p"};
    const QString bt2020 = Check::makeMedia(tmp.filePath("2020.mp4"), QStringList(src) << "-colorspace" << "bt2020nc"
                                                                                       << "-color_primaries" << "bt2020"
                                                                                       << "-color_trc" << "bt709");
    const QString bt601 = Check::makeMedia(tmp.filePath("601.mp4"), QStringList(src) << "-colorspace" << "smpte170m"
                                                                                     << "-color_primaries" << "smpte170m"
                                                                                     << "-color_trc" << "smpte170m");
    const QString bt709 = Check::makeMedia(tmp.filePath("709.mp4"), QStringList(src) << "-colorspace" << "bt709"
                                                                                     << "-color_primaries" << "bt709"
                                                                                     << "-color_trc" << "bt709");
    if (!CHECK(!bt2020.isEmpty() && !bt601.isEmpty() && !bt709.isEmpty())) return Check::result();
    Video::Reader::Options o;
    o.hardware = false;
    auto r2020 = Video::Reader::open(bt2020, o);
    auto r601 = Video::Reader::open(bt601, o);
    auto r709 = Video::Reader::open(bt709, o);
    if (!CHECK(r2020 && r601 && r709)) return Check::result();
    CHECK_EQ(r2020->info().colorspace, 2020);
    CHECK_EQ(r601->info().colorspace, 601);
    CHECK_EQ(r709->info().colorspace, 709);
    for (const bool fast : {false, true})
        for (const QSize size : {QSize(640, 360), QSize(320, 180)}) {
            const int w = size.width(), h = size.height();
            // Each in a fresh thread (the cached contexts are per thread): `before` converted first, then 2020 and 601
            auto run = [&](Video::Reader& before, std::vector<uint8_t>* out2020, std::vector<uint8_t>* out601) {
                std::thread t([&] {
                    convert(before, w, h, fast);
                    *out2020 = convert(*r2020, w, h, fast);
                    *out601 = convert(*r601, w, h, fast);
                });
                t.join();
            };
            std::vector<uint8_t> after601, after709, b601, b709, alone601;
            run(*r601, &after601, &b601);
            run(*r709, &after709, &b709);
            std::thread t([&] { alone601 = convert(*r601, w, h, fast); });
            t.join();
            const double d2020 = meanDiff(after601, after709), d601 = meanDiff(b709, alone601);
            if (!CHECK(d2020 == 0 && d601 == 0))
                std::printf("       %s %dx%d: 2020 after 601 / after 709 differ by %.2f, 601 by %.2f\n",
                            fast ? "fast" : "exact", w, h, d2020, d601);
            // 2020 has its own matrix: the same pixels give other colours than as 601 or 709
            const auto as709 = [&] {
                std::vector<uint8_t> v;
                std::thread t2([&] { v = convert(*r709, w, h, fast); });
                t2.join();
                return v;
            }();
            CHECK(meanDiff(after601, alone601) > 0.5 && meanDiff(after601, as709) > 0.3);
        }
    return Check::result();
}
