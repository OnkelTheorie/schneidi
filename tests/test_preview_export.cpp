// The preview (Engine, rendered directly at 960 px) must show the same picture as the export (full project size):
// pixel values (title size and position, outline, transform position, crop, blur) refer to the project format.
// Regression: the preview consumer's width/height went into the shared profile, so the preview took 960x540 for the
// project size and drew all of these twice as large.
#include "check.h"

#include "core/EffectRegistry.h"
#include "engine/Engine.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QApplication>
#include <QEventLoop>
#include <QImage>
#include <QTemporaryDir>
#include <QTimer>
#include <cstring>
#include <functional>

namespace {

Clip media(int id, const QString& path, int start, int len)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.in = 0;
    c.out = len - 1;
    return c;
}

// Export path: full project size, scaled down like the Viewer
QImage exportFrame(const Timeline& tl, const ProjectFormat& f, int pos, QSize size)
{
    auto profile = makeProfile(f);
    TimelineBuilder b(*profile);
    auto tractor = b.build(tl);
    tractor->seek(pos);
    std::unique_ptr<Mlt::Frame> fr(tractor->get_frame());
    mlt_image_format fmt = mlt_image_rgba;
    int w = profile->width(), h = profile->height();
    const uint8_t* d = fr->get_image(fmt, w, h);
    QImage img(w, h, QImage::Format_RGBA8888);
    std::memcpy(img.bits(), d, size_t(w) * h * 4);
    return img.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

// The last frame the Viewer got (the first ones may still be black while the decoder starts)
QImage previewFrame(Engine& e, const Timeline& tl, int pos)
{
    QImage shown;
    auto conn = QObject::connect(&e, &Engine::frameReady, &e, [&](const QImage& im) { shown = im; });
    e.updateTimeline(tl);
    e.showTimeline(pos);
    QEventLoop loop;
    QTimer::singleShot(1500, &loop, &QEventLoop::quit);
    loop.exec();
    e.seek(pos); // shown again once everything is decoded
    QTimer::singleShot(1500, &loop, &QEventLoop::quit);
    loop.exec();
    QObject::disconnect(conn);
    return shown;
}

// Mean absolute difference per channel (0..255)
double difference(const QImage& a, const QImage& b)
{
    if (a.size() != b.size() || a.isNull()) return 255;
    const QImage x = a.convertToFormat(QImage::Format_RGB32), y = b.convertToFormat(QImage::Format_RGB32);
    double sum = 0;
    for (int j = 0; j < x.height(); ++j)
        for (int i = 0; i < x.width(); ++i) {
            const QRgb p = x.pixel(i, j), q = y.pixel(i, j);
            sum += std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q)) + std::abs(qBlue(p) - qBlue(q));
        }
    return sum / (3.0 * x.width() * x.height());
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("preview-export");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg fehlt");
    QTemporaryDir dir;
    const QString bars = Check::makeMedia(dir.filePath("bars.mp4"), {"-f", "lavfi", "-i", "smptebars=size=1920x1080:rate=25:duration=4",
                                                                     "-c:v", "libx264", "-qp", "0", "-pix_fmt", "yuv420p"});
    const QString grid = Check::makeMedia(dir.filePath("grid.mp4"), {"-f", "lavfi", "-i", "testsrc2=size=1920x1080:rate=25:duration=4",
                                                                     "-c:v", "libx264", "-qp", "0", "-pix_fmt", "yuv420p"});
    if (!CHECK(!bars.isEmpty() && !grid.isEmpty())) return Check::result();

    Engine engine;
    QString err;
    if (!engine.init(&err)) return Check::skip("kein MLT-Consumer");
    ProjectFormat f;
    engine.setFormat(f);

    struct Case {
        const char* name;
        std::function<void(Timeline&)> setup;
    };
    const QVector<Case> cases = {
        {"Titel mit Versatz, Kontur und Box", [&](Timeline& tl) {
             Clip t;
             t.id = 10;
             t.kind = ClipKind::Title;
             t.start = 0;
             t.out = 99;
             t.title.text = "Zeile eins\nzweite Zeile";
             t.title.size = 110;
             t.title.bold = true;
             t.title.outlineOn = true;
             t.title.outlineWidth = 8;
             t.title.boxOn = true;
             t.title.posX = -250;
             t.title.posY = -280;
             tl.video[1].clips << t;
         }},
        {"Bild im Bild (Zoom, Position, Crop)", [&](Timeline& tl) {
             Clip c = media(20, grid, 0, 100);
             c.transform.zoomX = c.transform.zoomY = 0.45;
             c.transform.posX = 420;
             c.transform.posY = 220;
             c.transform.cropLeft = 200;
             c.transform.cropTop = 100;
             tl.video[1].clips << c;
         }},
        {"Unschärfe", [&](Timeline& tl) {
             Clip& c = tl.video[0].clips[0];
             EffectRegistry::add(c, "blur");
         }},
    };
    for (const Case& k : cases) {
        Timeline tl;
        tl.video.resize(2);
        tl.video[0].clips << media(1, bars, 0, 100);
        k.setup(tl);
        const QImage shown = previewFrame(engine, tl, 25);
        if (!CHECK(!shown.isNull())) continue;
        const QImage want = exportFrame(tl, f, 25, shown.size());
        const double d = difference(shown, want);
        if (!Check::report(d < 4.0, k.name, __FILE__, __LINE__, QString("mittlere Abweichung %1").arg(d)) &&
            qEnvironmentVariableIsSet("PREVIEW_EXPORT_DUMP")) {
            shown.save(qEnvironmentVariable("PREVIEW_EXPORT_DUMP") + QString("/%1_preview.png").arg(k.name));
            want.save(qEnvironmentVariable("PREVIEW_EXPORT_DUMP") + QString("/%1_export.png").arg(k.name));
        }
    }
    return Check::result();
}
