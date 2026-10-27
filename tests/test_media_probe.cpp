// Engine::probe reads media files with its own header reader (engine/MediaProbe) instead of an MLT producer. Per
// generated file (containers, codecs, several titled sound streams, rotated phone video, variable frame rate, sound
// only, odd size, interlaced, cover pictures, timecode track, broken files) and project frame rate: the result
// equals the MLT producer's (Engine::probeMlt) in every field, and the own reader decides it itself where it should
// (still images and broken files stay with MLT). Times of both are printed (real material: bench_probe).

#include "check.h"

#include "engine/Engine.h"
#include "engine/MediaProbe.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <vector>

namespace {

struct Case {
    const char* file;
    QStringList args;   // ffmpeg arguments without output; empty = written by the test itself
    bool own = true;    // decided by the own reader (else MLT)
};

QString describe(const MediaInfo& m)
{
    QString s = QString("len=%1 video=%2 audio=%3 image=%4 streams=[").arg(m.length).arg(m.hasVideo).arg(m.hasAudio)
                    .arg(m.isImage);
    for (const AudioStreamInfo& a : m.audioStreams) s += QString("%1ch '%2' ").arg(a.channels).arg(a.title);
    return s + "]";
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("media_probe");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    Check::initMlt();

    QTemporaryDir dir;
    const auto in = [&](const char* name) { return dir.filePath(name); };
    const QStringList v{"-f", "lavfi", "-i", "testsrc2=size=320x240:rate=30:duration=3.2"};
    const QStringList a{"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=3.2"};
    const QStringList x264{"-c:v", "libx264", "-preset", "ultrafast"};
    const QStringList pal{"-f", "lavfi", "-i", "testsrc2=size=720x576:rate=25:duration=2"};
    const QStringList vfr{"-f", "lavfi", "-i", "testsrc2=size=320x240:rate=60:duration=3", "-vf",
                          "select='not(between(n,20,40))'", "-fps_mode", "vfr"};
    const std::vector<Case> cases{
        {"h264_aac.mp4", v + a + x264 + QStringList{"-c:a", "aac"}},
        {"hevc.mp4", v + a + QStringList{"-c:v", "libx265", "-preset", "ultrafast", "-tag:v", "hvc1", "-c:a", "aac"}},
        {"vp9_opus.webm", v + a + QStringList{"-c:v", "libvpx-vp9", "-deadline", "realtime", "-cpu-used", "8", "-c:a", "libopus"}},
        {"prores.mov", v + a + QStringList{"-c:v", "prores_ks", "-c:a", "pcm_s16le"}},
        // A game recording: five sound streams, titled (one with spaces, one "und"), one of them mono
        {"obs5.mkv", v + a + a + a + a + a
                         + QStringList{"-map", "0", "-map", "1", "-map", "2", "-map", "3", "-map", "4", "-map", "5"} + x264
                         + QStringList{"-c:a", "aac", "-ac", "2", "-ac:a:1", "1", "-metadata:s:a:0", "title=Desktop", "-metadata:s:a:1",
                                       "title= Mic ", "-metadata:s:a:2", "title=und", "-metadata:s:a:4", "title=Discord"}},
        {"obs5.mp4", v + a + a + a + QStringList{"-map", "0", "-map", "1", "-map", "2", "-map", "3"} + x264
                         + QStringList{"-c:a", "aac", "-metadata:s:a:0", "title=Desktop", "-metadata:s:a:2", "title=Mic"}},
        {"rotated.mp4", QStringList{"-display_rotation:v:0", "90", "-i", in("h264_aac.mp4"), "-c", "copy"}},
        {"vfr.mp4", vfr + x264},
        {"vfr.mkv", vfr + x264},
        {"ntsc.mov", QStringList{"-f", "lavfi", "-i", "testsrc2=size=320x240:rate=30000/1001:duration=3"} + x264},
        {"timecode.mov", QStringList{"-f", "lavfi", "-i", "testsrc2=size=320x240:rate=24000/1001:duration=3"} + a + x264
                             + QStringList{"-c:a", "aac", "-shortest", "-timecode", "01:00:00:00"}},
        {"odd.mp4", QStringList{"-f", "lavfi", "-i", "testsrc2=size=321x241:rate=25:duration=2"} + x264
                        + QStringList{"-pix_fmt", "yuv444p"}},
        {"interlaced.ts", pal + a + QStringList{"-flags", "+ilme+ildct", "-c:v", "mpeg2video", "-c:a", "mp2", "-shortest"}},
        {"h264.mts", v + a + x264 + QStringList{"-c:a", "aac", "-f", "mpegts"}},
        {"ps.mpg", pal + a + QStringList{"-c:v", "mpeg2video", "-c:a", "mp2", "-shortest"}},
        {"mjpeg.avi", pal + a + QStringList{"-c:v", "mjpeg", "-c:a", "pcm_s16le", "-shortest"}},
        {"h264.flv", v + a + x264 + QStringList{"-c:a", "aac", "-shortest"}},
        {"ac3.mkv", v + a + x264 + QStringList{"-c:a", "ac3"}},
        {"late_audio.mkv", v + QStringList{"-itsoffset", "0.5"} + a + QStringList{"-map", "0", "-map", "1"} + x264
                               + QStringList{"-c:a", "aac"}},
        {"frag.mp4", v + a + x264 + QStringList{"-c:a", "aac", "-movflags", "+frag_keyframe+empty_moov"}},
        {"sound.wav", a + QStringList{"-c:a", "pcm_s16le"}},
        {"f64.wav", a + QStringList{"-c:a", "pcm_f64le"}, false}, // MLT plays no 64-bit samples: not opened
        // 80.5 frames at 25 fps: how a half frame rounds
        {"half.wav", QStringList{"-f", "lavfi", "-i", "sine=sample_rate=48000:duration=3.22", "-c:a", "pcm_s16le"}},
        {"surround.wav", a + QStringList{"-af", "pan=5.1|c0=c0|c1=c0|c2=c0|c3=c0|c4=c0|c5=c0", "-c:a", "pcm_s16le"}},
        {"sound.flac", a + QStringList{"-c:a", "flac"}},
        {"sound.mp3", a + QStringList{"-c:a", "libmp3lame"}},
        {"sound.m4a", a + QStringList{"-c:a", "aac"}},
        {"sound.opus", a + QStringList{"-c:a", "libopus"}},
        {"sound.ogg", a + QStringList{"-c:a", "libvorbis"}},
        {"cover.png", QStringList{"-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25:duration=1", "-frames:v", "1"}, false},
        {"cover.mp3", QStringList{"-i", in("sound.mp3"), "-i", in("cover.png"), "-map", "0", "-map", "1", "-c:a", "copy",
                                  "-c:v", "png", "-disposition:v", "attached_pic"}},
        {"cover_sound.mp4", QStringList{"-i", in("sound.m4a"), "-i", in("cover.png"), "-map", "0", "-map", "1", "-c:a",
                                        "copy", "-c:v", "png", "-disposition:v", "attached_pic"}},
        {"text.mp4", {}, false},
        {"empty.mp4", {}, false},
        {"truncated.mp4", {}, false},
    };
    for (const Case& c : cases) {
        if (!c.args.isEmpty()) {
            if (Check::makeMedia(in(c.file), c.args).isEmpty()) std::printf("%s: not written, skipped\n", c.file);
            continue;
        }
        QFile f(in(c.file));
        CHECK(f.open(QIODevice::WriteOnly));
        if (qstrcmp(c.file, "text.mp4") == 0) f.write("not a video\n");
        if (qstrcmp(c.file, "truncated.mp4") == 0) {
            QFile whole(in("h264_aac.mp4"));
            CHECK(whole.open(QIODevice::ReadOnly));
            f.write(whole.read(30000));
        }
    }

    const FrameRate rates[] = {{25, 1}, {30000, 1001}, {60, 1}, {24000, 1001}};
    double ownTotal = 0, mltTotal = 0;
    for (const Case& c : cases) {
        const QString path = in(c.file);
        if (!QFileInfo::exists(path)) continue;
        for (const FrameRate& rate : rates) {
            ProjectFormat format;
            format.rate = rate;
            QElapsedTimer t;
            t.start();
            const std::optional<MediaInfo> own = MediaProbe::read(path, rate);
            const double ownMs = t.nsecsElapsed() / 1e6;
            t.restart();
            const MediaInfo mlt = Engine::probeMlt(format, path);
            const double mltMs = t.nsecsElapsed() / 1e6;
            const MediaInfo probed = Engine::probe(format, path);
            ownTotal += ownMs;
            mltTotal += mltMs;
            if (!Check::report(own.has_value() == c.own, "own reader decides", __FILE__, __LINE__,
                               QString("%1 @ %2: own=%3").arg(c.file, rate.label()).arg(own.has_value())))
                continue;
            const QString want = describe(mlt), got = describe(probed);
            Check::report(got == want && probed.path == mlt.path && probed.name == mlt.name, "probe == MLT", __FILE__,
                          __LINE__, QString("%1 @ %2\n       own: %3\n       MLT: %4").arg(c.file, rate.label(), got, want));
            if (rate == rates[0])
                std::printf("%-16s own %7.2f ms  MLT %7.2f ms  %s%s\n", c.file, ownMs, mltMs, qPrintable(want),
                            own ? "" : "  (MLT)");
        }
    }
    std::printf("total: own %.1f ms, MLT %.1f ms\n", ownTotal, mltTotal);

    // What the files really hold (guards against both readers being wrong the same way)
    ProjectFormat format;
    const MediaInfo obs = Engine::probe(format, in("obs5.mkv"));
    CHECK(obs.length == 80 || obs.length == 81); // 3.2 s at 25 fps (Matroska: the sound's priming after it)
    CHECK(obs.hasVideo && obs.hasAudio && !obs.isImage);
    CHECK_EQ(obs.audioStreams.size(), 5);
    if (obs.audioStreams.size() == 5) {
        CHECK_EQ(obs.audioStreams[0].title, QString("Desktop"));
        CHECK_EQ(obs.audioStreams[1].title, QString("Mic"));
        CHECK_EQ(obs.audioStreams[0].channels, 2);
        CHECK_EQ(obs.audioStreams[1].channels, 1);
        CHECK_EQ(obs.audioStreams[2].title, QString()); // "und" = no title
        CHECK_EQ(obs.audioStreams[4].title, QString("Discord"));
    }
    const MediaInfo cover = Engine::probe(format, in("cover.mp3"));
    CHECK(!cover.hasVideo && cover.hasAudio);
    const MediaInfo still = Engine::probe(format, in("cover.png"));
    CHECK(still.isImage && still.hasVideo && still.length == 125);
    const MediaInfo surround = Engine::probe(format, in("surround.wav"));
    CHECK(surround.audioStreams.size() == 1 && surround.audioStreams[0].channels == 6);
    return Check::result();
}
