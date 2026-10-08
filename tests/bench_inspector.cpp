// Benchmark (not a CTest test): cost of one Inspector change with many effects and keyframes.
// Measures building the MLT timeline (what every Inspector change triggers) and rendering preview frames.
// Run: ./build-tests/tests/bench_inspector [clips] [frei0r effects]
#include "check.h"

#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/ProjectFormat.h"
#include "engine/Frei0r.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QTemporaryDir>
#include <cstring>
#include <clocale>
#include <cstdio>

namespace {

QString g_media;

Timeline make(int clips, const QStringList& frei0r, bool effects)
{
    Timeline tl;
    tl.video.resize(2);
    for (int i = 0; i < clips; ++i) {
        Clip c;
        c.id = i + 1;
        c.mediaPath = g_media;
        c.in = 0;
        c.out = 99;
        c.start = i * 100;
        if (effects) {
            EffectRegistry::add(c, "color");
            EffectRegistry::add(c, "blur");
            EffectRegistry::add(c, "grade");
            for (const QString& id : frei0r) EffectRegistry::add(c, id);
            // keyframes on transform and effect parameters
            for (AnimParam p : {AnimParam::ZoomX, AnimParam::ZoomY, AnimParam::PosX, AnimParam::FxBrightness,
                                AnimParam::FxBlur, AnimParam::GradeSaturation}) {
                Keys::setKey(c, p, 0, Keys::staticValue(c, p));
                Keys::setKey(c, p, 50, Keys::staticValue(c, p) + (p == AnimParam::ZoomX || p == AnimParam::ZoomY ? 0.2 : 5));
                Keys::setKey(c, p, 99, Keys::staticValue(c, p));
            }
        }
        tl.video[0].clips << c;
    }
    return tl;
}

double msBuild(const Mlt::Profile& prof, const Timeline& tl, int n)
{
    TimelineBuilder b(const_cast<Mlt::Profile&>(prof));
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < n; ++i) auto tr = b.build(tl);
    return t.nsecsElapsed() / 1e6 / n;
}

