// Exportformate der Deliver-Seite: MP3, AIFF, Apple Lossless, Bittiefen (16/24/32 Float) und eine andere Bildrate
// als die Timeline (Bilder umgerechnet, Länge stimmt, Ton ohne Sprünge). Geprüft per ffprobe/ffmpeg.

#include "check.h"

#include "core/RenderJob.h"
#include "engine/Exporter.h"
#include "engine/RenderQueue.h"

#include <QApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>
#include <clocale>
#include <cmath>

namespace {

const ProjectFormat kFmt{640, 360, {25, 1}};
constexpr int kFrames = 100; // 4 s
QString g_media;

Timeline timeline()
{
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = g_media;
    c.out = kFrames - 1;
    c.linkId = 1;
    tl.video[0].clips << c;
    c.id = 2;
    tl.audio[0].clips << c;
    return tl;
}

bool render(const RenderJob& job)
{
    Exporter ex;
    QEventLoop loop;
    bool ok = false;
    QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString&) {
        ok = success;
        loop.quit();
    });
    QString error;
    if (!CHECK(ex.start(timeline(), RenderQueue::exportSettings(job, kFmt), &error))) {
        std::printf("       %s\n", qPrintable(error));
        return false;
    }
    QTimer::singleShot(120000, &loop, &QEventLoop::quit);
    loop.exec();
    return CHECK(ok);
}

// erster Strom der Art ("audio"/"video") laut ffprobe
QJsonObject probe(const QString& path, const char* type)
{
    QProcess p;
    p.start("ffprobe", {"-v", "error", "-show_streams", "-show_format", "-of", "json", path});
    p.waitForFinished(30000);
    const QJsonObject o = QJsonDocument::fromJson(p.readAllStandardOutput()).object();
    for (const QJsonValue& v : o.value("streams").toArray())
        if (v.toObject().value("codec_type").toString() == QLatin1String(type)) {
            QJsonObject s = v.toObject();
            s["format_duration"] = o.value("format").toObject().value("duration");
            return s;
        }
    return {};
}

RenderJob job(const QString& path, const QString& format, int bits = 24)
{
    RenderJob j;
    j.id = 1;
    j.path = path;
    j.settings.format = format;
    j.settings.audioBits = bits;
    return j;
}

void testAudioFormats(const QString& dir)
{
    struct Case { const char* format; int bits; const char* file; const char* codec; const char* sampleFmt; };
    const Case cases[] = {
        {"mp3", 24, "a.mp3", "mp3", nullptr},
        {"wav", 16, "a16.wav", "pcm_s16le", nullptr},
        {"wav", 24, "a24.wav", "pcm_s24le", nullptr},
        {"wav", 32, "a32.wav", "pcm_f32le", nullptr},
        {"aiff", 24, "a24.aiff", "pcm_s24be", nullptr},
        {"aiff", 32, "a32.aiff", "pcm_f32be", nullptr},
        {"alac", 16, "a16.m4a", "alac", "s16p"},
        {"alac", 24, "a24.m4a", "alac", "s32p"},
        {"prores", 32, "p32.mov", "pcm_f32le", nullptr},
    };
    for (const Case& c : cases) {
        const QString path = dir + "/" + c.file;
        if (!render(job(path, c.format, c.bits))) continue;
        const QJsonObject a = probe(path, "audio");
        if (!CHECK_EQ(a.value("codec_name").toString(), QString(c.codec))) std::printf("       %s\n", c.file);
        if (c.sampleFmt) CHECK_EQ(a.value("sample_fmt").toString(), QString(c.sampleFmt));
        CHECK(std::abs(a.value("format_duration").toString().toDouble() - 4.0) < 0.1);
    }
    // MP3: Bitrate wie gewählt
    RenderJob mp3 = job(dir + "/b.mp3", "mp3");
    mp3.settings.audioBitrateK = 192;
    if (render(mp3)) CHECK_EQ(probe(mp3.path, "audio").value("bit_rate").toString(), QString("192000"));
}

