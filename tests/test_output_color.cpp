// SPDX-License-Identifier: AGPL-3.0-only
// OutputColor: the Y'CbCr matrix between color standards (against exact values, all formats) and the filter on a
// real graph: a BT.601 source must leave the workers as BT.709 (otherwise the export's single encoder thread does
// the matrix for every frame, docs/dev/mlt.md › Export-Tempo).

#include "check.h"

#include "engine/OutputColor.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QApplication>
#include <QTemporaryDir>
#include <array>
#include <clocale>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

struct Ycc {
    double y, cb, cr;
};

void weights(int cs, double& kr, double& kb)
{
    if (cs == 709) kr = 0.2126, kb = 0.0722;
    else kr = 0.299, kb = 0.114;
}

// R'G'B' 0..1 -> limited range Y'CbCr code values (8 bit, times 4 for 10 bit)
Ycc encode(double r, double g, double b, int cs, int scale = 1)
{
    double kr, kb;
    weights(cs, kr, kb);
    const double y = kr * r + (1 - kr - kb) * g + kb * b;
    return {scale * (16 + 219 * y), scale * (128 + 224 * (b - y) / (2 * (1 - kb))),
            scale * (128 + 224 * (r - y) / (2 * (1 - kr)))};
}

// Exact conversion of code values (via R'G'B', no clipping)
Ycc exact(Ycc v, int from, int to, int scale = 1)
{
    double kr, kb;
    weights(from, kr, kb);
    const double y = (v.y / scale - 16) / 219, pb = (v.cb / scale - 128) / 224, pr = (v.cr / scale - 128) / 224;
    const double r = y + 2 * (1 - kr) * pr, b = y + 2 * (1 - kb) * pb;
    const double g = (y - kr * r - kb * b) / (1 - kr - kb);
    return encode(r, g, b, to, scale);
}

const std::array<std::array<double, 3>, 7> kColors{{
    {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0.75, 0.75, 0}, {0, 0.75, 0.75}, {0.5, 0.5, 0.5}, {0.1, 0.05, 0.2}}};

// Fills a width x height image of the format with one color, converts it, returns the first pixel's values
template <typename T>
Ycc roundTrip(mlt_image_format fmt, Ycc in, int from, int to, int w = 4, int h = 2)
{
    const bool packed = fmt == mlt_image_yuv422;
    const int cw = fmt == mlt_image_yuv444p10 ? w : w / 2, ch = fmt == mlt_image_yuv444p10 ? h : h / 2;
    const size_t luma = size_t(w) * h, chroma = packed ? 0 : size_t(cw) * ch;
    std::vector<T> src(packed ? luma * 2 : luma + 2 * chroma), dst(src.size(), T(0));
    const T y = T(std::lround(in.y)), cb = T(std::lround(in.cb)), cr = T(std::lround(in.cr));
    if (packed) {
        for (size_t i = 0; i < luma / 2; ++i) {
            src[4 * i] = src[4 * i + 2] = y;
            src[4 * i + 1] = cb;
            src[4 * i + 3] = cr;
        }
    } else {
        std::fill(src.begin(), src.begin() + luma, y);
        std::fill(src.begin() + luma, src.begin() + luma + chroma, cb);
        std::fill(src.begin() + luma + chroma, src.end(), cr);
    }
    if (!OutputColor::convert(reinterpret_cast<const uint8_t*>(src.data()), reinterpret_cast<uint8_t*>(dst.data()), fmt,
                              w, h, from, to))
        return {-1, -1, -1};
    // every pixel the same
    for (size_t i = 0; i < (packed ? luma / 2 : luma); ++i)
        if (packed ? dst[4 * i] != dst[0] || dst[4 * i + 2] != dst[0] : dst[i] != dst[0]) return {-2, -2, -2};
    return packed ? Ycc{double(dst[0]), double(dst[1]), double(dst[3])}
                  : Ycc{double(dst[0]), double(dst[luma]), double(dst[luma + chroma])};
}

double worst(Ycc a, Ycc b)
{
    return std::max({std::abs(a.y - b.y), std::abs(a.cb - b.cb), std::abs(a.cr - b.cr)});
}

