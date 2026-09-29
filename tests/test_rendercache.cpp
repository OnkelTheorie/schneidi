// Test Render-Cache (engine/RenderCache): Schlüssel ändert sich genau bei Änderungen, die das Bild betreffen;
// Modus Aus/Smart/Benutzer; Rendern (mit/ohne Alpha) liefert dasselbe Bild wie die Live-Vorschau; der Builder
// liest die Cache-Datei nur mit ClipCache (Vorschau), Export und Standbild nie; Hintergrund-Rendern per sync().
#include "check.h"

#include "core/EffectRegistry.h"
#include "core/ProjectFile.h"
#include "core/ProjectFormat.h"
#include "engine/Exporter.h"
#include "engine/Profiles.h"
#include "engine/RenderCache.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTimer>
#include <clocale>
#include <cstring>

namespace {

QString g_media;
const ProjectFormat kFmt{640, 360, {25, 1}};

Clip baseClip()
{
    Clip c;
    c.id = 1;
    c.mediaPath = g_media;
    c.in = 10;
    c.out = 39; // 30 Frames
    return c;
}

Timeline one(const Clip& c)
{
    Timeline tl;
    tl.video.resize(1);
    tl.video[0].clips << c;
    return tl;
}

QImage grab(Mlt::Producer& p, int pos, int w, int h)
{
    p.seek(pos);
    std::unique_ptr<Mlt::Frame> f(p.get_frame());
    if (!f) return {};
    mlt_image_format fmt = mlt_image_rgba;
    const uint8_t* d = f->get_image(fmt, w, h);
    if (!d) return {};
    QImage img(w, h, QImage::Format_RGBA8888);
    std::memcpy(img.bits(), d, size_t(w) * h * 4);
    return img.copy();
}

// Timeline rendern; cache: Vorschau-Builder mit Render-Cache
QImage render(const Timeline& tl, int pos, const TimelineBuilder::ClipCache& cache = {})
{
    auto prof = makeProfile(kFmt);
    TimelineBuilder b(*prof);
    if (cache) b.setClipCache(cache);
    auto tr = b.build(tl);
    return grab(*tr, pos, prof->width(), prof->height());
}

// Mittlere Abweichung (0..255) der RGB-Kanäle
double diff(const QImage& a, const QImage& b)
{
    if (a.size() != b.size() || a.isNull()) return 999;
    double s = 0;
    for (int y = 0; y < a.height(); y += 2)
        for (int x = 0; x < a.width(); x += 2) {
            const QColor p = a.pixelColor(x, y), q = b.pixelColor(x, y);
            s += std::abs(p.red() - q.red()) + std::abs(p.green() - q.green()) + std::abs(p.blue() - q.blue());
        }
    return s / (3.0 * (a.width() / 2) * (a.height() / 2));
}

bool isRed(const QImage& img)
{
    const QColor c = img.pixelColor(img.width() / 2, img.height() / 2);
    return c.red() > 200 && c.green() < 60 && c.blue() < 60;
}

void testKeys()
{
    const Clip c = baseClip();
    const QString k = RenderCache::key(c, kFmt);
    auto same = [&](const std::function<void(Clip&)>& fn) {
        Clip d = c;
        fn(d);
        return RenderCache::key(d, kFmt) == k;
    };
    // Betrifft das Bild nicht: Position, ID, Ton, Fades, Übergänge, Verknüpfung, Markierung selbst
    CHECK(same([](Clip& d) { d.start = 500; }));
    CHECK(same([](Clip& d) { d.id = 7; }));
    CHECK(same([](Clip& d) { d.volumeDb = -6; d.pan = 30; d.keepPitch = false; }));
    CHECK(same([](Clip& d) { d.fadeIn = 5; d.fadeOut = 3; }));
    CHECK(same([](Clip& d) { d.transIn = 10; d.transOut = 10; }));
    CHECK(same([](Clip& d) { d.linkId = 3; }));
    CHECK(same([](Clip& d) { d.renderCache = true; }));
    CHECK(same([](Clip& d) { d.keys[AnimParam::Volume] = {{10, -3.0}, {20, 0.0}}; }));
    // Betrifft das Bild: Ausschnitt, Geschwindigkeit, Effekte, Transform, Keyframes
    CHECK(!same([](Clip& d) { d.in = 11; }));
    CHECK(!same([](Clip& d) { d.out = 40; }));
    CHECK(!same([](Clip& d) { d.speed = 0.5; }));
    CHECK(!same([](Clip& d) { d.reverse = true; }));
    CHECK(!same([](Clip& d) { d.freeze = true; }));
    CHECK(!same([](Clip& d) { EffectRegistry::add(d, "blur"); }));
    CHECK(!same([](Clip& d) { d.transform.zoomX = 1.5; }));
    CHECK(!same([](Clip& d) { d.keys[AnimParam::Opacity] = {{10, 100.0}, {30, 0.0}}; }));
    {
        Clip a = c, b = c;
        EffectRegistry::add(a, "blur");
        EffectRegistry::add(b, "blur");
        EffectRegistry::instance(b, "blur")->params["strength"] = 60.0;
        CHECK(RenderCache::key(a, kFmt) != RenderCache::key(b, kFmt)); // Effekt-Parameter
        EffectRegistry::instance(b, "blur")->params["strength"] = EffectRegistry::instance(a, "blur")->params.value("strength");
        EffectRegistry::instance(b, "blur")->enabled = false;
        CHECK(RenderCache::key(a, kFmt) != RenderCache::key(b, kFmt)); // Effekt aus
    }
    // Projektformat und geänderte Quelldatei
    CHECK(RenderCache::key(c, ProjectFormat{1280, 720, {25, 1}}) != k);
    CHECK(RenderCache::key(c, ProjectFormat{640, 360, {50, 1}}) != k);
    {
        QFile f(g_media);
        CHECK(f.open(QIODevice::ReadWrite));
        const QDateTime t = QFileInfo(g_media).lastModified();
        f.setFileTime(t.addSecs(-3600), QFileDevice::FileModificationTime);
        f.close();
        CHECK(RenderCache::key(c, kFmt) != k);
        QFile g(g_media);
        g.open(QIODevice::ReadWrite);
        g.setFileTime(t, QFileDevice::FileModificationTime);
        g.close();
        CHECK_EQ(RenderCache::key(c, kFmt), k);
    }

    // Modus: Aus = nie, Benutzer = nur markierte, Smart = zusätzlich teure; Titel nie
    Clip marked = c;
    marked.renderCache = true;
    Clip fx = c;
    EffectRegistry::add(fx, "blur");
    Clip fast = c;
    fast.speed = 2.0;
    Clip title;
    title.kind = ClipKind::Title;
    title.renderCache = true;
    title.out = 20;
    using M = RenderCache::Mode;
    CHECK(!RenderCache::wanted(marked, M::Off));
    CHECK(RenderCache::wanted(marked, M::User));
    CHECK(!RenderCache::wanted(c, M::User));
    CHECK(!RenderCache::wanted(fx, M::User));
    CHECK(RenderCache::wanted(fx, M::Smart));
    CHECK(RenderCache::wanted(fast, M::Smart));
    CHECK(!RenderCache::wanted(c, M::Smart));
    CHECK(RenderCache::wanted(marked, M::Smart));
    CHECK(!RenderCache::wanted(title, M::User));
    Clip off = marked;
    off.enabled = false;
    CHECK(!RenderCache::wanted(off, M::User));

    // Projektdatei: Markierung wird gespeichert und geladen
    ProjectData data;
    data.timeline = one(marked);
    data.media << MediaInfo{g_media, "src.mp4"};
    ProjectData back;
    QString error;
    CHECK(ProjectFile::fromJson(ProjectFile::toJson(data, {}), {}, &back, &error));
    CHECK(!back.timeline.video.isEmpty() && !back.timeline.video[0].clips.isEmpty()
          && back.timeline.video[0].clips[0].renderCache);
}

void testRender(const QString& dir)
{
    // Clip mit Effekt, Geschwindigkeit und Keyframes: Cache-Datei == Live-Bild
    Clip c = baseClip();
    c.start = 20;
    c.speed = 0.5;
    EffectRegistry::add(c, "color");
    EffectRegistry::instance(c, "color")->params["saturation"] = -60.0;
    c.keys[AnimParam::FxBrightness] = {{10, -40.0}, {39, 40.0}};
    const QString file = dir + "/opaque.mov";
    QString error;
    CHECK(RenderCache::render(c, kFmt, file, &error));
    CHECK_EQ(error, QString());
    CHECK(QFileInfo::exists(file));
    CHECK(!QFileInfo::exists(file + ".part.mov"));

    auto prof = makeProfile(kFmt);
    {
        Mlt::Producer p(*prof, file.toUtf8().constData());
        CHECK(p.is_valid());
        CHECK_EQ(p.get_length(), c.length());
        CHECK_EQ(QByteArray(p.get("meta.media.0.codec.name")), QByteArray("prores"));
        const QImage img = grab(p, 5, 640, 360);
        CHECK(!img.isNull() && img.pixelColor(5, 5).alpha() == 255);
    }
    const Timeline tl = one(c);
    auto cache = [&](const Clip&) { return file; };
    for (int f : {20, 35, 49}) {
        const QImage live = render(tl, f), cached = render(tl, f, cache);
        const double d = diff(live, cached);
        CHECK(d < 2.5) || std::printf("       Frame %d: Abweichung %.2f\n", f, d);
    }

    // Herausgezoomt: Rand durchsichtig -> ProRes 4444 mit Alpha; auf V2 über V1 gleich wie live
    Clip z = baseClip();
    z.start = 0;
    z.transform.zoomX = z.transform.zoomY = 0.5;
    const QString alphaFile = dir + "/alpha.mov";
    CHECK(RenderCache::render(z, kFmt, alphaFile, &error));
    {
        Mlt::Producer p(*prof, alphaFile.toUtf8().constData());
        const QImage img = grab(p, 3, 640, 360);
        CHECK(!img.isNull() && img.pixelColor(5, 5).alpha() == 0);
        CHECK(!img.isNull() && img.pixelColor(320, 180).alpha() == 255);
    }
    Timeline two;
    two.video.resize(2);
    Clip under = baseClip();
    under.id = 2;
    under.start = 0;
    under.in = 60;
    under.out = 89;
    two.video[0].clips << under;
    two.video[1].clips << z;
    const double d = diff(render(two, 3), render(two, 3, [&](const Clip& cl) { return cl.id == z.id ? alphaFile : QString(); }));
    CHECK(d < 2.5) || std::printf("       Alpha: Abweichung %.2f\n", d);

    // Abbrechen: keine (halbe) Datei
    std::atomic<bool> cancel{true};
    const QString cancelled = dir + "/cancelled.mov";
    CHECK(!RenderCache::render(c, kFmt, cancelled, &error, &cancel));
    CHECK(!QFileInfo::exists(cancelled) && !QFileInfo::exists(cancelled + ".part.mov"));
}

void testBuilderUsesCacheOnlyForPreview(const QString& red)
{
    Clip c = baseClip();
    c.renderCache = true;
    const Timeline tl = one(c);
    CHECK(!isRed(render(tl, 5)));                                          // ohne ClipCache (Export)
    CHECK(isRed(render(tl, 5, [&](const Clip&) { return red; })));         // Vorschau mit Cache
    CHECK(!isRed(render(tl, 5, [&](const Clip&) { return QString(); }))); // kein passender Cache

    // Fades wirken weiter über dem Cache
    Clip f = c;
    f.fadeIn = 10;
    const QImage first = render(one(f), 0, [&](const Clip&) { return red; });
    CHECK(!first.isNull() && first.pixelColor(320, 180).red() < 40);

    // Übergänge (Bereich vor dem Clip-Anfang) kommen aus dem Original, der Rest aus dem Cache
    Clip a = baseClip();
    a.id = 5;
    a.in = 0;
    a.out = 29;
    Clip b = c;
    b.id = 6;
    b.start = 30;
    a.transOut = b.transIn = 10; // zentrierte Überblendung 25..35
    Timeline t2;
    t2.video.resize(1);
    t2.video[0].clips << a << b;
    auto cacheB = [&](const Clip& cl) { return cl.id == b.id ? red : QString(); };
    CHECK(!isRed(render(t2, 20, cacheB)));
    CHECK(isRed(render(t2, 45, cacheB)));

    // RenderCache::resolve: nur im passenden Modus und solange der Schlüssel passt
    RenderCache rc;
    rc.setFormat(kFmt);
    rc.setMode(RenderCache::Mode::User);
    QDir().mkpath(RenderCache::cacheDir());
    QFile::remove(rc.path(c));
    CHECK(QFile::copy(red, rc.path(c)));
    CHECK_EQ(rc.resolve(c), rc.path(c));
    Clip moved = c;
    moved.start = 100;
    CHECK_EQ(rc.resolve(moved), rc.path(c)); // Verschieben: Cache bleibt gültig
    Clip changed = c;
    changed.transform.rotation = 10;
    CHECK(rc.resolve(changed).isEmpty()); // geändert: ungültig
    Clip unmarked = c;
    unmarked.renderCache = false;
    CHECK(rc.resolve(unmarked).isEmpty());
    rc.setMode(RenderCache::Mode::Off);
    CHECK(rc.resolve(c).isEmpty());
    rc.setMode(RenderCache::Mode::User);
    const QVector<RenderCache::Span> spans = rc.spans(one(c) );
    CHECK(spans.size() == 1 && spans[0].start == c.start && spans[0].end == c.end() && spans[0].done == 1.0);
    CHECK(rc.spans(one(changed)).value(0).done == 0.0);

    // Export liest nie aus dem Cache (Cache-Datei ist rot)
    const QString out = QFileInfo(red).dir().filePath("export.mp4");
    Exporter ex;
    ExportSettings s;
    s.path = out;
    s.format = kFmt;
    s.crf = 30;
    s.preset = "ultrafast";
    QEventLoop loop;
    bool ok = false;
    QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString&) {
        ok = success;
        loop.quit();
    });
    QString error;
    CHECK(ex.start(tl, s, &error));
    CHECK(Exporter::anyRunning());
    QTimer::singleShot(60000, &loop, &QEventLoop::quit);
    loop.exec();
    CHECK(ok);
    CHECK(!Exporter::anyRunning());
    auto prof = makeProfile(kFmt);
    Mlt::Producer p(*prof, out.toUtf8().constData());
    CHECK(p.is_valid() && !isRed(grab(p, 5, 640, 360)));
    rc.clear();
    CHECK(!QFileInfo::exists(rc.path(c)));
}

