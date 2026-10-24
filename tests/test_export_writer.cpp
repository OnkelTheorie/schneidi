// Own export path (engine/ExportWriter) against MLT's avformat consumer: the same timeline written both ways with a
// lossless codec (FFV1 in MKV, PCM sound) must give the same decoded pictures and sound, frame count and color tags.
// Cases: BT.709 and BT.601 sources, title over video (composited in RGBA), In/Out, another frame rate (frames in order),
// packed 4:2:2 from MLT (8 and 10 bit), an output width that is no multiple of 16 (MLT buffer handed over as it is),
// sound from MLT's graph instead of the own audio engine. Also: cancelling and destroying while it runs.
// SCHNEIDI_BENCH_WRITER=<media>: frames per second of both paths (libx264 ultrafast) instead of the tests.

#include "check.h"

#include "engine/Exporter.h"

#include <framework/mlt_log.h>

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

// Decoded stream as raw data: md5 and size (frames/samples follow from the size)
struct Decoded {
    QByteArray md5;
    qint64 bytes = 0;
    QByteArray data; // sound only (small)
};

Decoded decode(const QString& path, bool video, const QString& pixFmt)
{
    QProcess p;
    QStringList args{"-v", "error", "-i", path, "-map", video ? "0:v:0" : "0:a:0"};
    if (video) args << "-f" << "rawvideo" << "-pix_fmt" << pixFmt;
    else args << "-f" << "s16le" << "-ac" << "2";
    args << "-";
    p.start("ffmpeg", args);
    QCryptographicHash hash(QCryptographicHash::Md5);
    Decoded d;
    while (p.waitForReadyRead(30000) || p.bytesAvailable()) {
        const QByteArray chunk = p.readAllStandardOutput();
        if (chunk.isEmpty() && p.state() == QProcess::NotRunning) break;
        hash.addData(chunk);
        d.bytes += chunk.size();
        if (!video) d.data += chunk;
    }
    p.waitForFinished(30000);
    d.md5 = hash.result().toHex();
    return d;
}

QJsonObject videoStream(const QString& path)
{
    QProcess p;
    p.start("ffprobe", {"-v", "error", "-select_streams", "v:0", "-show_streams", "-of", "json", path});
    p.waitForFinished(30000);
    return QJsonDocument::fromJson(p.readAllStandardOutput()).object().value("streams").toArray().first().toObject();
}

// Renders the case with both writers and compares
void compare(const char* name, const QString& dir, const Timeline& tl, ExportSettings s, int frames)
{
    const QString mlt = dir + "/" + name + "-mlt.mkv", own = dir + "/" + name + "-own.mkv";
    s.path = mlt;
    s.writer = ExportSettings::Writer::Mlt;
    if (!CHECK(render(tl, s))) return;
    s.path = own;
    s.writer = ExportSettings::Writer::Own;
    if (!CHECK(render(tl, s))) return;
    const Decoded vm = decode(mlt, true, s.pixFmt), vo = decode(own, true, s.pixFmt);
    if (!CHECK_EQ(vo.bytes, vm.bytes) || !CHECK_EQ(vo.md5, vm.md5))
        std::printf("       %s: picture differs (own %lld bytes, MLT %lld)\n", name, vo.bytes, vm.bytes);
    // frame count from the stream (both identical when the md5 is)
    QProcess count;
    count.start("ffprobe", {"-v", "error", "-count_frames", "-select_streams", "v:0", "-show_entries",
                            "stream=nb_read_frames", "-of", "csv=p=0", own});
    count.waitForFinished(30000);
    if (!CHECK_EQ(count.readAllStandardOutput().trimmed().toInt(), frames)) std::printf("       %s: frame count\n", name);
    if (!s.audioCodec.isEmpty()) {
        // Exactly the length of the range (s16 stereo); MLT's consumer pads FLAC's last frame with silence (+0.07 s)
        const Decoded am = decode(mlt, false, {}), ao = decode(own, false, {});
        const double fps = s.rate.num > 0 ? s.rate.fps() : kFmt.rate.fps();
        CHECK_EQ(ao.bytes, qint64(std::llround(frames / fps * 48000)) * 4);
        if (!CHECK(am.bytes >= ao.bytes) || !CHECK(am.data.left(ao.data.size()) == ao.data))
            std::printf("       %s: sound differs (own %lld bytes, MLT %lld)\n", name, ao.bytes, am.bytes);
    }
    const QJsonObject tm = videoStream(mlt), to = videoStream(own);
    for (const char* key : {"color_space", "color_primaries", "color_transfer", "pix_fmt", "width", "height", "r_frame_rate"})
        if (!CHECK_EQ(to.value(key).toVariant().toString(), tm.value(key).toVariant().toString()))
            std::printf("       %s: %s\n", name, key);
    CHECK_EQ(to.value("color_range").toString(), QString("tv")); // MLT left it unspecified
}

