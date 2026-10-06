// Export mit mehreren Threads: Bilder parallel (außer frei0r/Green Screen), gleiches Ergebnis wie mit einem Thread.
// Gibt die Renderzeiten aus (1 Thread / alle Threads), damit sich der Gewinn nachmessen lässt.

#include "check.h"

#include "core/EffectRegistry.h"
#include "engine/Exporter.h"
#include "engine/Profiles.h"

#include <Mlt.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QTemporaryDir>
#include <QTimer>
#include <clocale>
#include <cstring>
#include <memory>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace {

QString g_media;
ProjectFormat kFmt{1920, 1080, {25, 1}};
constexpr int kFrames = 100;

Clip effectClip(bool chromakey)
{
    Clip c;
    c.id = 1;
    c.mediaPath = g_media;
    c.in = 0;
    c.out = kFrames - 1;
    EffectRegistry::add(c, "color");
    EffectRegistry::instance(c, "color")->params["saturation"] = 40.0;
    EffectRegistry::add(c, "blur");
    if (chromakey) EffectRegistry::add(c, "chromakey");
    return c;
}

Timeline one(const Clip& c)
{
    Timeline tl;
    tl.video.resize(1);
    tl.video[0].clips << c;
    return tl;
}

void testParallelFrames()
{
    const Timeline plain = one(effectClip(false));
    CHECK_EQ(Exporter::parallelFrames(plain, 0), Exporter::availableThreads());
    CHECK_EQ(Exporter::parallelFrames(plain, 3), 3);
    CHECK_EQ(Exporter::parallelFrames(one(effectClip(true)), 0), 1);
    // ausgeschalteter Green Screen zählt nicht
    Clip off = effectClip(true);
    EffectRegistry::instance(off, "chromakey")->enabled = false;
    CHECK_EQ(Exporter::parallelFrames(one(off), 4), 4);
    // Green Screen in einer verschachtelten Sequenz (Compound Clip)
    Timeline outer = plain;
    auto nested = std::make_shared<NestedTimelines>();
    nested->insert(7, one(effectClip(true)));
    outer.nested = nested;
    CHECK_EQ(Exporter::parallelFrames(outer, 4), 1);
}

// Rechenzeit des Prozesses (alle Threads) in s; 0 = unbekannt
double cpuSeconds()
{
#ifdef Q_OS_WIN
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    auto s = [](const FILETIME& f) { return (double(f.dwHighDateTime) * 4294967296.0 + f.dwLowDateTime) / 1e7; };
    return s(kernel) + s(user);
#else
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
#endif
}
double g_cpuLoad = 0; // letzter render(): Rechenzeit / Laufzeit = im Mittel belegte Prozessoren

// Rendert und liefert die Dauer in ms (-1 = fehlgeschlagen)
qint64 render(const Timeline& tl, const QString& path, int threads)
{
    Exporter ex;
    ExportSettings s;
    s.path = path;
    s.format = kFmt;
    s.crf = qEnvironmentVariableIsSet("SCHNEIDI_BENCH_CLIP") ? 20 : 23; // Messung: wie die Deliver-Seite (Qualität Hoch, medium)
    s.preset = qEnvironmentVariableIsSet("SCHNEIDI_BENCH_CLIP") ? "medium" : "veryfast";
    s.threads = threads;
    QEventLoop loop;
    bool ok = false;
    QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString&) {
        ok = success;
        loop.quit();
    });
    QElapsedTimer t;
    t.start();
    const double cpu0 = cpuSeconds();
    QString error;
    if (!CHECK(ex.start(tl, s, &error))) return -1;
    QTimer::singleShot(900000, &loop, &QEventLoop::quit);
    loop.exec();
    g_cpuLoad = (cpuSeconds() - cpu0) * 1000.0 / std::max<qint64>(1, t.elapsed());
    return CHECK(ok) ? t.elapsed() : -1;
}

QImage grab(const QString& path, int pos)
{
    auto prof = makeProfile(kFmt);
    Mlt::Producer p(*prof, path.toUtf8().constData());
    if (!p.is_valid()) return {};
    p.seek(pos);
    std::unique_ptr<Mlt::Frame> f(p.get_frame());
    mlt_image_format fmt = mlt_image_rgba;
    int w = kFmt.width, h = kFmt.height;
    const uint8_t* img = f ? f->get_image(fmt, w, h) : nullptr;
    if (!img) return {};
    return QImage(img, w, h, QImage::Format_RGBA8888).copy();
}

