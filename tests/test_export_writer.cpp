// SPDX-License-Identifier: AGPL-3.0-only
// Export writer (engine/ExportWriter): cancelling and destroying while it runs leaves no file and no running threads.
// Until 0.15 the writer was also held against MLT's avformat consumer here (pictures, sound, color tags); the output
// itself is checked by test_export_timing, test_export_formats, test_export_audio and test_export_safety.
// SCHNEIDI_BENCH_WRITER=<media>: frames per second of the writer (libx264 ultrafast) instead of the tests.

#include "check.h"

#include "engine/Exporter.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <clocale>
#include <cmath>
#include <functional>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace {

const ProjectFormat kFmt{640, 360, {25, 1}};
constexpr int kFrames = 75; // 3 s
QString g_709, g_601;

Clip clipOf(int id, const QString& media, int start, int length)
{
    Clip c;
    c.id = id;
    c.mediaPath = media;
    c.start = start;
    c.out = length - 1;
    return c;
}

Timeline timeline(const QString& media = g_709)
{
    Timeline tl;
    tl.video.resize(2);
    tl.audio.resize(1);
    Clip v = clipOf(1, media, 0, kFrames);
    v.linkId = 1;
    tl.video[0].clips << v;
    Clip a = v;
    a.id = 2;
    tl.audio[0].clips << a;
    return tl;
}

ExportSettings settings(const QString& path)
{
    ExportSettings s;
    s.path = path;
    s.format = kFmt;
    s.videoCodec = "ffv1";
    s.pixFmt = "yuv420p";
    s.crf = 0;
    s.audioCodec = "pcm_s16le";
    // The slim FFmpeg of the packages has no ffv1 encoder: ProRes there (only cancelling is checked here)
    if (!avcodec_find_encoder_by_name("ffv1")) {
        s.videoCodec = "prores_ks";
        s.pixFmt = "yuv422p10le";
    }
    return s;
}

bool render(const Timeline& tl, const ExportSettings& s, QString* used = nullptr)
{
    Exporter ex;
    QEventLoop loop;
    bool ok = false;
    QString message;
    QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString& msg) {
        ok = success;
        message = msg;
        loop.quit();
    });
    QString error;
    if (!ex.start(tl, s, &error)) {
        std::printf("       %s\n", qPrintable(error));
        return false;
    }
    QTimer::singleShot(120000, &loop, &QEventLoop::quit);
    loop.exec();
    if (!ok) std::printf("       %s: %s\n", qPrintable(s.path), qPrintable(message.isEmpty() ? "timeout" : message));
    if (used) *used = ex.usedCodec();
    return ok;
}

void testCancel(const QString& dir)
{
    Timeline tl = timeline();
    for (int i = 1; i < 40; ++i) { // 2 minutes
        Clip v = tl.video[0].clips.first();
        v.id = 100 + i;
        v.start = i * kFrames;
        v.linkId = 0;
        tl.video[0].clips << v;
    }
    ExportSettings s = settings(dir + "/cancel.mkv");
    {
        Exporter ex;
        QEventLoop loop;
        bool finished = false, ok = true;
        QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString&) {
            finished = true;
            ok = success;
            loop.quit();
        });
        QObject::connect(&ex, &Exporter::progress, &ex, [&](int percent) {
            if (percent >= 1 && !finished) ex.cancel();
        });
        QString error;
        if (!CHECK(ex.start(tl, s, &error))) return (void)std::printf("       %s\n", qPrintable(error));
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);
        loop.exec();
        CHECK(finished && !ok);
        CHECK(!QFileInfo::exists(s.path));
    }
    {
        auto ex = std::make_unique<Exporter>();
        QString error;
        if (!CHECK(ex->start(tl, s, &error))) return (void)std::printf("       %s\n", qPrintable(error));
        QEventLoop loop;
        QObject::connect(ex.get(), &Exporter::progress, &loop, [&](int percent) {
            if (percent >= 1) loop.quit();
        });
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);
        loop.exec();
        CHECK(ex->isRunning());
        ex.reset(); // must stop the threads and release all frames before the graph goes
        CHECK(true);
    }
}

