// Export aus Opus-Quellen (MKV/WebM-artig und MP4) nach H.264/AAC (MP4): Ton bleibt synchron zum Bild, Ton- und
// Bildlänge passen. FFmpeg meldet dabei „[opus] Could not update timestamps for skipped samples“ und „[mp4] Timestamps
// are unset in a packet for stream 1 … making some up“ – beides harmlos (siehe Bundle::configureMltLogging); dieser
// Test belegt das. Außerdem: Meldestufen und nicht geladenes jackrack-Modul (LADSPA-Meldung).

#include "check.h"

#include "engine/Exporter.h"
#include "engine/Profiles.h"

#include <Mlt.h>
#include <QApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>
#include <clocale>
#include <cmath>
#include <cstring>

namespace {

const ProjectFormat kFmt{320, 180, {25, 1}};
constexpr int kFrames = 150;    // 6 s
constexpr int kFlashFrame = 50; // weißes Bild bei 2,0 s, gleichzeitig ein 1-kHz-Klick im Ton

Timeline timeline(const QString& media)
{
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = media;
    c.out = kFrames - 1;
    c.linkId = 1;
    tl.video[0].clips << c;
    c.id = 2;
    tl.audio[0].clips << c;
    return tl;
}

bool render(const Timeline& tl, const QString& path)
{
    Exporter ex;
    ExportSettings s;
    s.path = path;
    s.format = kFmt;
    s.preset = "veryfast";
    QEventLoop loop;
    bool ok = false;
    QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString&) {
        ok = success;
        loop.quit();
    });
    QString error;
    if (!CHECK(ex.start(tl, s, &error))) {
        std::printf("       %s\n", qPrintable(error));
        return false;
    }
    QTimer::singleShot(120000, &loop, &QEventLoop::quit);
    loop.exec();
    return CHECK(ok);
}

QByteArray ffmpegOut(const QStringList& args)
{
    QProcess p;
    p.start("ffmpeg", QStringList{"-v", "error"} + args);
    p.waitForFinished(60000);
    return p.readAllStandardOutput();
}

// erster Zeitpunkt (s), an dem der Ton laut wird; -1 = nie
double audioOnset(const QString& path)
{
    const QByteArray d = ffmpegOut({"-i", path, "-map", "0:a", "-ac", "1", "-ar", "48000", "-f", "f32le", "-"});
    const int n = int(d.size() / sizeof(float));
    for (int i = 0; i < n; ++i) {
        float v;
        std::memcpy(&v, d.constData() + i * sizeof(float), sizeof v);
        if (std::abs(v) > 0.2f) return i / 48000.0;
    }
    return -1;
}

// erstes helles Bild (Index); -1 = keins
int flashFrame(const QString& path)
{
    const QByteArray d =
        ffmpegOut({"-i", path, "-map", "0:v", "-vf", "scale=1:1", "-pix_fmt", "gray", "-f", "rawvideo", "-"});
    for (int i = 0; i < d.size(); ++i)
        if (quint8(d[i]) > 128) return i;
    return -1;
}

QJsonObject probe(const QString& path)
{
    QProcess p;
    p.start("ffprobe", {"-v", "error", "-show_streams", "-of", "json", path});
    p.waitForFinished(30000);
    QJsonObject r;
    for (const QJsonValue& v : QJsonDocument::fromJson(p.readAllStandardOutput()).object().value("streams").toArray())
        r[v.toObject().value("codec_type").toString()] = v.toObject();
    return r;
}

void testLogging()
{
    CHECK(qgetenv("MLT_REPOSITORY_DENY").split(':').contains("libmltjackrack"));
    CHECK_EQ(mlt_log_get_level(), MLT_LOG_WARNING);
    // jackrack nicht geladen: kein LADSPA-Filter, keine Suche nach Plugins
    auto prof = makeProfile(kFmt);
    mlt_filter f = mlt_factory_filter(prof->get_profile(), "ladspa", nullptr);
    CHECK(f == nullptr);
    if (f) mlt_filter_close(f);
}

void testSync(const QString& dir, const QString& source)
{
    std::printf("Quelle %s\n", qPrintable(QFileInfo(source).fileName()));
    const QString out = dir + "/out_" + QFileInfo(source).completeBaseName() + ".mp4";
    if (!render(timeline(source), out)) return;
    const QJsonObject s = probe(out);
    CHECK_EQ(s.value("audio").toObject().value("codec_name").toString(), QString("aac"));
    const double aStart = s.value("audio").toObject().value("start_time").toString().toDouble();
    const double vStart = s.value("video").toObject().value("start_time").toString().toDouble();
    const double aDur = s.value("audio").toObject().value("duration").toString().toDouble();
    const double vDur = s.value("video").toObject().value("duration").toString().toDouble();
    std::printf("       Start Ton %.4f / Bild %.4f, Länge Ton %.4f / Bild %.4f\n", aStart, vStart, aDur, vDur);
    CHECK(std::abs(aStart - vStart) < 0.001);
    CHECK(std::abs(vDur - kFrames / 25.0) < 0.001);
    CHECK(std::abs(aDur - vDur) < 0.05); // AAC: höchstens das letzte Paket (1024 Samples) zu lang
    const double onset = audioOnset(out);
    const int flash = flashFrame(out);
    std::printf("       Klick bei %.4f s, helles Bild %d\n", onset, flash);
    CHECK_EQ(flash, kFlashFrame);
    CHECK(std::abs(onset - kFlashFrame / 25.0) < 0.002);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    qunsetenv("SCHNEIDI_VERBOSE");
    QApplication app(argc, argv);
    Check::initApp("export_opus");
    if (!Check::haveFfmpeg() || QStandardPaths::findExecutable("ffprobe").isEmpty())
        return Check::skip("ffmpeg/ffprobe nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    const QStringList input{"-f", "lavfi", "-i",
                            "color=black:size=320x180:rate=25:duration=6,drawbox=enable='between(n,50,51)':color=white:t=fill",
                            "-f", "lavfi", "-i",
                            "aevalsrc='if(between(t,2,2.04),0.8*sin(2*PI*1000*t),0)':s=48000:d=6", "-c:v", "libx264",
                            "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "libopus", "-b:a", "128k"};
    const QString mkv = Check::makeMedia(tmp.filePath("opus.mkv"), input);
    const QString mp4 = Check::makeMedia(tmp.filePath("opus.mp4"), input);
    if (mkv.isEmpty() || mp4.isEmpty()) return Check::skip("ffmpeg ohne libx264/libopus");

    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    testLogging();
    testSync(tmp.path(), mkv);
    testSync(tmp.path(), mp4);
    return Check::result();
}