double msFrames(const Mlt::Profile& prof, const Timeline& tl, int pos, int n, int w, int h)
{
    TimelineBuilder b(const_cast<Mlt::Profile&>(prof));
    auto tr = b.build(tl);
    tr->seek(pos);
    { std::unique_ptr<Mlt::Frame> f(tr->get_frame()); mlt_image_format fm = mlt_image_rgba; int ww = w, hh = h; f->get_image(fm, ww, hh); }
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < n; ++i) {
        std::unique_ptr<Mlt::Frame> f(tr->get_frame());
        mlt_image_format fm = mlt_image_rgba;
        int ww = w, hh = h;
        f->get_image(fm, ww, hh);
    }
    return t.nsecsElapsed() / 1e6 / n;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("bench-inspector");
    const int clips = argc > 1 ? atoi(argv[1]) : 20;
    const int nFrei0r = argc > 2 ? atoi(argv[2]) : 5;
    QTemporaryDir tmp;
    g_media = Check::makeMedia(tmp.filePath("bars.mp4"),
                               {"-f", "lavfi", "-i", "testsrc2=size=1920x1080:rate=25:duration=4", "-c:v", "libx264",
                                "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    Frei0r::prepareEnvironment();
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    Frei0r::registerEffects();
    QStringList frei0r;
    for (const EffectDescriptor& d : EffectRegistry::all())
        if (d.category == EffectRegistry::Frei0rCategory && d.video && frei0r.size() < nFrei0r
            && !d.id.contains("bluescreen") && !d.id.contains("facedetect"))
            frei0r << d.id;
    std::printf("clips %d, frei0r: %s\n", clips, qPrintable(frei0r.join(", ")));

    const ProjectFormat fmt;
    auto prof = makeProfile(fmt);
    const Timeline plain = make(clips, frei0r, false);
    const Timeline heavy = make(clips, frei0r, true);
    std::printf("build plain   %8.1f ms\n", msBuild(*prof, plain, 5));
    std::printf("build effects %8.1f ms\n", msBuild(*prof, heavy, 5));
    std::printf("frame plain   %8.1f ms (960x540)\n", msFrames(*prof, plain, 30, 20, 960, 540));
    std::printf("frame effects %8.1f ms (960x540)\n", msFrames(*prof, heavy, 30, 20, 960, 540));
    std::printf("frame effects %8.1f ms (1920x1080)\n", msFrames(*prof, heavy, 30, 10, 1920, 1080));
    // one effect at a time (single clip)
    QStringList each = {"color", "blur", "grade"};
    each += frei0r;
    for (const QString& id : each) {
        Timeline one = make(1, {}, false);
        EffectRegistry::add(one.video[0].clips[0], id);
        std::printf("  %-34s %7.1f ms\n", qPrintable(id), msFrames(*prof, one, 30, 10, 960, 540) );
    }
    auto variant = [&](const char* name, bool fx, bool f0r, bool kfTransform, bool kfFx) {
        Timeline t = make(1, {}, false);
        Clip& c = t.video[0].clips[0];
        if (fx) { EffectRegistry::add(c, "color"); EffectRegistry::add(c, "blur"); EffectRegistry::add(c, "grade"); }
        if (f0r) for (const QString& id : frei0r) EffectRegistry::add(c, id);
        if (kfTransform) for (AnimParam p : {AnimParam::ZoomX, AnimParam::ZoomY, AnimParam::PosX}) {
            Keys::setKey(c, p, 0, Keys::staticValue(c, p)); Keys::setKey(c, p, 99, Keys::staticValue(c, p) + (p == AnimParam::PosX ? 5 : 0.2)); }
        if (kfFx) for (AnimParam p : {AnimParam::FxBrightness, AnimParam::FxBlur, AnimParam::GradeSaturation}) {
            Keys::setKey(c, p, 0, Keys::staticValue(c, p)); Keys::setKey(c, p, 99, Keys::staticValue(c, p) + 5); }
        std::printf("  %-34s %7.1f ms (960) %7.1f ms (480)\n", name, msFrames(*prof, t, 30, 10, 960, 540),
                    msFrames(*prof, t, 30, 10, 480, 270));
    };
    variant("builtin fx", true, false, false, false);
    variant("builtin fx + fx keys", true, false, false, true);
    variant("frei0r all", false, true, false, false);
    variant("all fx", true, true, false, false);
    variant("transform keys", false, false, true, false);
    variant("all fx + transform keys", true, true, true, false);
    variant("all fx + all keys", true, true, true, true);
    {
        Timeline kf = make(1, {}, false);
        Clip& c = kf.video[0].clips[0];
        Keys::setKey(c, AnimParam::ZoomX, 0, 100);
        Keys::setKey(c, AnimParam::ZoomX, 99, 120);
        std::printf("  %-34s %7.1f ms\n", "zoom keyframes", msFrames(*prof, kf, 30, 10, 960, 540));
    }
    // quality: preview (960) vs full render scaled down, zoom 130 % with effects
    {
        Timeline t = make(1, {}, false);
        Clip& c = t.video[0].clips[0];
        EffectRegistry::add(c, "color");
        EffectRegistry::add(c, "grade");
        EffectRegistry::instance(c, "color")->params["saturation"] = 30.0;
        c.transform.zoomX = c.transform.zoomY = 1.3;
        c.transform.posX = 100;
        auto grab = [&](int w, int h) {
            TimelineBuilder b(*prof);
            auto tr = b.build(t);
            tr->seek(30);
            std::unique_ptr<Mlt::Frame> f(tr->get_frame());
            mlt_image_format fm = mlt_image_rgba;
            const uint8_t* d = f->get_image(fm, w, h);
            QImage img(w, h, QImage::Format_RGBA8888);
            std::memcpy(img.bits(), d, size_t(w) * h * 4);
            return img.copy();
        };
        const QImage small = grab(960, 540);
        const QImage ref = grab(1920, 1080).scaled(960, 540, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        double diff = 0;
        for (int y = 0; y < 540; ++y)
            for (int x = 0; x < 960; ++x) {
                const QColor a = small.pixelColor(x, y), b = ref.pixelColor(x, y);
                diff += std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) + std::abs(a.blue() - b.blue());
            }
        std::printf("quality: mean abs diff preview vs full %.2f (0..255 per channel)\n", diff / (960.0 * 540 * 3));
    }
    return 0;
}