int bench(const QString& media)
{
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    QProcess p;
    p.start("ffprobe", {"-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height,r_frame_rate,nb_frames",
                        "-of", "json", media});
    p.waitForFinished(30000);
    const QJsonObject st = QJsonDocument::fromJson(p.readAllStandardOutput()).object().value("streams").toArray().first().toObject();
    const QStringList r = st.value("r_frame_rate").toString().split('/');
    ProjectFormat f{st.value("width").toInt(), st.value("height").toInt(), {r.value(0).toInt(), std::max(1, r.value(1).toInt())}};
    const int frames = std::min(600, st.value("nb_frames").toString().toInt() > 0 ? st.value("nb_frames").toString().toInt() : 600);
    Clip c = clipOf(1, media, 0, frames);
    c.linkId = 1;
    tl.video[0].clips << c;
    c.id = 2;
    tl.audio[0].clips << c;
    QTemporaryDir tmp;
    {
        ExportSettings s;
        const QString keep = qEnvironmentVariable("SCHNEIDI_BENCH_KEEP"); // folder: keep the file
        s.path = keep.isEmpty() ? tmp.filePath("bench.mp4") : keep + "/bench.mp4";
        s.format = f;
        s.preset = qEnvironmentVariable("SCHNEIDI_BENCH_PRESET", "ultrafast");
        if (qEnvironmentVariableIsSet("SCHNEIDI_BENCH_CODEC")) s.videoCodec = qEnvironmentVariable("SCHNEIDI_BENCH_CODEC");
        if (qEnvironmentVariableIsSet("SCHNEIDI_BENCH_NOAUDIO")) s.audioCodec.clear();
        if (qEnvironmentVariableIsSet("SCHNEIDI_BENCH_THREADS")) s.threads = qEnvironmentVariableIntValue("SCHNEIDI_BENCH_THREADS");
        QElapsedTimer t;
        t.start();
        const bool ok = render(tl, s);
        std::printf("writer: %s, %d frames %.0f ms = %.1f fps\n", ok ? "ok" : "FAILED", frames, double(t.elapsed()), frames * 1000.0 / std::max<qint64>(1, t.elapsed()));
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("export_writer");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    if (qEnvironmentVariableIsSet("SCHNEIDI_BENCH_WRITER")) {
        Check::initEngine();
        std::setlocale(LC_NUMERIC, "C");
        return bench(qEnvironmentVariable("SCHNEIDI_BENCH_WRITER"));
    }
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    if (qEnvironmentVariableIsSet("SCHNEIDI_TEST_KEEP")) { // look at the files afterwards
        tmp.setAutoRemove(false);
        std::printf("files in %s\n", qPrintable(tmp.path()));
    }
    const QStringList src{"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=3", "-f", "lavfi", "-i",
                          "sine=frequency=440:duration=3:sample_rate=48000", "-c:v", "libx264", "-preset", "ultrafast",
                          "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"};
    g_709 = Check::makeMedia(tmp.filePath("709.mp4"),
                             QStringList(src) << "-colorspace" << "bt709" << "-color_primaries" << "bt709" << "-color_trc"
                                              << "bt709");
    g_601 = Check::makeMedia(tmp.filePath("601.mp4"), src); // unmarked small picture: taken for BT.601
    if (!CHECK(!g_709.isEmpty() && !g_601.isEmpty())) return Check::result();
    Check::initEngine();
    std::setlocale(LC_NUMERIC, "C");
    static bool waitedTooLong = false;
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext&, const QString& msg) {
        if (msg.contains("Decode order")) waitedTooLong = true;
        if (type != QtInfoMsg && type != QtDebugMsg) std::fprintf(stderr, "%s\n", qPrintable(msg));
    });
    testCancel(tmp.path());
    if (!CHECK(!waitedTooLong)) std::printf("       a frame waited for the decode order\n");
    return Check::result();
}
