// SPDX-License-Identifier: AGPL-3.0-only
// Engine::probe reads media files with its own header reader (engine/MediaProbe). Per generated file (containers,
// codecs, several titled sound streams, rotated phone video, variable frame rate, sound only, odd size, interlaced,
// cover pictures, timecode track, broken files) and project frame rate: the reader decides the files it should, and
// broken or unplayable ones give an empty result (no picture, no sound). The values were held against MLT's producer
// until 0.15; what the files really hold is checked below.

#include "check.h"

#include "engine/Engine.h"
#include "engine/FfmpegLog.h"
#include "engine/MediaProbe.h"
#include "engine/OpenMedia.h"
#include "engine/video/Reader.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <climits>
#include <cstring>
#include <vector>

namespace {

struct Case {
    const char* file;
    QStringList args;   // ffmpeg arguments without output; empty = written by the test itself
    bool own = true;    // decided by the reader (else unreadable: empty result)
};

QString describe(const MediaInfo& m)
{
    QString s = QString("len=%1 video=%2 audio=%3 image=%4 streams=[").arg(m.length).arg(m.hasVideo).arg(m.hasAudio)
                    .arg(m.isImage);
    for (const AudioStreamInfo& a : m.audioStreams) s += QString("%1ch '%2' ").arg(a.channels).arg(a.title);
    return s + "]";
}

// h264_aac.mp4 with all header lengths 0 (version 0 boxes: mvhd/mdhd = version+flags, created, modified, time scale,
// duration; tkhd = version+flags, created, modified, track id, reserved, duration)
QByteArray zeroLengths(const QString& source)
{
    QFile whole(source);
    if (!whole.open(QIODevice::ReadOnly)) return {};
    QByteArray data = whole.readAll();
    for (const char* box : {"mvhd", "mdhd", "tkhd"}) {
        for (qsizetype at = data.indexOf(box); at >= 0; at = data.indexOf(box, at + 4)) {
            if (data[at + 4] != 0) continue; // version 1 (64-bit times): ffmpeg writes version 0 here
            const qsizetype duration = at + (qstrcmp(box, "tkhd") == 0 ? 24 : 20);
            for (int i = 0; i < 4; ++i) data[duration + i] = 0;
        }
    }
    return data;
}

QStringList g_avLog; // FFmpeg's messages (FfmpegLog)

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("media_probe");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    Check::initEngine();

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
        {"f64.wav", a + QStringList{"-c:a", "pcm_f64le"}, false}, // 64-bit samples are not played: not opened
        // 80.5 frames at 25 fps: how a half frame rounds
        {"half.wav", QStringList{"-f", "lavfi", "-i", "sine=sample_rate=48000:duration=3.22", "-c:a", "pcm_s16le"}},
        {"surround.wav", a + QStringList{"-af", "pan=5.1|c0=c0|c1=c0|c2=c0|c3=c0|c4=c0|c5=c0", "-c:a", "pcm_s16le"}},
        {"sound.flac", a + QStringList{"-c:a", "flac"}},
        {"sound.mp3", a + QStringList{"-c:a", "libmp3lame"}},
        {"sound.m4a", a + QStringList{"-c:a", "aac"}},
        {"alac.m4a", a + QStringList{"-c:a", "alac"}},
        {"opus.mkv", v + a + x264 + QStringList{"-c:a", "libopus"}},
        {"sound.opus", a + QStringList{"-c:a", "libopus"}},
        {"sound.ogg", a + QStringList{"-c:a", "libvorbis"}},
        {"cover.png", QStringList{"-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25:duration=1", "-frames:v", "1"}},
        {"photo.jpg", QStringList{"-f", "lavfi", "-i", "testsrc2=size=640x480:rate=25:duration=1", "-frames:v", "1"}},
        {"cover.mp3", QStringList{"-i", in("sound.mp3"), "-i", in("cover.png"), "-map", "0", "-map", "1", "-c:a", "copy",
                                  "-c:v", "png", "-disposition:v", "attached_pic"}},
        {"cover_sound.mp4", QStringList{"-i", in("sound.m4a"), "-i", in("cover.png"), "-map", "0", "-map", "1", "-c:a",
                                        "copy", "-c:v", "png", "-disposition:v", "attached_pic"}},
        {"text.mp4", {}, false},
        {"empty.mp4", {}, false},
        {"broken.png", {}, false}, // not a picture: Qt cannot read it
        {"truncated.mp4", {}, false},
        // Header lengths of 0 (mvhd/tkhd/mdhd, as an aborted recording can leave them): no length: unreadable
        {"zero_length.mp4", {}, false},
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
        if (qstrcmp(c.file, "zero_length.mp4") == 0) f.write(zeroLengths(in("h264_aac.mp4")));
    }

    const FrameRate rates[] = {{25, 1}, {30000, 1001}, {60, 1}, {24000, 1001}};
    double total = 0;
    for (const Case& c : cases) {
        const QString path = in(c.file);
        if (!QFileInfo::exists(path)) continue;
        for (const FrameRate& rate : rates) {
            ProjectFormat format;
            format.rate = rate;
            QElapsedTimer t;
            t.start();
            const std::optional<MediaInfo> own = MediaProbe::read(path, rate);
            total += t.nsecsElapsed() / 1e6;
            const MediaInfo probed = Engine::probe(format, path);
            if (!Check::report(own.has_value() == c.own, "reader decides", __FILE__, __LINE__,
                               QString("%1 @ %2: own=%3").arg(c.file, rate.label()).arg(own.has_value())))
                continue;
            const QString want = own ? describe(*own) : describe(MediaInfo{}), got = describe(probed);
            Check::report(got == want && probed.path == path && probed.name == QFileInfo(path).fileName(),
                          "probe == reader", __FILE__, __LINE__,
                          QString("%1 @ %2\n       probe: %3\n       want: %4").arg(c.file, rate.label(), got, want));
            if (rate == rates[0]) std::printf("%-16s %s%s\n", c.file, qPrintable(got), own ? "" : "  (unreadable)");
        }
    }
    std::printf("total: %.1f ms\n", total);

    // What the files really hold
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
    // Absurd header length (Matroska Duration 10^10 s, a broken header): capped, never a negative length
    {
        QFile mkv(in("ac3.mkv"));
        QFile f(in("absurd_length.mkv"));
        if (CHECK(mkv.open(QIODevice::ReadOnly) && f.open(QIODevice::WriteOnly))) {
            QByteArray data = mkv.readAll();
            const qsizetype at = data.indexOf(QByteArray("\x44\x89\x88", 3)); // Duration, 8-byte float (ms)
            if (CHECK(at >= 0)) {
                const double ms = 1e13;
                quint64 bits;
                std::memcpy(&bits, &ms, 8);
                for (int i = 0; i < 8; ++i) data[at + 3 + i] = char(bits >> (56 - 8 * i));
            }
            f.write(data);
            f.close();
            const std::optional<MediaInfo> absurd = MediaProbe::read(f.fileName(), {25, 1});
            CHECK(absurd && absurd->length == INT_MAX && absurd->hasVideo);
        }
    }
    const MediaInfo surround = Engine::probe(format, in("surround.wav"));
    CHECK(surround.audioStreams.size() == 1 && surround.audioStreams[0].channels == 6);

    // Files that name other files (FFmpeg picks the demuxer by content, whatever the name): never opened, so a server's
    // work folder cannot read a file outside through one. An IMF composition (needs a full FFmpeg build) whose asset
    // map points at an MXF elsewhere, a concat list next to a clip
    {
        QDir(dir.path()).mkpath("elsewhere");
        QDir(dir.path()).mkpath("work");
        const QString mxf = Check::makeMedia(in("elsewhere/secret.mxf"), {"-f", "lavfi", "-i",
                                                                          "testsrc2=size=64x48:rate=24:duration=1",
                                                                          "-c:v", "mpeg2video"});
        const auto write = [&](const char* name, const QByteArray& data) {
            QFile f(in(name));
            CHECK(f.open(QIODevice::WriteOnly));
            f.write(data);
        };
        const QByteArray uuid = "urn:uuid:8713c020-2489-45f5-a9f7-87be539e20";
        write("work/composition.mp4",
              "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<CompositionPlaylist xmlns=\"http://www.smpte-ra.org/schemas/"
              "2067-3/2016\" xmlns:cc=\"http://www.smpte-ra.org/schemas/2067-2/2016\" xmlns:xsi=\"http://www.w3.org/2001/"
              "XMLSchema-instance\"><Id>" + uuid + "b5</Id><ContentTitle>x</ContentTitle><EditRate>24 1</EditRate>"
              "<SegmentList><Segment><Id>" + uuid + "b6</Id><SequenceList><cc:MainImageSequence><Id>" + uuid + "b7</Id>"
              "<TrackId>" + uuid + "b8</TrackId><ResourceList><Resource xsi:type=\"TrackFileResourceType\"><Id>" + uuid
              + "b9</Id><EditRate>24 1</EditRate><IntrinsicDuration>24</IntrinsicDuration><EntryPoint>0</EntryPoint>"
              "<SourceDuration>24</SourceDuration><TrackFileId>" + uuid + "ba</TrackFileId></Resource></ResourceList>"
              "</cc:MainImageSequence></SequenceList></Segment></SegmentList></CompositionPlaylist>\n");
        write("work/ASSETMAP.xml", "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<AssetMap xmlns=\"http://www.smpte-ra.org/"
                                   "schemas/429-9/2007/AM\"><Id>" + uuid + "bb</Id><AssetList><Asset><Id>" + uuid
                                   + "ba</Id><ChunkList><Chunk><Path>" + mxf.toUtf8()
                                   + "</Path></Chunk></ChunkList></Asset></AssetList></AssetMap>\n");
        QFile::copy(in("h264_aac.mp4"), in("work/clip.mp4"));
        write("work/list.mp4", "ffconcat version 1.0\nfile clip.mp4\n");
        FfmpegLog::install([](bool, const QString& text) { g_avLog << text; });
        for (const char* name : {"work/composition.mp4", "work/list.mp4"}) {
            const MediaInfo m = Engine::probe(format, in(name));
            CHECK(!m.hasVideo && !m.hasAudio);
            QString error;
            CHECK(!Video::Reader::open(in(name), {}, &error));
        }
        // The refusal says so in one short line, not with FFmpeg's whole list of allowed demuxers (~3 KB)
        CHECK(!g_avLog.isEmpty());
        for (const QString& line : std::as_const(g_avLog))
            if (!CHECK(line.size() < 300)) std::printf("       long FFmpeg line: %s...\n", qPrintable(line.left(120)));
        CHECK(OpenMedia::refused("imf") && OpenMedia::refused("hls") && OpenMedia::refused("concat"));
        CHECK(!OpenMedia::refused("mov,mp4,m4a,3gp,3g2,mj2") && !OpenMedia::refused("image2"));
        // Plain files still open
        CHECK(Engine::probe(format, in("work/clip.mp4")).hasVideo);
        CHECK(Video::Reader::open(in("work/clip.mp4"), {}));
    }
    return Check::result();
}
