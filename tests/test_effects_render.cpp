// SPDX-License-Identifier: AGPL-3.0-only
// Test effects in the render (MLT): render color correction/blur on color bars and check pixel values -
// direction of the sliders, keyframes, splitting, blur independent of resolution, title with effect (alpha stays),
// same picture with German number format (LC_NUMERIC) at runtime. Test medium is created with ffmpeg.
#include "check.h"

#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/ProjectFormat.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QApplication>
#include <QImage>
#include <QTemporaryDir>
#include <clocale>
#include <cstring>
#include <functional>

namespace {

QString g_media;

QImage render(const Timeline& tl, ProjectFormat fmt, int pos, int w = 0, int h = 0)
{
    auto prof = makeProfile(fmt);
    TimelineBuilder b(*prof);
    auto tr = b.build(tl);
    tr->seek(pos);
    std::unique_ptr<Mlt::Frame> f(tr->get_frame());
    mlt_image_format ifmt = mlt_image_rgba;
    if (!w) {
        w = prof->width();
        h = prof->height();
    }
    const uint8_t* d = f->get_image(ifmt, w, h);
    if (!d) return {};
    QImage img(w, h, QImage::Format_RGBA8888);
    std::memcpy(img.bits(), d, size_t(w) * h * 4);
    return img.copy();
}

// One video track with one clip (color bars, 100 frames), fn adjusts the clip
Timeline one(const std::function<void(Clip&)>& fn)
{
    Timeline tl;
    tl.video.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = g_media;
    c.in = 0;
    c.out = 99;
    fn(c);
    tl.video[0].clips << c;
    return tl;
}

std::function<void(Clip&)> fx(const QString& id, std::initializer_list<std::pair<const char*, double>> vals)
{
    QVector<std::pair<const char*, double>> v(vals);
    return [=](Clip& c) {
        EffectRegistry::add(c, id);
        for (auto [k, x] : v) EffectRegistry::instance(c, id)->params[k] = x;
    };
}

struct Rgb { double r = 0, g = 0, b = 0; };

// Mean of a region (relative 0..1)
Rgb mean(const QImage& img, QRectF rel)
{
    const QRect q(int(rel.x() * img.width()), int(rel.y() * img.height()), int(rel.width() * img.width()),
                  int(rel.height() * img.height()));
    Rgb s;
    int n = 0;
    for (int y = q.top(); y <= q.bottom(); ++y)
        for (int x = q.left(); x <= q.right(); ++x) {
            const QColor c = img.pixelColor(x, y);
            s.r += c.red(); s.g += c.green(); s.b += c.blue();
            ++n;
        }
    if (n) { s.r /= n; s.g /= n; s.b /= n; }
    return s;
}
// SMPTE bars: gray on the left (75 %), cyan at ~35 %
Rgb gray(const QImage& img) { return mean(img, QRectF(0.02, 0.1, 0.08, 0.4)); }
Rgb cyan(const QImage& img) { return mean(img, QRectF(0.35, 0.1, 0.05, 0.4)); }

// Mean deviation of two images (scaled to the same size), 0..255
double diff(QImage a, QImage b)
{
    if (a.isNull() || b.isNull()) return 1e9;
    const QSize s(480, 270);
    a = a.scaled(s, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB888);
    b = b.scaled(s, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB888);
    double sum = 0;
    for (int y = 0; y < s.height(); ++y)
        for (int x = 0; x < s.width() * 3; ++x) sum += std::abs(a.scanLine(y)[x] - b.scanLine(y)[x]);
    return sum / (s.width() * s.height() * 3);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("effects-render");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    g_media = Check::makeMedia(tmp.filePath("bars.mp4"),
                               {"-f", "lavfi", "-i", "smptebars=size=1920x1080:rate=25:duration=4", "-vf",
                                "scale=out_color_matrix=bt709", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt",
                                "yuv420p", "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709"});
    // BT.709 like phone/camera material: MLT colors BT.601 sources differently when scaling (docs/dev/mlt.md), which would
    // distort the size comparisons of the blur
    if (!CHECK(!g_media.isEmpty())) return Check::result();

    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    const ProjectFormat fmt; // 1920 × 1080 @ 25
    {
        // The MLT Qt module (qtblend, qtext) refuses depending on the build without X11/Wayland -> skip without a display
        auto prof = makeProfile(fmt);
        Mlt::Transition qt(*prof, "qtblend");
        if (!qt.is_valid()) return Check::skip("MLT-Qt-Modul nicht nutzbar (kein Display? z. B. xvfb-run -a ctest …)");
    }

    const QImage ref = render(one([](Clip&) {}), fmt, 10);
    if (!CHECK(!ref.isNull())) return Check::result();
    const Rgb g0 = gray(ref), c0 = cyan(ref);
    CHECK(g0.r > 150 && std::abs(g0.r - g0.b) < 10); // bars detected
    CHECK(c0.r < 60 && c0.g > 150 && c0.b > 150);

    // Color correction: neutral/disabled = unchanged, sliders in the right direction
    CHECK(diff(render(one(fx("color", {})), fmt, 10), ref) < 1.0);
    CHECK(diff(render(one([](Clip& c) {
                   fx("color", {{"brightness", 50}})(c);
                   c.effects[0].enabled = false;
               }), fmt, 10), ref) < 1.0);
    CHECK(gray(render(one(fx("color", {{"brightness", 50}})), fmt, 10)).r > g0.r + 20);
    CHECK(gray(render(one(fx("color", {{"brightness", -50}})), fmt, 10)).r < g0.r - 20);
    CHECK(gray(render(one(fx("color", {{"contrast", 50}})), fmt, 10)).r > g0.r + 5); // 75 % gray gets brighter
    const Rgb desat = cyan(render(one(fx("color", {{"saturation", -100}})), fmt, 10));
    CHECK(std::abs(desat.r - desat.g) < 12 && std::abs(desat.g - desat.b) < 12);
    CHECK(cyan(render(one(fx("color", {{"saturation", 50}})), fmt, 10)).r <= c0.r);
    const Rgb warm = gray(render(one(fx("color", {{"temperature", 60}})), fmt, 10));
    const Rgb cold = gray(render(one(fx("color", {{"temperature", -60}})), fmt, 10));
    CHECK(warm.r > warm.b + 15 && cold.b > cold.r + 15);
    const Rgb magenta = gray(render(one(fx("color", {{"tint", 60}})), fmt, 10));
    const Rgb green = gray(render(one(fx("color", {{"tint", -60}})), fmt, 10));
    CHECK(magenta.g < magenta.r - 10 && green.g > green.r + 10);

    // Keyframes brightness -100 (frame 0) -> +100 (frame 50), constant afterwards
    const Timeline keyed = one([](Clip& c) {
        fx("color", {})(c);
        Keys::setKey(c, AnimParam::FxBrightness, 0, -100);
        Keys::setKey(c, AnimParam::FxBrightness, 50, 100);
    });
    const QImage k0 = render(keyed, fmt, 0), k25 = render(keyed, fmt, 25), k40 = render(keyed, fmt, 40),
                 k50 = render(keyed, fmt, 50), k75 = render(keyed, fmt, 75);
    CHECK(gray(k0).r + 20 < gray(k25).r && gray(k25).r + 20 < gray(k50).r);
    CHECK(std::abs(gray(k50).r - gray(k75).r) < 2);
    CHECK(std::abs(gray(k25).r - g0.r) < 6); // middle = brightness 0
    // Split at 25: both parts look like before
    {
        Timeline tl = keyed;
        Clip orig = tl.video[0].clips[0], l = orig, r = orig;
        l.out = 24;
        r.in = 25;
        r.start = 25;
        r.id = 2;
        Keys::split(orig, l, r);
        tl.video[0].clips = {l, r};
        CHECK(diff(render(tl, fmt, 25), k25) < 1.0);
        CHECK(diff(render(tl, fmt, 40), k40) < 1.0);
    }

    // Blur: visible, and the same in preview size, 720p and 4K project (sigma relative to the height)
    const Timeline blur = one(fx("blur", {{"strength", 40}}));
    const QImage full = render(blur, fmt, 10);
    ProjectFormat small = fmt, big = fmt;
    small.width = 1280;
    small.height = 720;
    big.width = 3840;
    big.height = 2160;
    CHECK(diff(ref, full) > 3.0);
    CHECK(diff(full, render(blur, fmt, 10, 960, 540)) < 1.5);
    CHECK(diff(full, render(blur, small, 10)) < 1.5);
    CHECK(diff(full, render(blur, big, 10)) < 1.5);
    const Timeline blurK = one([](Clip& c) {
        fx("blur", {})(c);
        Keys::setKey(c, AnimParam::FxBlur, 0, 0);
        Keys::setKey(c, AnimParam::FxBlur, 50, 60);
    });
    CHECK(diff(render(blurK, fmt, 0), ref) < 1.0);
    CHECK(diff(render(blurK, fmt, 50), render(one(fx("blur", {{"strength", 60}})), fmt, 10)) < 1.0);

    // Title with effect over the video: background still shows through (alpha is preserved)
    for (const char* id : {"color", "blur"}) {
        Timeline tl = one([](Clip&) {});
        tl.video.resize(2);
        Clip t;
        t.id = 5;
        t.kind = ClipKind::Title;
        t.title.text = "HALLO";
        t.title.size = 200;
        t.in = 0;
        t.out = 99;
        EffectRegistry::add(t, id);
        EffectRegistry::instance(t, id)->params["brightness"] = -60;
        tl.video[1].clips << t;
        const double d = diff(render(tl, fmt, 10), ref);
        if (!CHECK(d < 15.0)) std::printf("       Titel+%s: Abweichung %.2f\n", id, d);
    }

    // German number format at runtime (as in the app after startup) must not change anything
    const Timeline mixed = one(fx("color", {{"brightness", 50}, {"temperature", 30}, {"saturation", -25}}));
    const QImage mixedC = render(mixed, fmt, 10);
    if (std::setlocale(LC_NUMERIC, "de_DE.UTF-8") || std::setlocale(LC_NUMERIC, "de_DE.utf8")) {
        CHECK(diff(render(mixed, fmt, 10), mixedC) < 0.5);
        CHECK(diff(render(keyed, fmt, 25), k25) < 0.5);
        CHECK(diff(render(blur, fmt, 10), full) < 0.5);
        std::setlocale(LC_NUMERIC, "C");
    } else {
        std::printf("Hinweis: de_DE-Locale fehlt, Test mit deutschem Zahlenformat übersprungen\n");
    }

    // BT.601 source (without color info, as ffmpeg writes it): same colors in full size, preview size and
    // 720p project (MLT scaler colored it wrongly, detour via ProducerFactory)
    const QString bars601 = Check::makeMedia(tmp.filePath("bars601.mp4"),
                                             {"-f", "lavfi", "-i", "smptebars=size=1920x1080:rate=25:duration=2",
                                              "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (CHECK(!bars601.isEmpty())) {
        g_media = bars601;
        const Timeline plain = one([](Clip&) {});
        ProjectFormat small601 = fmt;
        small601.width = 1280;
        small601.height = 720;
        const QImage native = render(plain, fmt, 10);
        const Rgb c = cyan(native);
        CHECK(c.r < 10 && std::abs(c.g - 191) < 6 && std::abs(c.b - 191) < 6); // 75 %-Cyan
        CHECK(diff(native, render(plain, fmt, 10, 960, 540)) < 1.0);
        CHECK(diff(native, render(plain, small601, 10)) < 1.0);
    }
    return Check::result();
}