// Hintergrund: sync() rendert markierte Clips nach kurzer Ruhe, Änderung macht ungültig -> neu
void testBackground()
{
    RenderCache rc;
    rc.setFormat(kFmt);
    rc.setMode(RenderCache::Mode::User);
    rc.clear();
    Clip c = baseClip();
    c.renderCache = true;
    EffectRegistry::add(c, "blur");
    auto waitFor = [&](const std::function<bool()>& done, int ms) {
        QElapsedTimer t;
        t.start();
        while (!done() && t.elapsed() < ms) {
            QEventLoop loop;
            QTimer::singleShot(50, &loop, &QEventLoop::quit);
            loop.exec();
        }
        return done();
    };
    int changed = 0;
    QObject::connect(&rc, &RenderCache::cacheChanged, [&] { ++changed; });
    rc.sync(one(c));
    CHECK(rc.resolve(c).isEmpty());
    CHECK_EQ(rc.pendingCount(), 1);
    CHECK(waitFor([&] { return !rc.resolve(c).isEmpty(); }, 60000));
    CHECK(changed >= 1);
    CHECK_EQ(rc.pendingCount(), 0);

    // Pausiert während der Wiedergabe
    Clip d = c;
    d.speed = 2.0;
    rc.setPlaying(true);
    rc.sync(one(d));
    CHECK(rc.resolve(d).isEmpty()); // geändert -> ungültig
    waitFor([] { return false; }, 2500);
    CHECK(rc.isRendering());
    CHECK(rc.resolve(d).isEmpty());
    rc.setPlaying(false);
    CHECK(waitFor([&] { return !rc.resolve(d).isEmpty(); }, 60000));

    // Markierung entfernt während des Renderns -> abgebrochen, nichts liegen gelassen
    Clip e = c;
    e.in = 0;
    rc.sync(one(e));
    waitFor([&] { return rc.isRendering(); }, 5000);
    Clip unmarked = e;
    unmarked.renderCache = false;
    rc.sync(one(unmarked));
    CHECK(!rc.isRendering());
    CHECK(!QFileInfo::exists(rc.path(e)));
    const QStringList parts = QDir(RenderCache::cacheDir()).entryList({"*.part.mov"});
    CHECK(parts.isEmpty());
    rc.clear();
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("rendercache");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    g_media = Check::makeMedia(tmp.filePath("src.mp4"),
                               {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=4", "-vf",
                                "scale=out_color_matrix=bt709", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt",
                                "yuv420p", "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709"});
    const QString red = Check::makeMedia(tmp.filePath("red.mov"),
                                         {"-f", "lavfi", "-i", "color=c=red:size=640x360:rate=25:duration=4",
                                          "-c:v", "prores_ks", "-profile:v", "1"});
    if (!CHECK(!g_media.isEmpty() && !red.isEmpty())) return Check::result();

    Mlt::Factory::init();
    std::setlocale(LC_NUMERIC, "C");
    {
        auto prof = makeProfile(kFmt);
        Mlt::Transition qt(*prof, "qtblend");
        if (!qt.is_valid()) return Check::skip("MLT-Qt-Modul nicht nutzbar (kein Display? z. B. xvfb-run -a ctest …)");
    }
    testKeys();
    testRender(tmp.path());
    testBuilderUsesCacheOnlyForPreview(red);
    testBackground();
    return Check::result();
}