// mittlere Abweichung pro Kanal (0..255), Stichprobe
double diff(const QImage& a, const QImage& b)
{
    if (a.size() != b.size() || a.isNull()) return 255;
    double sum = 0;
    int n = 0;
    for (int y = 0; y < a.height(); y += 7)
        for (int x = 0; x < a.width(); x += 7) {
            const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
            sum += std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q)) + std::abs(qBlue(p) - qBlue(q));
            n += 3;
        }
    return sum / n;
}

void testRender(const QString& dir)
{
    const Timeline tl = one(effectClip(false));
    const QString single = dir + "/one.mp4", all = dir + "/all.mp4";
    const qint64 t1 = render(tl, single, 1);
    const qint64 tn = render(tl, all, 0);
    std::printf("Export 1080p, %d Bilder, Farbkorrektur + Unschärfe: 1 Thread %lld ms, alle (%d) %lld ms\n", kFrames,
                t1, Exporter::availableThreads(), tn);
    // gleiche Bilder in gleicher Reihenfolge (parallel darf nichts vertauschen)
    for (int pos : {0, 37, kFrames - 1}) {
        const double d = diff(grab(single, pos), grab(all, pos));
        if (!CHECK(d < 2.0)) std::printf("       Bild %d: Abweichung %.2f\n", pos, d);
    }
    auto prof = makeProfile(kFmt);
    Mlt::Producer p(*prof, all.toUtf8().constData());
    CHECK_EQ(p.get_length(), kFrames);

    // Green Screen: läuft trotz „alle Threads“ (ein Bild nach dem anderen) sauber durch
    Timeline key = one(effectClip(true));
    key.video[0].clips[0].out = 24;
    const qint64 tk = render(key, dir + "/key.mp4", 0);
    std::printf("Export mit Green Screen (25 Bilder): %lld ms\n", tk);
}

// Messung mit einer echten Datei (nicht Teil des normalen Testlaufs):
//   SCHNEIDI_BENCH_CLIP=<Datei> [SCHNEIDI_BENCH_CORES=1,0] [SCHNEIDI_BENCH_FPS=60] test_export_threads
// Clip ganz auf V1 + A1 (Schnitt ohne Effekte) und noch einmal mit Farbkorrektur
int bench(const QString& clip)
{
    kFmt = ProjectFormat{1920, 1080, {qEnvironmentVariableIntValue("SCHNEIDI_BENCH_FPS") > 0
                                          ? qEnvironmentVariableIntValue("SCHNEIDI_BENCH_FPS") : 60, 1}};
    auto prof = makeProfile(kFmt);
    Mlt::Producer src(*prof, clip.toUtf8().constData());
    if (!CHECK(src.is_valid())) return Check::result();
    Clip c;
    c.id = 1;
    c.mediaPath = clip;
    c.out = src.get_length() - 1;
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    tl.video[0].clips << c;
    c.id = 2;
    tl.audio[0].clips << c;
    Timeline graded = tl;
    EffectRegistry::add(graded.video[0].clips[0], "color");
    EffectRegistry::instance(graded.video[0].clips[0], "color")->params["saturation"] = 40.0;
    QTemporaryDir tmp;
    const QStringList threadList = qEnvironmentVariable("SCHNEIDI_BENCH_THREADS", qEnvironmentVariable("SCHNEIDI_BENCH_CORES", "1,0")).split(',');
    for (const auto& [name, t] : {std::pair<const char*, const Timeline*>{"Schnitt", &tl}, {"Farbkorrektur", &graded}})
        for (const QString& n : threadList) {
            const qint64 ms = render(*t, tmp.filePath("bench.mp4"), n.toInt());
            std::printf("%s, %d Bilder, Threads %s: %lld ms (%.1f Bilder/s), im Mittel %.1f Prozessoren belegt\n", name, c.out + 1,
                        n == "0" ? qPrintable(QString("alle (%1)").arg(Exporter::availableThreads())) : qPrintable(n), ms,
                        ms > 0 ? (c.out + 1) * 1000.0 / ms : 0.0, g_cpuLoad);
            std::fflush(stdout);
        }
    return Check::result();
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("export_threads");
    if (qEnvironmentVariableIsSet("SCHNEIDI_BENCH_CLIP")) {
        Check::initMlt();
        std::setlocale(LC_NUMERIC, "C");
        return bench(qEnvironmentVariable("SCHNEIDI_BENCH_CLIP"));
    }
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    g_media = Check::makeMedia(tmp.filePath("src.mp4"),
                               {"-f", "lavfi", "-i", "testsrc2=size=1920x1080:rate=25:duration=4", "-c:v", "libx264",
                                "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!g_media.isEmpty())) return Check::result();

    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    testParallelFrames();
    testRender(tmp.path());
    return Check::result();
}
