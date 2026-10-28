// SPDX-License-Identifier: AGPL-3.0-only
// Speed brake: exports a few typical timelines and measures them against FFmpeg alone with the same encoder settings
// (schneidi time / FFmpeg time, wall clock and CPU time). Red when a ratio is more than 15 % above the baseline of
// this machine (tests/data/speed-baseline.json) – the tests elsewhere only check *that* an export is right, so a
// slow path (BT.601 sources at 64 instead of ~145 fps, 2026-10-20) went unnoticed for three weeks.
// Also: Exporter::start() must not block the GUI thread (it opened every file there, 6 s for a real project).
// No baseline for this machine: prints the numbers and passes. Write/update it on an idle machine with
//   SPEED_BASELINE=write ctest -R test_speed
// A busy machine (other programs use more than 20 % of the processors) measures nothing useful: then the test is skipped.
#include "check.h"

#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "engine/Exporter.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cstdio>
#ifndef Q_OS_WIN
#include <sys/resource.h>
#endif

namespace {

const ProjectFormat kFmt{1920, 1080, {60, 1}};
constexpr int kSeconds = 5;
constexpr int kFrames = kSeconds * 60;
constexpr double kBrake = 1.15; // more than 15 % slower than the baseline = red

// CPU seconds of this process (self) or of its finished children; -1 = not measured here
double cpuSeconds(bool children)
{
#ifdef Q_OS_WIN
    Q_UNUSED(children);
    return -1; // wall clock only (GetProcessTimes of a finished QProcess child is not at hand)
#else
    rusage u{};
    getrusage(children ? RUSAGE_CHILDREN : RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
#endif
}

// Busy share of all processors right now (other programs: a game, a recording …), measured over 1 s; 0 = unknown.
// Not the load average: that still counts the tests that ran just before.
double busyShare()
{
    auto read = [](quint64* idle, quint64* total) {
        QFile f("/proc/stat"); // Linux only; elsewhere unknown
        if (!f.open(QIODevice::ReadOnly)) return false;
        const QList<QByteArray> v = f.readLine().simplified().split(' '); // cpu user nice system idle iowait …
        *idle = *total = 0;
        for (int i = 1; i < v.size(); ++i) {
            *total += v[i].toULongLong();
            if (i == 4 || i == 5) *idle += v[i].toULongLong();
        }
        return v.size() > 5;
    };
    quint64 i0, t0, i1, t1;
    if (!read(&i0, &t0)) return 0;
    QThread::msleep(1000);
    if (!read(&i1, &t1) || t1 <= t0) return 0;
    return 1.0 - double(i1 - i0) / double(t1 - t0);
}

struct Time {
    double wall = 0, cpu = -1; // s
};

Time bestOf(int runs, const std::function<Time()>& once)
{
    Time best{1e9, 1e9};
    for (int i = 0; i < runs; ++i) {
        const Time t = once();
        if (t.wall <= 0) return {};
        best.wall = std::min(best.wall, t.wall);
        best.cpu = t.cpu < 0 ? -1 : std::min(best.cpu, t.cpu);
    }
    return best;
}

ExportSettings settings(const QString& path)
{
    ExportSettings s;
    s.path = path;
    s.format = kFmt;
    s.videoCodec = "libx264";
    s.preset = "ultrafast"; // cheap encoder: the ratio shows what schneidi costs, not x264
    s.crf = 23;
    return s;
}

Time exportOnce(const Timeline& tl, ExportSettings s)
{
    Exporter ex;
    QEventLoop loop;
    bool ok = false;
    QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString&) {
        ok = success;
        loop.quit();
    });
    const double cpu0 = cpuSeconds(false);
    QElapsedTimer t;
    t.start();
    QString error;
    if (!CHECK(ex.start(tl, s, &error))) {
        std::printf("       %s\n", qPrintable(error));
        return {};
    }
    QTimer::singleShot(300000, &loop, &QEventLoop::quit);
    loop.exec();
    const double wall = t.elapsed() / 1000.0;
    if (!CHECK(ok)) return {};
    QFile::remove(s.path);
    return {wall, cpu0 < 0 ? -1 : cpuSeconds(false) - cpu0};
}

Time ffmpegOnce(const QString& media, const QString& out, const QStringList& extra = {})
{
    QProcess p;
    const double cpu0 = cpuSeconds(true);
    QElapsedTimer t;
    t.start();
    p.start("ffmpeg", QStringList{"-v", "error", "-nostdin", "-y", "-i", media, "-c:v", "libx264", "-preset", "ultrafast",
                                  "-crf", "23", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "192k"}
                          + extra + QStringList{out});
    if (!p.waitForFinished(300000) || p.exitCode() != 0) {
        std::printf("       ffmpeg: %s\n", p.readAllStandardError().constData());
        return {};
    }
    const double wall = t.elapsed() / 1000.0;
    QFile::remove(out);
    return {wall, cpu0 < 0 ? -1 : cpuSeconds(true) - cpu0};
}

