// Test schneidi's own picture effects (engine/VideoEffects), which replaced the frei0r plugins:
// - Green Screen keyer on a synthetic picture: green background (also in shade), skin, gray/white/black, a soft edge
//   with green spill on the subject: background transparent, subject opaque and unchanged, edge in between, the green
//   fringe taken out (spill)
// - the frei0r look-alikes (glow, halftone, noise, scan lines, broken sync, gradient, alpha operations): plausible
//   pixels; next to the frei0r plugin of the same name (only with SCHNEIDI_FREI0R=1 and frei0r installed) the mean
//   difference is printed as a report, not checked
// - render through the TimelineBuilder without any frei0r: keyed clip over black, a glow on a clip
// - old projects: the frei0r effect ids, parameters and keyframes load as known effects; nothing frei0r is loaded
//   while the opt-in is off
// - speed: ms per 1080p frame for every effect (printed)
// Comparison pictures: SCHNEIDI_FREI0R=1 OWN_EFFECTS_DUMP=<folder> build/tests/test_own_effects
#include "check.h"

#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/ProjectFile.h"
#include "core/ProjectFormat.h"
#include "engine/Frei0r.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"
#include "engine/VideoEffects.h"

#include <Mlt.h>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QTemporaryDir>
#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

namespace {

using Pixels = std::vector<uint8_t>;

struct Img {
    int w = 0, h = 0;
    Pixels px;
    uint8_t* at(int x, int y) { return px.data() + (size_t(y) * w + x) * 4; }
    const uint8_t* at(int x, int y) const { return px.data() + (size_t(y) * w + x) * 4; }
};

Img filled(int w, int h, QColor c)
{
    Img im{w, h, Pixels(size_t(w) * h * 4)};
    for (size_t i = 0; i < im.px.size(); i += 4) {
        im.px[i] = uint8_t(c.red());
        im.px[i + 1] = uint8_t(c.green());
        im.px[i + 2] = uint8_t(c.blue());
        im.px[i + 3] = 255;
    }
    return im;
}

void set(Img& im, int x, int y, double r, double g, double b)
{
    uint8_t* p = im.at(x, y);
    p[0] = uint8_t(std::lround(std::clamp(r, 0.0, 255.0)));
    p[1] = uint8_t(std::lround(std::clamp(g, 0.0, 255.0)));
    p[2] = uint8_t(std::lround(std::clamp(b, 0.0, 255.0)));
}

// Green screen scene 240x160: background green (right third in shade), a skin disk with a soft 3 px edge and green
// spill on its outer 6 px, patches of white, gray and black
constexpr int kW = 240, kH = 160, kCx = 80, kCy = 80;
constexpr double kR = 40;
const QColor kGreen(30, 190, 40), kSkin(224, 172, 140);

Img greenScene()
{
    Img im = filled(kW, kH, kGreen);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            double r = kGreen.red(), g = kGreen.green(), b = kGreen.blue();
            if (x >= 160) r *= 0.55, g *= 0.55, b *= 0.55; // shade on the screen
            const double d = std::hypot(x - kCx, y - kCy);
            const double cover = std::clamp((kR - d) / 3.0, 0.0, 1.0); // soft edge 3 px
            if (cover > 0) {
                double sr = kSkin.red(), sg = kSkin.green(), sb = kSkin.blue();
                if (d > kR - 6) sg += 35; // light from the green screen on the subject's edge
                r = r + (sr - r) * cover, g = g + (sg - g) * cover, b = b + (sb - b) * cover;
            }
            set(im, x, y, r, g, b);
        }
    for (int y = 10; y < 40; ++y)
        for (int x = 140; x < 160; ++x) set(im, x, y, 240, 240, 240); // white
    for (int y = 60; y < 90; ++y)
        for (int x = 140; x < 160; ++x) set(im, x, y, 128, 128, 128); // gray
    for (int y = 110; y < 140; ++y)
        for (int x = 140; x < 160; ++x) set(im, x, y, 8, 8, 8); // black
    return im;
}