// größte Sprungstelle im Ton (zweite Ableitung, Mono 48 kHz) ab 0,05 s – Sinus 440 Hz bleibt weit darunter
int maxJump(const QString& path)
{
    QProcess p;
    p.start("ffmpeg", {"-v", "error", "-i", path, "-map", "0:a", "-f", "s16le", "-ac", "1", "-ar", "48000", "-"});
    p.waitForFinished(60000);
    const QByteArray d = p.readAllStandardOutput();
    const auto* s = reinterpret_cast<const qint16*>(d.constData());
    const int n = int(d.size() / 2);
    int jump = 0;
    for (int i = 2400; i < n - 2400; ++i) {
        const int j = std::abs(s[i] - 2 * s[i - 1] + s[i - 2]);
        if (j > 300 && qEnvironmentVariableIsSet("JUMPS")) std::printf("Sprung %d bei %.4f s\n", j, i / 48000.0);
        jump = std::max(jump, j);
    }
    return n > 4800 ? jump : 99999;
}

void testFrameRate(const QString& dir)
{
    RenderJob j = job(dir + "/r30.mov", "prores");
    j.settings.rateNum = 30;
    j.settings.rateDen = 1;
    if (render(j)) {
        const QJsonObject v = probe(j.path, "video");
        CHECK_EQ(v.value("r_frame_rate").toString(), QString("30/1"));
        CHECK_EQ(v.value("nb_frames").toString(), QString("120"));
        const double dur = probe(j.path, "audio").value("duration").toString().toDouble();
        if (!CHECK(std::abs(dur - 4.0) < 0.05)) std::printf("       Ton %.3f s\n", dur);
        const int jump = maxJump(j.path);
        if (!CHECK(jump < 300)) std::printf("       Sprung %d\n", jump);
    }
    // In/Out-Bereich (Frames 25–74 = 2 s) mit 50 fps -> 100 Bilder
    RenderJob r = job(dir + "/r50.mov", "prores");
    r.settings.rateNum = 50;
    r.inOut = true;
    r.from = 25;
    r.to = 74;
    if (render(r)) {
        CHECK_EQ(probe(r.path, "video").value("nb_frames").toString(), QString("100"));
        CHECK(maxJump(r.path) < 300);
    }
    // gleiche Bildrate wie die Timeline: normaler Weg
    RenderJob same = job(dir + "/r25.mov", "prores");
    same.settings.rateNum = 25;
    if (render(same)) CHECK_EQ(probe(same.path, "video").value("nb_frames").toString(), QString("100"));
}

void testSettingsJson()
{
    RenderSettings s;
    s.format = "aiff";
    s.audioBits = 32;
    s.rateNum = 30000;
    s.rateDen = 1001;
    const RenderSettings back = RenderSettings::fromJson(s.toJson());
    CHECK(back == s);
    CHECK_EQ(back.audioCodec(), QString("pcm_f32be"));
    RenderSettings alac;
    alac.format = "alac";
    alac.audioBits = 32; // gibt es bei Apple Lossless nicht -> 24 Bit
    CHECK_EQ(alac.audioSampleFormat(), QString("s32p"));
    RenderSettings audio;
    audio.format = "wav";
    audio.rateNum = 50;
    CHECK(audio.outputRate(kFmt.rate) == kFmt.rate); // nur Audio: Bildrate egal
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("export_formats");
    testSettingsJson();
    if (!Check::haveFfmpeg() || QStandardPaths::findExecutable("ffprobe").isEmpty())
        return Check::skip("ffmpeg/ffprobe nicht gefunden");
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    g_media = Check::makeMedia(tmp.filePath("src.mp4"),
                               {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=4", "-f", "lavfi", "-i",
                                "sine=frequency=440:duration=4", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac",
                                "-shortest"});
    if (!CHECK(!g_media.isEmpty())) return Check::result();
    testAudioFormats(tmp.path());
    testFrameRate(tmp.path());
    return Check::result();
}