void testMatrix()
{
    for (const auto& c : kColors) {
        const Ycc s601 = encode(c[0], c[1], c[2], 601);
        // Input is rounded to code values, so up to ~1 off the exact (unrounded) target
        const Ycc want = exact(Ycc{std::round(s601.y), std::round(s601.cb), std::round(s601.cr)}, 601, 709);
        for (mlt_image_format fmt : {mlt_image_yuv422, mlt_image_yuv420p}) {
            const Ycc got = roundTrip<uint8_t>(fmt, s601, 601, 709);
            if (!CHECK(worst(got, want) <= 0.51))
                std::printf("       %s (%.2f %.2f %.2f): %.0f %.0f %.0f, want %.2f %.2f %.2f\n", mlt_image_format_name(fmt),
                            c[0], c[1], c[2], got.y, got.cb, got.cr, want.y, want.cb, want.cr);
        }
        const Ycc s601x = encode(c[0], c[1], c[2], 601, 4);
        const Ycc want10 = exact(Ycc{std::round(s601x.y), std::round(s601x.cb), std::round(s601x.cr)}, 601, 709, 4);
        for (mlt_image_format fmt : {mlt_image_yuv420p10, mlt_image_yuv444p10})
            CHECK(worst(roundTrip<uint16_t>(fmt, s601x, 601, 709), want10) <= 0.51);
        // and back: 709 -> 601 hits the source again
        const Ycc s709 = encode(c[0], c[1], c[2], 709);
        CHECK(worst(roundTrip<uint8_t>(mlt_image_yuv420p, s709, 709, 601), s601) <= 1.01);
    }
    // 601 family members are the same matrix: nothing changes
    const Ycc gray{120, 100, 160};
    CHECK(worst(roundTrip<uint8_t>(mlt_image_yuv420p, gray, 601, 170), gray) == 0);
    // Not handled: unknown standard, odd size, RGB
    uint8_t px[64] = {};
    CHECK(!OutputColor::convert(px, px, mlt_image_yuv420p, 4, 2, 601, 999));
    CHECK(!OutputColor::convert(px, px, mlt_image_yuv420p, 3, 2, 601, 709));
    CHECK(!OutputColor::convert(px, px, mlt_image_rgba, 4, 2, 601, 709));
}

// The color standard and image a frame of the graph arrives with at a consumer asking for yuv420p
struct Arrived {
    int colorspace = 0;
    std::vector<uint8_t> image;
};

Arrived fetch(const QString& media, bool withFilter)
{
    const ProjectFormat fmt{640, 360, {25, 1}};
    auto prof = makeProfile(fmt);
    TimelineBuilder b(*prof);
    Timeline tl;
    tl.video.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = media;
    c.out = 49;
    tl.video[0].clips << c;
    auto tr = b.build(tl);
    Mlt::Producer out(*tr);
    if (withFilter) out.attach(*OutputColor::make(*prof));
    out.seek(10);
    std::unique_ptr<Mlt::Frame> f(out.get_frame());
    mlt_image_format ifmt = mlt_image_yuv420p;
    int w = fmt.width, h = fmt.height;
    const uint8_t* d = f->get_image(ifmt, w, h);
    Arrived a;
    if (!d || ifmt != mlt_image_yuv420p) return a;
    a.colorspace = f->get_int("colorspace");
    a.image.assign(d, d + size_t(w) * h * 3 / 2);
    return a;
}

void testGraph(const QString& dir)
{
    // Small unmarked file: MLT takes it for BT.601 (ProducerFactory gives it a 601 profile)
    const QString media = Check::makeMedia(dir + "/bars.mp4", {"-f", "lavfi", "-i", "smptebars=size=640x360:rate=25:duration=2",
                                                               "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!media.isEmpty())) return;
    const Arrived plain = fetch(media, false);
    const Arrived converted = fetch(media, true);
    if (!CHECK(!plain.image.empty() && !converted.image.empty())) return;
    // Without the filter MLT hands over 601 (the reason for it); if this changes, the filter is no longer needed
    if (plain.colorspace != 601) std::printf("Note: MLT now hands over color standard %d instead of 601\n", plain.colorspace);
    CHECK_EQ(converted.colorspace, 709);
    // Same picture, converted exactly (per chroma sample, its 2x2 luma)
    const int w = 640, h = 360, cw = w / 2;
    const size_t luma = size_t(w) * h, chroma = luma / 4;
    double worstDiff = 0;
    for (int cy = 0; cy < h / 2; ++cy)
        for (int cx = 0; cx < cw; ++cx) {
            const size_t ci = size_t(cy) * cw + cx;
            const double cb = plain.image[luma + ci], cr = plain.image[luma + chroma + ci];
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i) {
                    const size_t li = size_t(2 * cy + j) * w + 2 * cx + i;
                    const Ycc want = exact(Ycc{double(plain.image[li]), cb, cr}, plain.colorspace, 709);
                    const Ycc got{double(converted.image[li]), double(converted.image[luma + ci]),
                                  double(converted.image[luma + chroma + ci])};
                    worstDiff = std::max(worstDiff, worst(got, Ycc{std::clamp(want.y, 0.0, 255.0),
                                                                   std::clamp(want.cb, 0.0, 255.0),
                                                                   std::clamp(want.cr, 0.0, 255.0)}));
                }
        }
    if (!CHECK(worstDiff <= 1.0)) std::printf("       largest difference %.2f\n", worstDiff);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("output_color");
    testMatrix();
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg not found");
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    testGraph(tmp.path());
    return Check::result();
}