// Green excess of a pixel: green above the mean of red and blue
double greenExcess(const uint8_t* p) { return p[1] - (p[0] + p[2]) / 2.0; }

double meanLuma(const Img& im)
{
    double s = 0;
    for (size_t i = 0; i < im.px.size(); i += 4) s += 0.2126 * im.px[i] + 0.7152 * im.px[i + 1] + 0.0722 * im.px[i + 2];
    return s / (im.px.size() / 4);
}

// A test picture with structure: horizontal ramp in red, vertical in green, a white square
Img pattern(int w, int h)
{
    Img im = filled(w, h, Qt::black);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const bool square = x > w * 2 / 5 && x < w * 3 / 5 && y > h * 2 / 5 && y < h * 3 / 5;
            set(im, x, y, square ? 250 : 255.0 * x / w, square ? 250 : 255.0 * y / h, square ? 250 : 60);
        }
    return im;
}

double meanAbsDiff(const Img& a, const Img& b)
{
    if (a.px.size() != b.px.size()) return -1;
    double s = 0;
    size_t n = 0;
    for (size_t i = 0; i < a.px.size(); ++i) {
        if (i % 4 == 3) continue;
        s += std::abs(int(a.px[i]) - int(b.px[i]));
        ++n;
    }
    return s / n;
}

// Runs an MLT filter (own or frei0r) on a picture: producer = the picture as a PNG
Img runMlt(Mlt::Profile& profile, const QString& png, Mlt::Filter& f, int w, int h)
{
    Mlt::Producer p(profile, ("qimage:" + png).toUtf8().constData());
    if (!p.is_valid()) p = Mlt::Producer(profile, png.toUtf8().constData());
    if (!p.is_valid()) return {};
    p.attach(f);
    p.seek(0);
    std::unique_ptr<Mlt::Frame> frame(p.get_frame());
    mlt_image_format fmt = mlt_image_rgba;
    const uint8_t* d = frame->get_image(fmt, w, h);
    if (!d) return {};
    Img out{w, h, Pixels(d, d + size_t(w) * h * 4)};
    p.detach(f);
    return out;
}

QString savePng(const Img& im, const QString& path)
{
    QImage q(im.px.data(), im.w, im.h, QImage::Format_RGBA8888);
    q.save(path);
    return path;
}

