// Own compositing (engine/video/Compose) instead of MLT's qtblend (Qt module, GPL): the same timelines render the same
// picture as with qtblend. Positions, scale, rotation, opacity, crop, alpha edges, odd sizes, 4:3 and 9:16 in 16:9,
// a track over another, titles with outline/box (also with animated size), stills with alpha (PNG) and JPG, at export
// size and at preview size, RGBA (preview) and YUV (export). Mean and largest difference per case are printed; a bit
// of difference at sub-pixel edges is fine (bilinear rounding), the mean stays small. Without MLT's Qt module there is
// nothing to compare against: skipped with a message (the own services are covered by the other render tests).

#include "check.h"

#include "core/Keyframes.h"
#include "core/ProjectFormat.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"
#include "engine/video/Compose.h"

#include <Mlt.h>
#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <clocale>
#include <cmath>
#include <functional>

namespace {

const ProjectFormat kFmt{640, 360, {25, 1}};

struct Picture {
    int w = 0, h = 0, channels = 4;
    QVector<uint8_t> px;
};

Picture render(const Timeline& tl, mlt_image_format format, double scale, int frame)
{
    auto profile = makeProfile(kFmt);
    TimelineBuilder b(*profile);
    auto tractor = b.build(tl);
    tractor->seek(frame);
    std::unique_ptr<Mlt::Frame> f(tractor->get_frame());
    Picture pic;
    pic.w = int(kFmt.width * scale);
    pic.h = int(kFmt.height * scale);
    pic.channels = format == mlt_image_rgba ? 4 : 1;
    mlt_image_format got = format;
    int w = pic.w, h = pic.h;
    const uint8_t* img = f->get_image(got, w, h);
    if (!img || w != pic.w || h != pic.h || got != format) {
        std::printf("       render: %dx%d %s instead of %dx%d %s\n", w, h, mlt_image_format_name(got), pic.w, pic.h,
                    mlt_image_format_name(format));
        return {};
    }
    pic.px.assign(img, img + size_t(pic.channels) * w * h); // YUV: the luma plane
    return pic;
}

struct Diff {
    double mean = 0; // over all channels and pixels
    int max = 0;
    double over16 = 0; // share of values that differ by more than 16
    double level = 0;  // mean value of the own picture (a black or empty render would compare equal too)
};

Diff compare(const Picture& a, const Picture& b)
{
    Diff d;
    if (a.px.size() != b.px.size() || a.px.isEmpty()) {
        d.max = 999;
        return d;
    }
    double sum = 0, level = 0;
    size_t big = 0, n = 0;
    for (qsizetype i = 0; i < a.px.size(); ++i) {
        if (a.channels == 4 && i % 4 == 3) continue; // the output is opaque (black background)
        const int v = std::abs(int(a.px[i]) - int(b.px[i]));
        sum += v;
        level += b.px[i];
        d.max = std::max(d.max, v);
        big += v > 16;
        ++n;
    }
    d.mean = sum / double(n);
    d.over16 = double(big) / double(n);
    d.level = level / double(n);
    return d;
}

Clip clip(int id, const QString& media, const std::function<void(Clip&)>& setup = {})
{
    Clip c;
    c.id = id;
    c.mediaPath = media;
    c.out = 24;
    if (setup) setup(c);
    return c;
}

Timeline single(const Clip& c)
{
    Timeline tl;
    tl.video.resize(1);
    tl.video[0].clips << c;
    return tl;
}

Timeline stacked(const Clip& low, const Clip& up)
{
    Timeline tl;
    tl.video.resize(2);
    tl.video[0].clips << low;
    tl.video[1].clips << up;
    return tl;
}

Clip title(int id, const std::function<void(TitleStyle&)>& setup)
{
    Clip t;
    t.id = id;
    t.kind = ClipKind::Title;
    t.out = 24;
    t.title.text = "Hallo Welt\nzweite Zeile";
    t.title.size = 60;
    setup(t.title);
    return t;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("compose");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    auto video = [&](const char* name, const char* size) {
        return Check::makeMedia(tmp.filePath(name), {"-f", "lavfi", "-i", QString("testsrc2=size=%1:rate=25:duration=2").arg(size),
                                                     "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    };
    const QString wide = video("wide.mp4", "640x360"), narrow = video("narrow.mp4", "480x360"),
                  tall = video("tall.mp4", "360x640"), odd = video("odd.mp4", "334x198"),
                  big = video("big.mp4", "1280x720");
    // Still with alpha: a soft-edged half-transparent disc and an opaque bar on transparency, odd size
    QImage alpha(301, 187, QImage::Format_ARGB32);
    alpha.fill(Qt::transparent);
    {
        QPainter p(&alpha);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(QColor(255, 80, 20, 170));
        p.setPen(Qt::NoPen);
        p.drawEllipse(QRectF(10.5, 12.3, 200, 150));
        p.setBrush(QColor(30, 200, 255));
        p.drawRect(QRectF(150.25, 40.5, 120, 33));
    }
    const QString pngFile = tmp.filePath("alpha.png");
    CHECK(alpha.save(pngFile));
    QImage photo(257, 143, QImage::Format_RGB32);
    for (int y = 0; y < photo.height(); ++y)
        for (int x = 0; x < photo.width(); ++x) photo.setPixel(x, y, qRgb(x, (x * y) % 256, y * 255 / photo.height()));
    const QString jpgFile = tmp.filePath("photo.jpg");
    CHECK(photo.save(jpgFile, "JPG", 92));
    if (!CHECK(!wide.isEmpty() && !narrow.isEmpty() && !tall.isEmpty() && !odd.isEmpty() && !big.isEmpty()))
        return Check::result();
    qputenv("SCHNEIDI_MLT_QT", "1"); // MLT's Qt module too: the old services to compare with
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    // The real module, not the own services standing in for its names (they come without metadata)
    if (!mlt_repository_metadata(mlt_factory_repository(), mlt_service_transition_type, "qtblend")
        || !mlt_repository_metadata(mlt_factory_repository(), mlt_service_producer_type, "qimage"))
        return Check::skip("MLT's Qt module (qtblend, qimage) is not there: nothing to compare against");

    auto zoomed = [](double z, double x = 0, double y = 0, double rot = 0) {
        return [=](Clip& c) {
            c.transform.zoomX = c.transform.zoomY = z;
            c.transform.posX = x;
            c.transform.posY = y;
            c.transform.rotation = rot;
        };
    };
    struct Case {
        const char* name;
        Timeline tl;
    };
    // ref: the old services (pictures through MLT's qimage), else the own ones
    auto makeCases = [&](bool ref) {
        const auto still = [ref](const QString& path) { return (ref ? "qimage:" : "schneidi_still:") + path; };
        const QString png = still(pngFile), jpg = still(jpgFile);
        return QVector<Case>{
            {"zoom 0.5, moved", single(clip(1, wide, zoomed(0.5, 101.3, -47.6)))},
            {"zoom 1.37 (fills)", single(clip(1, wide, zoomed(1.37, 13.5, 7.25)))},
            {"zoom x/y apart", single(clip(1, wide, [](Clip& c) { c.transform.zoomX = 0.8, c.transform.zoomY = 0.55; }))},
            {"rotated 30", single(clip(1, wide, zoomed(0.7, 0, 0, 30)))},
            {"rotated -7.5, moved", single(clip(1, wide, zoomed(0.9, -60, 25, -7.5)))},
            {"rotated 90", single(clip(1, wide, zoomed(0.5, 0, 0, 90)))},
            {"opacity 60", single(clip(1, wide, [](Clip& c) {
                 c.transform.zoomX = c.transform.zoomY = 0.8;
                 c.transform.opacity = 60;
             }))},
            {"crop", single(clip(1, wide, [](Clip& c) {
                 c.transform.cropLeft = 40.5;
                 c.transform.cropTop = 20;
                 c.transform.cropRight = 100;
                 c.transform.cropBottom = 33.3;
             }))},
            {"crop + zoom + rotation", single(clip(1, wide, [](Clip& c) {
                 c.transform.cropLeft = 60;
                 c.transform.cropBottom = 50;
                 c.transform.zoomX = c.transform.zoomY = 0.75;
                 c.transform.rotation = 12;
             }))},
            {"4:3 in 16:9", single(clip(1, narrow, zoomed(1.0, 30, 0)))},
            {"4:3 in 16:9, zoom 1", single(clip(1, narrow))},
            {"9:16 in 16:9", single(clip(1, tall, zoomed(1.2, -100, 10)))},
            {"odd size 334x198", single(clip(1, odd, zoomed(0.83, 3.3, 1.1, 3)))},
            {"source larger than the timeline", single(clip(1, big, zoomed(0.6, 20, 20, 5)))},
            {"track over track", stacked(clip(1, wide), clip(2, narrow, zoomed(0.45, 150, 80, -20)))},
            {"track over track, opacity", stacked(clip(1, tall, zoomed(1.5)), clip(2, wide, [](Clip& c) {
                 c.transform.zoomX = c.transform.zoomY = 0.6;
                 c.transform.opacity = 45;
             }))},
            {"PNG with alpha", single(clip(1, png, zoomed(1.0)))},
            {"PNG with alpha, rotated", stacked(clip(1, wide), clip(2, png, zoomed(0.9, 40, -30, 25)))},
            {"JPG odd size, moved", stacked(clip(1, wide), clip(2, jpg, zoomed(0.66, -77.7, 33.3)))},
            {"title", stacked(clip(1, wide), title(2, [](TitleStyle&) {}))},
            {"title with outline and box", stacked(clip(1, wide), title(2, [](TitleStyle& t) {
                 t.outlineOn = true;
                 t.outlineWidth = 3;
                 t.boxOn = true;
                 t.posX = -40;
                 t.posY = 50;
             }))},
            {"title zoomed and rotated", stacked(clip(1, wide), [] {
                 Clip t = title(2, [](TitleStyle& s) { s.outlineOn = true; });
                 t.transform.zoomX = t.transform.zoomY = 0.8;
                 t.transform.rotation = 10;
                 return t;
             }())},
            {"title with animated size", stacked(clip(1, wide), [] {
                 Clip t = title(2, [](TitleStyle& s) { s.color = QColor(255, 220, 0); });
                 t.keys[AnimParam::TitleSize] = {Keyframe{0, 40}, Keyframe{24, 90}};
                 return t;
             }())},
            {"wipe in from nothing, border", single(clip(1, wide, [](Clip& c) {
                 c.transIn = 12;
                 c.transInAlone = true;
                 c.transInStyle.type = TransitionType::WipeRight;
                 c.transInStyle.border = 6;
             }))},
            {"dip to color between clips", [&] {
                 Timeline tl;
                 tl.video.resize(1);
                 Clip a = clip(1, wide, [](Clip& c) {
                     c.out = 19;
                     c.transOut = 10;
                     c.transOutStyle.type = TransitionType::DipToColor;
                     c.transOutStyle.color = QColor(40, 90, 200);
                 });
                 Clip b = clip(2, narrow, [](Clip& c) {
                     c.start = 20;
                     c.in = 10;
                     c.out = 40;
                     c.transIn = 10;
                     c.transInStyle = TransitionStyle{};
                     c.transInStyle.type = TransitionType::DipToColor;
                     c.transInStyle.color = QColor(40, 90, 200);
                 });
                 tl.video[0].clips << a << b;
                 return tl;
             }()},
            {"animated zoom and rotation", single(clip(1, wide, [](Clip& c) {
                 c.keys[AnimParam::ZoomX] = {Keyframe{0, 0.4}, Keyframe{24, 1.1}};
                 c.keys[AnimParam::ZoomY] = {Keyframe{0, 0.4}, Keyframe{24, 1.1}};
                 c.keys[AnimParam::Rotation] = {Keyframe{0, -20}, Keyframe{24, 40}};
             }))},
            {"still PNG fitted (no transform)", stacked(clip(1, wide), clip(2, png))},
            {"still JPG fitted (no transform)", single(clip(1, jpg))},
        };
        };
    const QVector<Case> refCases = makeCases(true), cases = makeCases(false);
    struct Size {
        const char* name;
        mlt_image_format format;
        double scale;
    };
    const Size sizes[] = {{"export RGBA", mlt_image_rgba, 1.0}, {"preview RGBA", mlt_image_rgba, 0.5},
                          {"export YUV", mlt_image_yuv420p, 1.0}};
    double worstMean = 0;
    // Arguments: only cases whose name contains the first one; SCHNEIDI_COMPOSE_DUMP=<folder> saves both pictures
    const QString only = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    const QString dump = qEnvironmentVariable("SCHNEIDI_COMPOSE_DUMP");
    for (qsizetype i = 0; i < cases.size(); ++i)
        for (const Size& s : sizes)
            for (int frame : {3, 17}) {
                const Case& c = cases[i];
                if (!only.isEmpty() && !QString(c.name).contains(only)) continue;
                Compose::setQtReference(true);
                const Picture ref = render(refCases[i].tl, s.format, s.scale, frame);
                Compose::setQtReference(false);
                const Picture own = render(c.tl, s.format, s.scale, frame);
                const Diff d = compare(ref, own);
                if (!dump.isEmpty() && s.format == mlt_image_rgba && !ref.px.isEmpty() && !own.px.isEmpty()) {
                    const QString base = QString("%1/%2-%3-%4").arg(dump, QString(c.name).replace(' ', '_')).arg(s.scale).arg(frame);
                    QImage(ref.px.constData(), ref.w, ref.h, QImage::Format_RGBA8888).save(base + "-ref.png");
                    QImage(own.px.constData(), own.w, own.h, QImage::Format_RGBA8888).save(base + "-own.png");
                }
                worstMean = std::max(worstMean, d.mean);
                // Text: glyph and box edges land a fraction of a pixel apart (own layout with QPainterPath); crop edges in
                // the preview are cut at preview size (qtblend cut at media size and scaled)
                const bool text = QString(c.name).contains("title"), crop = QString(c.name).startsWith("crop");
                const double meanLimit = text ? 2.5 : 1.0, edgeLimit = text ? 0.035 : crop ? 0.02 : 0.01;
                const bool ok = d.mean < meanLimit && d.over16 < edgeLimit && d.level > 8;
                std::printf("  %s %-34s %-13s frame %2d: mean %.3f max %3d >16: %.3f%% (level %.0f)\n", ok ? "  " : "!!",
                            c.name, s.name, frame, d.mean, d.max, d.over16 * 100, d.level);
                CHECK(ok);
            }
    std::printf("  worst mean difference %.3f\n", worstMean);
    return Check::result();
}
