// SPDX-License-Identifier: AGPL-3.0-only
// Video::toRgba sets the colour matrix for every conversion: the swscale contexts are cached per thread, and a
// colourspace without a table of its own (BT.2020) once took whatever the previous conversion had set – a 601 picture
// converted after a 2020 one came out different (mean 2.5 levels) from the same picture converted alone. Exact and
// fast (preview) conversions, unscaled and scaled.
#include "check.h"

#include "engine/video/Convert.h"
#include "engine/video/Reader.h"

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

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("video_convert");
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