void testCompare(const QString& dir)
{
    compare("709", dir, timeline(), settings({}), kFrames);
    compare("601", dir, timeline(g_601), settings({}), kFrames);
    {
        Timeline tl = timeline();
        Clip t;
        t.id = 10;
        t.kind = ClipKind::Title;
        t.start = 10;
        t.out = 39;
        t.title.text = "Titel";
        t.title.size = 80;
        t.title.outlineOn = true;
        tl.video[1].clips << t;
        Clip pip = clipOf(11, g_601, 30, 30);
        pip.transform.zoomX = pip.transform.zoomY = 0.5;
        pip.transform.posX = 100;
        pip.transform.opacity = 70;
        tl.video[1].clips << pip;
        compare("composite", dir, tl, settings({}), kFrames);
    }
    {
        ExportSettings s = settings({});
        s.from = 20;
        s.to = 49;
        compare("inout", dir, timeline(), s, 30);
    }
    {
        ExportSettings s = settings({});
        s.rate = FrameRate{30, 1};
        compare("rate30", dir, timeline(), s, 90);
    }
    for (const char* pixFmt : {"yuv422p", "yuv422p10le", "yuv444p10le"}) {
        ExportSettings s = settings({});
        s.pixFmt = pixFmt;
        compare(pixFmt, dir, timeline(g_601), s, kFrames);
    }
    {
        ExportSettings s = settings({});
        s.size = QSize(650, 366); // rows of 650 bytes: the MLT buffer goes to the encoder with its own strides
        compare("oddwidth", dir, timeline(), s, kFrames);
    }
    {
        // Lower track covered for 40 frames, then visible (engine/DecodeOrder: released frames must not hold it up)
        Timeline tl = timeline(g_601);
        Clip top = clipOf(20, g_709, 0, 40);
        top.transform.zoomX = top.transform.zoomY = 1.2;
        tl.video[1].clips << top;
        QElapsedTimer t;
        t.start();
        compare("covered", dir, tl, settings({}), kFrames);
        CHECK(t.elapsed() < 20000);
    }
    {
        ExportSettings s = settings({});
        s.audioPath = ExportSettings::AudioPath::Mlt;
        s.audioCodec = "flac";
        compare("mltsound", dir, timeline(), s, kFrames);
        s.rate = FrameRate{30, 1}; // repeated frames deliver no sound (RateConverter)
        compare("mltsound30", dir, timeline(), s, 90);
    }
}

// Cancel while running, and destroy a running export: nothing left behind, no crash
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
    s.writer = ExportSettings::Writer::Own;
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
        if (!CHECK(ex.start(tl, s, &error))) return;
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);
        loop.exec();
        CHECK(finished && !ok);
        CHECK(!QFileInfo::exists(s.path));
    }
    {
        auto ex = std::make_unique<Exporter>();
        QString error;
        if (!CHECK(ex->start(tl, s, &error))) return;
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
    const QString only = qEnvironmentVariable("SCHNEIDI_BENCH_ONLY"); // "own" or "mlt"
    for (const auto writer : {ExportSettings::Writer::Mlt, ExportSettings::Writer::Own}) {
        if (!only.isEmpty() && (only == "own") != (writer == ExportSettings::Writer::Own)) continue;
        ExportSettings s;
        const QString keep = qEnvironmentVariable("SCHNEIDI_BENCH_KEEP"); // folder: keep the files
        s.path = keep.isEmpty() ? tmp.filePath("bench.mp4")
                                : keep + (writer == ExportSettings::Writer::Own ? "/own.mp4" : "/mlt.mp4");
        s.format = f;
        s.preset = qEnvironmentVariable("SCHNEIDI_BENCH_PRESET", "ultrafast");
        s.writer = writer;
        if (qEnvironmentVariableIsSet("SCHNEIDI_BENCH_CODEC")) s.videoCodec = qEnvironmentVariable("SCHNEIDI_BENCH_CODEC");
        if (qEnvironmentVariableIsSet("SCHNEIDI_BENCH_NOAUDIO")) s.audioCodec.clear();
        if (qEnvironmentVariableIsSet("SCHNEIDI_BENCH_THREADS")) s.threads = qEnvironmentVariableIntValue("SCHNEIDI_BENCH_THREADS");
        QElapsedTimer t;
        t.start();
        const bool ok = render(tl, s);
        std::printf("%s: %s, %d frames %.0f ms = %.1f fps\n", writer == ExportSettings::Writer::Own ? "own" : "mlt",
                    ok ? "ok" : "FAILED", frames, double(t.elapsed()), frames * 1000.0 / std::max<qint64>(1, t.elapsed()));
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
        Check::initMlt();
        std::setlocale(LC_NUMERIC, "C");
        if (qEnvironmentVariableIsSet("SCHNEIDI_BENCH_MLTDEBUG")) mlt_log_set_level(MLT_LOG_DEBUG);
        return bench(qEnvironmentVariable("SCHNEIDI_BENCH_WRITER"));
    }
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    const QStringList src{"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=3", "-f", "lavfi", "-i",
                          "sine=frequency=440:duration=3:sample_rate=48000", "-c:v", "libx264", "-preset", "ultrafast",
                          "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"};
    g_709 = Check::makeMedia(tmp.filePath("709.mp4"),
                             QStringList(src) << "-colorspace" << "bt709" << "-color_primaries" << "bt709" << "-color_trc"
                                              << "bt709");
    g_601 = Check::makeMedia(tmp.filePath("601.mp4"), src); // unmarked small picture: MLT takes it for BT.601
    if (!CHECK(!g_709.isEmpty() && !g_601.isEmpty())) return Check::result();
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    static bool waitedTooLong = false;
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext&, const QString& msg) {
        if (msg.contains("Decode order")) waitedTooLong = true;
        if (type != QtInfoMsg && type != QtDebugMsg) std::fprintf(stderr, "%s\n", qPrintable(msg));
    });
    testCompare(tmp.path());
    if (!CHECK(!waitedTooLong)) std::printf("       a frame waited for the decode order\n");
    testCancel(tmp.path());
    return Check::result();
}