// ms per frame of fn on a 1920x1080 picture (fastest of 5: the least disturbed by other programs)
double timeIt(const std::function<void(uint8_t*)>& fn)
{
    const Img base = pattern(1920, 1080);
    std::vector<double> times;
    for (int i = 0; i < 5; ++i) {
        Img im = base;
        QElapsedTimer t;
        t.start();
        fn(im.px.data());
        times.push_back(t.nsecsElapsed() / 1e6);
    }
    return *std::min_element(times.begin(), times.end());
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("own_effects");
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");

    // ---- Nothing frei0r while the opt-in is off (the default) ----
    // SCHNEIDI_FREI0R=1 test_own_effects: frei0r loaded instead, prints the comparison with the plugins
    const bool compare = Frei0r::enabled();
    if (!compare) {
        int frei0rFilters = 0;
        Mlt::Repository* repo = Mlt::Factory::init(); // already initialized: returns the repository
        if (CHECK(repo)) {
            std::unique_ptr<Mlt::Properties> filters(repo->filters());
            for (int i = 0; i < filters->count(); ++i)
                if (QString(filters->get_name(i)).startsWith("frei0r.")) ++frei0rFilters;
        }
        CHECK_EQ(frei0rFilters, 0);
        for (const EffectDescriptor& d : EffectRegistry::all()) CHECK(d.category != EffectRegistry::Frei0rCategory);
    }
    // The replaced plugins exist as own effects (same ids and parameter keys)
    for (const char* id : {"chromakey", "frei0r.glow", "frei0r.colorhalftone", "frei0r.rgbnoise", "frei0r.scanline0r",
                           "frei0r.nosync0r", "frei0r.cairogradient", "frei0r.alpha0ps_alpha0ps"}) {
        const EffectDescriptor* d = EffectRegistry::find(id);
        if (!CHECK(d)) continue;
        CHECK(VideoEffects::isOwn(id));
        CHECK(d->video && d->category.isEmpty());
        CHECK(QString(id) == "chromakey" ? !d->library && d->fixed : d->library && !d->fixed);
    }
    if (const EffectDescriptor* d = EffectRegistry::find("frei0r.glow"); CHECK(d && d->params.size() == 1)) {
        CHECK(d->params[0].key == "0" && d->params[0].min == 0 && d->params[0].max == 1);
        CHECK(d->params[0].anim == Keys::effectParam("frei0r.glow", "0"));
    }
    CHECK(!VideoEffects::isOwn("blur") && !VideoEffects::create("blur"));

    // ---- Green Screen ----
    CHECK(VideoEffects::keyDistance(kGreen, kGreen) < 1e-6);
    CHECK(std::abs(VideoEffects::keyDistance(kGreen, QColor(128, 128, 128)) - 0.6) < 0.05);
    CHECK(VideoEffects::keyDistance(kGreen, kSkin) > 0.65);
    CHECK(VideoEffects::keyDistance(kGreen, QColor(17, 105, 22)) < 0.3); // the same green in shade
    {
        const Img scene = greenScene();
        Img keyed = scene;
        VideoEffects::KeyParams kp;
        kp.color = kGreen; // picked with the eyedropper
        VideoEffects::key(keyed.px.data(), kW, kH, kp);
        CHECK_EQ(int(keyed.at(10, 10)[3]), 0);   // background
        CHECK_EQ(int(keyed.at(200, 80)[3]), 0);  // background in shade
        CHECK_EQ(int(keyed.at(kCx, kCy)[3]), 255); // subject
        for (int c = 0; c < 3; ++c) CHECK_EQ(int(keyed.at(kCx, kCy)[c]), int(scene.at(kCx, kCy)[c])); // unchanged
        CHECK_EQ(int(keyed.at(150, 20)[3]), 255);  // white
        CHECK_EQ(int(keyed.at(150, 75)[3]), 255);  // gray
        CHECK_EQ(int(keyed.at(150, 125)[3]), 255); // black
        CHECK_EQ(int(keyed.at(150, 75)[1]), 128);  // gray keeps its color (no spill in it)
        // Soft edge: along a line through the edge alpha rises from 0 to 255 with values in between
        int between = 0;
        for (int x = kCx - int(kR) - 3; x <= kCx - int(kR) + 3; ++x) {
            const int a = keyed.at(x, kCy)[3];
            if (a > 0 && a < 255) ++between;
        }
        CHECK(between >= 1);
        // Fringe: what stays of the outer ring (alpha > 0) is not green any more; without spill suppression it is
        Img raw = scene;
        kp.spill = 0;
        VideoEffects::key(raw.px.data(), kW, kH, kp);
        double fringe = 0, fringeRaw = 0;
        int n = 0;
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < 140; ++x) {
                const double d = std::hypot(x - kCx, y - kCy);
                if (d < kR - 7 || d > kR || keyed.at(x, y)[3] == 0) continue;
                fringe = std::max(fringe, greenExcess(keyed.at(x, y)));
                fringeRaw = std::max(fringeRaw, greenExcess(raw.at(x, y)));
                ++n;
            }
        std::printf("Green Screen fringe: %d pixels, green excess max %.1f (without spill suppression %.1f, skin %.1f)\n",
                    n, fringe, fringeRaw, greenExcess(scene.at(kCx, kCy)));
        CHECK(n > 100);
        CHECK(fringeRaw > 20);
        CHECK(fringe < 20 && fringe < 0.25 * fringeRaw); // the green fringe is gone
        // Colors far from the key keep their tint (yellow has much of green's chroma direction)
        Img yellow = filled(4, 4, QColor(240, 220, 20));
        VideoEffects::KeyParams yk;
        yk.color = kGreen;
        VideoEffects::key(yellow.px.data(), 4, 4, yk);
        CHECK(yellow.at(1, 1)[3] == 255 && std::abs(yellow.at(1, 1)[1] - 220) <= 2 && std::abs(yellow.at(1, 1)[0] - 240) <= 2);
        // Tolerance keeps its meaning: higher = more is keyed; softness 0 = hard edge
        auto keyedCount = [&](double tol, double soft) {
            Img im = scene;
            VideoEffects::KeyParams p;
            p.color = kGreen;
            p.tolerance = tol;
            p.softness = soft;
            VideoEffects::key(im.px.data(), kW, kH, p);
            int t = 0, partial = 0;
            for (size_t i = 3; i < im.px.size(); i += 4) {
                t += im.px[i] == 0;
                partial += im.px[i] > 0 && im.px[i] < 255;
            }
            return std::make_pair(t, partial);
        };
        CHECK(keyedCount(0.05, 0.2).first < keyedCount(0.3, 0.2).first);
        CHECK(keyedCount(0.3, 0.2).first <= keyedCount(0.5, 0.2).first);
        CHECK_EQ(keyedCount(0.3, 0).second, 0);
        // Input alpha is kept (multiplied)
        Img half = filled(4, 4, kSkin);
        half.at(0, 0)[3] = 100;
        VideoEffects::key(half.px.data(), 4, 4, VideoEffects::KeyParams{});
        CHECK_EQ(int(half.at(0, 0)[3]), 100);
    }

    // ---- frei0r look-alikes on pure data ----
    {
        const Img base = pattern(320, 180);
        Img g = base;
        VideoEffects::glow(g.px.data(), g.w, g.h, 0.5);
        CHECK(meanLuma(g) > meanLuma(base) + 10); // brighter
        // the glow spreads light next to the white square
        CHECK(g.at(320 * 2 / 5 - 6, 90)[2] > base.at(320 * 2 / 5 - 6, 90)[2] + 20);

        Img ht = filled(1280, 720, QColor(128, 128, 128));
        VideoEffects::halftone(ht.px.data(), ht.w, ht.h, 0.4, 0.3, 0.45, 0.25);
        int darkPx = 0, paperPx = 0;
        for (size_t i = 0; i < ht.px.size(); i += 4) {
            darkPx += ht.px[i] < 20;
            paperPx += std::abs(ht.px[i] - 128) < 3;
        }
        CHECK(darkPx > 1000 && paperPx > 1000); // dots black, the picture between them
        CHECK(meanLuma(ht) > 15 && meanLuma(ht) < 70); // half ink: dots touch, mostly dark (as frei0r: 33)

        Img nz = filled(320, 180, QColor(128, 128, 128));
        VideoEffects::noise(nz.px.data(), nz.w, nz.h, 0.1, 1);
        double var = 0, mean = 0;
        for (size_t i = 0; i < nz.px.size(); i += 4) var += (nz.px[i] - 128.0) * (nz.px[i] - 128.0), mean += nz.px[i];
        const double sd = std::sqrt(var / (nz.px.size() / 4));
        CHECK(std::abs(sd - 12.5) < 1.5);                       // as frei0r: 125 levels per 1.0
        CHECK(std::abs(mean / (nz.px.size() / 4) - 128) < 0.5); // no shift of the brightness
        Img nz2 = filled(320, 180, QColor(128, 128, 128));
        VideoEffects::noise(nz2.px.data(), nz2.w, nz2.h, 0.2, 2);
        CHECK(nz.px != nz2.px); // other frame, other noise

        Img sl = filled(8, 8, QColor(200, 100, 50));
        VideoEffects::scanlines(sl.px.data(), 8, 8);
        CHECK(sl.at(3, 0)[0] == 200 && sl.at(3, 1)[0] == 100 && sl.at(3, 1)[1] == 50 && sl.at(3, 2)[0] == 200);

        Img ns = base;
        VideoEffects::noSync(ns.px.data(), ns.w, ns.h, 0.25);
        CHECK(std::memcmp(ns.at(30, 50), base.at(30, 95), 4) == 0); // rolled up by a quarter
        CHECK(std::memcmp(ns.at(30, 179), base.at(30, 44), 4) == 0); // the top again at the bottom
        Img ns0 = base;
        VideoEffects::noSync(ns0.px.data(), ns0.w, ns0.h, 0);
        CHECK(ns0.px == base.px);

        Img gr = filled(100, 100, QColor(128, 128, 128));
        VideoEffects::GradientParams gp; // as frei0r: white at the top (start point), black at the bottom, half covering
        VideoEffects::gradient(gr.px.data(), 100, 100, gp);
        CHECK(gr.at(50, 0)[0] > 185 && gr.at(50, 99)[0] < 70 && std::abs(gr.at(50, 50)[0] - 128) < 6);
        Img mul = filled(100, 100, QColor(128, 128, 128));
        gp.blend = "multiply";
        gp.startOpacity = gp.endOpacity = 1;
        VideoEffects::gradient(mul.px.data(), 100, 100, gp);
        CHECK(mul.at(50, 99)[0] < 5 && std::abs(mul.at(50, 0)[0] - 128) < 4); // multiply with white/black
        gp.blend = "hslluminosity";
        Img lum = filled(100, 100, QColor(200, 50, 50));
        VideoEffects::gradient(lum.px.data(), 100, 100, gp);
        CHECK(lum.at(50, 0)[0] > 240 && lum.at(50, 99)[0] < 15);

        // Alpha: a keyed disk; shrink makes the opaque part smaller, grow larger, gray display shows the alpha
        Img disk = greenScene();
        VideoEffects::KeyParams kp;
        kp.color = kGreen;
        VideoEffects::key(disk.px.data(), kW, kH, kp);
        auto opaque = [](const Img& im) {
            int n = 0;
            for (size_t i = 3; i < im.px.size(); i += 4) n += im.px[i] == 255;
            return n;
        };
        Img shrink = disk, grow = disk, gray = disk;
        VideoEffects::AlphaParams ap;
        ap.operation = 0.3;
        ap.amount = 0.5;
        VideoEffects::alphaOps(shrink.px.data(), kW, kH, ap);
        ap.operation = 0.6;
        VideoEffects::alphaOps(grow.px.data(), kW, kH, ap);
        CHECK(opaque(shrink) < opaque(disk) && opaque(grow) > opaque(disk));
        ap = {};
        ap.display = 0.21;
        VideoEffects::alphaOps(gray.px.data(), kW, kH, ap);
        CHECK(gray.at(kCx, kCy)[0] == 255 && gray.at(10, 10)[0] == 0 && gray.at(10, 10)[3] == 255);
    }

    // ---- Next to the frei0r plugins (report only) ----
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    {
        Mlt::Profile profile("atsc_720p_25");
        const QString png = savePng(pattern(1280, 720), tmp.filePath("pattern.png"));
        struct Cmp {
            const char* id;
            QVector<QPair<const char*, const char*>> props;
        };
        const Cmp cmps[] = {{"frei0r.glow", {{"0", "0.5"}}},
                            {"frei0r.colorhalftone", {}},
                            {"frei0r.rgbnoise", {{"0", "0.2"}}},
                            {"frei0r.scanline0r", {}},
                            {"frei0r.nosync0r", {{"0", "0.25"}}},
                            {"frei0r.cairogradient", {}},
                            {"frei0r.alpha0ps_alpha0ps", {{"0", "0.21"}}}};
        for (const Cmp& c : cmps) {
            mlt_filter own = VideoEffects::create(c.id);
            if (!CHECK(own)) continue;
            Mlt::Filter of(own);
            mlt_filter_close(own);
            for (auto [k, v] : c.props) of.set(k, v);
            const Img mine = runMlt(profile, png, of, 1280, 720);
            CHECK(!mine.px.empty());
            Mlt::Filter ff(profile, c.id);
            if (!ff.is_valid()) continue; // frei0r not loaded (opt-in off): no comparison
            for (auto [k, v] : c.props) ff.set(k, v);
            const Img theirs = runMlt(profile, png, ff, 1280, 720);
            std::printf("vs %s: mean difference %.1f levels, mean brightness %.1f (frei0r %.1f)\n", c.id,
                        meanAbsDiff(mine, theirs), meanLuma(mine), meanLuma(theirs));
            if (const QString dump = qEnvironmentVariable("OWN_EFFECTS_DUMP"); !dump.isEmpty()) { // pictures to compare
                savePng(mine, QDir(dump).filePath(QString(c.id) + "-own.png"));
                savePng(theirs, QDir(dump).filePath(QString(c.id) + "-frei0r.png"));
            }
        }
    }

    // ---- Render through the TimelineBuilder (no frei0r loaded) ----
    if (Check::haveFfmpeg()) {
        const QString media = Check::makeMedia(
            tmp.filePath("green.mp4"),
            {"-f", "lavfi", "-i", "color=c=0x1EBE28:size=320x180:rate=25:duration=1", "-vf",
             "drawbox=x=120:y=60:w=80:h=60:color=0xE0AC8C:t=fill", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt",
             "yuv444p"});
        if (CHECK(!media.isEmpty())) {
            ProjectFormat fmt;
            fmt.width = 320;
            fmt.height = 180;
            auto render = [&](const std::function<void(Clip&)>& fn) {
                Timeline tl;
                tl.video.resize(1);
                Clip c;
                c.id = 1;
                c.mediaPath = media;
                c.out = 24;
                fn(c);
                tl.video[0].clips << c;
                auto prof = makeProfile(fmt);
                TimelineBuilder b(*prof);
                auto tr = b.build(tl);
                tr->seek(3);
                std::unique_ptr<Mlt::Frame> f(tr->get_frame());
                mlt_image_format ifmt = mlt_image_rgba;
                int w = prof->width(), h = prof->height();
                const uint8_t* d = f->get_image(ifmt, w, h);
                return d ? Img{w, h, Pixels(d, d + size_t(w) * h * 4)} : Img{};
            };
            const Img plain = render([](Clip&) {});
            const Img keyed = render([](Clip& c) {
                EffectRegistry::add(c, "chromakey");
                EffectRegistry::instance(c, "chromakey")->params["color"] = QColor(0x1E, 0xBE, 0x28);
            });
            if (CHECK(!plain.px.empty() && !keyed.px.empty())) {
                CHECK(plain.at(20, 20)[1] > 150);
                CHECK(keyed.at(20, 20)[0] < 10 && keyed.at(20, 20)[1] < 10); // keyed: black background shows
                CHECK(std::abs(keyed.at(160, 90)[0] - plain.at(160, 90)[0]) < 8); // subject stays
            }
            const Img glowing = render([](Clip& c) {
                EffectRegistry::add(c, "frei0r.glow");
                EffectRegistry::instance(c, "frei0r.glow")->params["0"] = 0.4;
            });
            if (CHECK(!glowing.px.empty())) CHECK(meanLuma(glowing) > meanLuma(plain) + 10);
            // Keyframes on the old dynamic parameter (fx:frei0r.glow:0) drive the own filter
            const AnimParam ga = Keys::effectParam("frei0r.glow", "0");
            const Img keyedGlow = render([&](Clip& c) {
                EffectRegistry::add(c, "frei0r.glow");
                Keys::setKey(c, ga, 0, 0.0);
                Keys::setKey(c, ga, 3, 1.0);
            });
            if (CHECK(!keyedGlow.px.empty())) CHECK(meanLuma(keyedGlow) > meanLuma(plain) + 10);
        }
    }

    // ---- An old project with frei0r effects loads as known effects ----
    {
        const QString path = tmp.filePath("old.schneidi");
        ProjectData pd;
        Clip c;
        c.id = 1;
        c.mediaPath = tmp.filePath("missing.mp4");
        c.out = 49;
        EffectRegistry::add(c, "frei0r.glow");
        EffectRegistry::instance(c, "frei0r.glow")->params["0"] = 0.3;
        EffectRegistry::add(c, "frei0r.rgbnoise");
        EffectRegistry::instance(c, "frei0r.rgbnoise")->params["0"] = 0.1;
        // as an older schneidi saved it: chromakey with only color and distance
        c.effects << EffectInstance{"chromakey", {{"color", QColor(0, 0xef, 0)}, {"distance", 0.25}}, true};
        Keys::setKey(c, Keys::effectParam("frei0r.glow", "0"), 0, 0.1);
        Keys::setKey(c, Keys::effectParam("frei0r.glow", "0"), 20, 0.6);
        pd.timeline.video.resize(1);
        pd.timeline.video[0].clips << c;
        pd.media = {MediaInfo{c.mediaPath, "missing.mp4", 50, true, false, false}};
        QString err;
        ProjectData loaded;
        if (CHECK(ProjectFile::save(pd, path, &err)) && CHECK(ProjectFile::load(path, &loaded, &err))) {
            CHECK(ProjectFile::missingEffects(loaded).isEmpty());
            const Clip& l = loaded.timeline.video[0].clips[0];
            CHECK(l == c);
            CHECK(qAbs(Keys::valueAt(l, Keys::effectParam("frei0r.glow", "0"), 10) - 0.35) < 1e-9);
            // the new key parameters are missing in the old file: their defaults apply
            CHECK(qAbs(EffectRegistry::value(l, "chromakey", "softness").toDouble() - 0.2) < 1e-9);
            CHECK(qAbs(EffectRegistry::value(l, "chromakey", "spill").toDouble() - 1.0) < 1e-9);
        }
    }

    // ---- Speed (ms per 1080p frame, one thread) ----
    {
        VideoEffects::KeyParams kp;
        std::printf("ms per 1080p frame: key %.1f", timeIt([&](uint8_t* p) { VideoEffects::key(p, 1920, 1080, kp); }));
        std::printf(", glow %.1f", timeIt([](uint8_t* p) { VideoEffects::glow(p, 1920, 1080, 0.5); }));
        std::printf(", halftone %.1f", timeIt([](uint8_t* p) { VideoEffects::halftone(p, 1920, 1080, 0.4, 0.3, 0.45, 0.25); }));
        std::printf(", noise %.1f", timeIt([](uint8_t* p) { VideoEffects::noise(p, 1920, 1080, 0.2, 7); }));
        std::printf(", scanlines %.1f", timeIt([](uint8_t* p) { VideoEffects::scanlines(p, 1920, 1080); }));
        std::printf(", nosync %.1f", timeIt([](uint8_t* p) { VideoEffects::noSync(p, 1920, 1080, 0.25); }));
        VideoEffects::GradientParams gp;
        std::printf(", gradient %.1f", timeIt([&](uint8_t* p) { VideoEffects::gradient(p, 1920, 1080, gp); }));
        VideoEffects::AlphaParams ap;
        ap.operation = 0.4;
        std::printf(", alpha shrink soft %.1f\n", timeIt([&](uint8_t* p) { VideoEffects::alphaOps(p, 1920, 1080, ap); }));
    }
    return Check::result();
}