Clip clip(int id, const QString& media, int start = 0, int link = 0)
{
    Clip c;
    c.id = id;
    c.mediaPath = media;
    c.in = 0;
    c.out = kFrames - 1;
    c.start = start;
    c.linkId = link;
    return c;
}

Timeline plain(const QString& media, int audioStream = 0)
{
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    tl.video[0].clips << clip(1, media, 0, 1);
    tl.audio[0].clips << clip(2, media, 0, 1);
    tl.audio[0].clips[0].audioStream = audioStream;
    return tl;
}

// Exporter::start() returns at once although the timeline has many files to open (built in a thread), and the
// export still comes out right. 2026-10-21: 6.2 s in start() for a 14-file project (opened one after another,
// BT.601 sources twice).
void testStartDoesNotBlock(const QStringList& files, const QString& dir)
{
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(2);
    int start = 0, id = 1;
    for (int i = 0; i < 24; ++i) {
        const QString& f = files[i % files.size()];
        Clip v = clip(id++, f, start, i + 1);
        v.in = (i * 7) % 200;
        v.out = v.in + 29;
        tl.video[0].clips << v;
        Clip a = v;
        a.id = id++;
        a.audioStream = f.endsWith(".mkv") ? 2 : 0;
        tl.audio[i % 2].clips << a;
        start += 30;
    }
    Exporter ex;
    QEventLoop loop;
    bool ok = false;
    QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString&) {
        ok = success;
        loop.quit();
    });
    ExportSettings s = settings(dir + "/start.mp4");
    QElapsedTimer t;
    t.start();
    QString error;
    CHECK(ex.start(tl, s, &error));
    const qint64 blocked = t.elapsed();
    if (!CHECK(blocked < 100)) std::printf("       start() blocked %lld ms\n", blocked);
    CHECK(ex.isRunning() && Exporter::anyRunning());
    QTimer::singleShot(300000, &loop, &QEventLoop::quit);
    loop.exec();
    CHECK(ok);
    CHECK(QFileInfo(s.path).size() > 0);
    std::printf("  start() %lld ms, export of %d clips from %lld files %.1f s\n", blocked, int(tl.video[0].clips.size()),
                qlonglong(files.size()), t.elapsed() / 1000.0);

    // Cancel while the graph is still being built: no file, nothing left running, no crash when the thread ends
    bool finished = false;
    Exporter ex2;
    QObject::connect(&ex2, &Exporter::finished, [&](bool success, const QString&) { finished = !success; });
    s.path = dir + "/cancel.mp4";
    CHECK(ex2.start(tl, s, &error));
    ex2.cancel();
    CHECK(finished);
    CHECK(!ex2.isRunning());
    CHECK(!Exporter::anyRunning());
    // Destroyed while building: waits for the thread
    {
        Exporter ex3;
        CHECK(ex3.start(tl, s, &error));
    }
    QTimer::singleShot(1500, &loop, &QEventLoop::quit); // the cancelled thread ends and goes away (deleteLater)
    loop.exec();
    CHECK(!QFileInfo::exists(s.path));
    CHECK(!Exporter::anyRunning());
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("speed");
    Check::initMlt();
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg fehlt");
    QTemporaryDir tmp;
    const QString dir = tmp.path();

    // 1080p60 like the user's recordings: BT.709 (NVIDIA) and BT.601 with several audio streams (the own clip program)
    const QStringList src{"-f", "lavfi", "-i", QString("testsrc2=size=1920x1080:rate=60:duration=%1").arg(kSeconds)};
    auto tone = [](int hz) {
        return QStringList{"-f", "lavfi", "-i", QString("sine=frequency=%1:sample_rate=48000:duration=%2").arg(hz).arg(kSeconds)};
    };
    const QStringList x264{"-c:v", "libx264", "-preset", "veryfast", "-g", "120", "-pix_fmt", "yuv420p", "-c:a", "aac"};
    const QString a709 = Check::makeMedia(dir + "/a709.mp4", src + tone(440) + x264
        + QStringList{"-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709"});
    const QString b601 = Check::makeMedia(dir + "/b601.mkv", src + tone(330) + tone(550) + tone(770)
        + QStringList{"-map", "0:v", "-map", "1:a", "-map", "2:a", "-map", "3:a"} + x264
        + QStringList{"-colorspace", "bt470bg"});
    if (a709.isEmpty() || b601.isEmpty()) return 1;
    QStringList files;
    for (int i = 0; i < 6; ++i) {
        const QString copy = QString("%1/file%2.%3").arg(dir).arg(i).arg(i % 2 ? "mkv" : "mp4");
        QFile::copy(i % 2 ? b601 : a709, copy);
        files << copy;
    }
    testStartDoesNotBlock(files, dir);

    const double busy = busyShare();
    if (busy > 0.2) {
        std::printf("Rechner ausgelastet (%.0f %% belegt): Tempo nicht gemessen\n", busy * 100);
        return Check::fails() ? Check::result() : Check::skip("Rechner ausgelastet");
    }

    // Scenarios: timeline, FFmpeg reference (source, extra arguments)
    struct Scenario {
        QString name;
        Timeline tl;
        FrameRate rate{0, 1};
        QString refMedia;
        QStringList refExtra;
    };
    QVector<Scenario> scenarios;
    scenarios.push_back({"709", plain(a709), {0, 1}, a709, {}});
    scenarios.push_back({"601", plain(b601), {0, 1}, b601, {}});
    {
        Timeline tl = plain(a709);
        Clip& v = tl.video[0].clips[0];
        EffectRegistry::add(v, "grade");
        Keys::setStaticValue(v, AnimParam::GradeSaturation, 60);
        EffectRegistry::add(v, "color");
        EffectRegistry::instance(v, "color")->params["saturation"] = 120.0;
        scenarios.push_back({"grade", tl, {0, 1}, a709, {}});
    }
    {
        Timeline tl = plain(a709);
        tl.video.resize(2);
        tl.audio.resize(2);
        Clip pip = clip(3, b601, 0, 2);
        pip.transform.zoomX = pip.transform.zoomY = 0.4;
        pip.transform.posX = 500;
        pip.transform.posY = 250;
        tl.video[1].clips << pip;
        Clip a = clip(4, b601, 0, 2);
        a.audioStream = 2;
        tl.audio[1].clips << a;
        scenarios.push_back({"layers", tl, {0, 1}, a709, {}});
    }
    scenarios.push_back({"rate30", plain(a709), {30, 1}, a709, {"-r", "30"}});

    const QString baselinePath = QStringLiteral(SCHNEIDI_TEST_DATA) + "/speed-baseline.json";
    QFile bf(baselinePath);
    QJsonObject all = bf.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(bf.readAll()).object() : QJsonObject{};
    bf.close();
    const QString machine = QString("%1/%2").arg(QSysInfo::machineHostName()).arg(QThread::idealThreadCount());
    const QJsonObject base = all.value(machine).toObject();
    const bool write = qEnvironmentVariable("SPEED_BASELINE") == "write";
    QJsonObject measured;

    std::printf("  %-8s %8s %8s %7s %7s   (ratio = schneidi / ffmpeg, baseline %s)\n", "", "schneidi", "ffmpeg", "wall",
                "cpu", base.isEmpty() ? "none" : qPrintable(machine));
    QHash<QString, Time> refs;
    for (const Scenario& sc : scenarios) {
        ExportSettings s = settings(dir + "/out.mp4");
        s.rate = sc.rate;
        const Time me = bestOf(2, [&] { return exportOnce(sc.tl, s); });
        const QString refKey = sc.refMedia + sc.refExtra.join(' ');
        if (!refs.contains(refKey)) refs[refKey] = bestOf(2, [&] { return ffmpegOnce(sc.refMedia, dir + "/ref.mp4", sc.refExtra); });
        const Time ref = refs[refKey];
        if (!CHECK(me.wall > 0 && ref.wall > 0)) continue;
        const double wall = me.wall / ref.wall;
        const double cpu = me.cpu > 0 && ref.cpu > 0 ? me.cpu / ref.cpu : -1;
        std::printf("  %-8s %7.2fs %7.2fs %7.2f %7.2f", qPrintable(sc.name), me.wall, ref.wall, wall, cpu);
        measured[sc.name] = QJsonObject{{"wall", std::round(wall * 100) / 100}, {"cpu", std::round(cpu * 100) / 100}};
        const QJsonObject b = base.value(sc.name).toObject();
        QStringList notes;
        auto brake = [&](const char* what, double now, double before) {
            if (before <= 0 || now <= 0) return;
            if (!CHECK(now <= before * kBrake)) notes << QString("%1 %2 > %3 +15 %").arg(what).arg(now, 0, 'f', 2).arg(before, 0, 'f', 2);
            else if (now < before / kBrake) notes << QString("%1 faster than baseline %2").arg(what).arg(before, 0, 'f', 2);
        };
        if (!write) {
            brake("wall", wall, b.value("wall").toDouble());
            brake("cpu", cpu, b.value("cpu").toDouble());
        }
        std::printf("%s%s\n", notes.isEmpty() ? "" : "   <- ", qPrintable(notes.join(", ")));
    }
    if (write && !Check::fails()) {
        all[machine] = measured;
        if (bf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            bf.write(QJsonDocument(all).toJson());
            std::printf("  baseline for %s written to %s\n", qPrintable(machine), qPrintable(baselinePath));
        }
    } else if (base.isEmpty()) {
        std::printf("  no baseline for %s: SPEED_BASELINE=write ctest -R test_speed (idle machine)\n", qPrintable(machine));
    }
    return Check::result();
}
