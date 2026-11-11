// SPDX-License-Identifier: AGPL-3.0-only
// Sound streams schneidi cannot play keep their place in a file's stream list (the sound reader counts streams by
// their place) but are never used (review of probe-vs-thumbnail, 2026-11-11):
// - a stream without a decoder here or without any packet (an empty MPEG-TS sound PID) was placed by the stream
//   presets, picked for a transcript, accepted by `audio_stream`; the sound reader read such a stream to the file's end
//   looking for its first sound, on every open (playback, waveforms, export)
// - the long stream analysis (60 s / 120 MB) is remembered per file: one that helped opens with it at once next time,
//   one in vain is not repeated; a quick look (the timeline's drag-enter, on the UI thread) never runs it
// - a cover picture beside sound that cannot be played is no picture to import, also when the header decides
// - the project file keeps whether a stream can be played (older files: every stream can)

#include "check.h"

#include "core/ProjectFile.h"
#include "core/StreamPresets.h"
#include "core/Transcript.h"
#include "engine/Engine.h"
#include "engine/MediaProbe.h"
#include "engine/audio/Reader.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>

namespace {

// The MPEG-TS with every packet of `pid` turned into a null packet: the stream is declared, but holds nothing
bool emptied(const QString& source, const QString& target, int pid)
{
    QFile in(source), out(target);
    if (!in.open(QIODevice::ReadOnly) || !out.open(QIODevice::WriteOnly)) return false;
    QByteArray data = in.readAll();
    int hits = 0;
    for (qsizetype at = 0; at + 188 <= data.size(); at += 188) {
        if (uchar(data[at]) != 0x47) return false;
        const int p = ((uchar(data[at + 1]) & 0x1F) << 8) | uchar(data[at + 2]);
        if (p != pid) continue;
        data[at + 1] = char((uchar(data[at + 1]) & 0xE0) | 0x1F);
        data[at + 2] = char(0xFF);
        ++hits;
    }
    return hits > 0 && out.write(data) == data.size();
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("probe_dead_streams");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    Check::initEngine();

    QTemporaryDir dir;
    const auto in = [&](const char* name) { return dir.filePath(name); };
    const QStringList x264{"-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"};
    const QStringList v{"-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25:duration=12"};
    const QStringList a{"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=12"};
    const FrameRate rate{25, 1};

    // 1. MPEG-TS: video, a sound stream, a second one whose PID holds nothing (0x100 video, 0x101/0x102 sound)
    if (CHECK(!Check::makeMedia(in("two.ts"), v + a + a + x264
                                                  + QStringList{"-map", "0", "-map", "1", "-map", "2", "-c:a", "aac",
                                                                "-metadata:s:a:0", "title=Game", "-metadata:s:a:1",
                                                                "title=Mic", "-shortest"})
                   .isEmpty())
        && CHECK(emptied(in("two.ts"), in("dead_mic.ts"), 0x102))) {
        MediaProbe::forgetForTest();
        const int before = MediaProbe::longAnalysesForTest();
        const std::optional<MediaInfo> m = MediaProbe::read(in("dead_mic.ts"), rate);
        // (FFmpeg's MPEG-TS demuxer may list a further stream after the empty one)
        if (CHECK(m.has_value()) && CHECK(m->audioStreams.size() >= 2)) {
            for (const AudioStreamInfo& s : m->audioStreams)
                std::printf("stream '%s' %d ch playable %d\n", qPrintable(s.title), s.channels, int(s.playable));
            CHECK(m->hasVideo && m->hasAudio);
            CHECK(m->audioStreamPlayable(0));
            CHECK(!m->audioStreamPlayable(1));
            // Never placed, whatever the rules say
            StreamPresets presets;
            const QVector<StreamPlacement> placed = presets.placements(*m);
            CHECK(!placed.isEmpty() && placed[0].stream == 0);
            CHECK(std::none_of(placed.begin(), placed.end(), [](const StreamPlacement& p) { return p.stream == 1; }));
            StreamPresets placeMic;
            placeMic.rules << StreamRule{"Mic", StreamRule::Action::Place} << StreamRule{"Game", StreamRule::Action::Skip};
            const QVector<StreamPlacement> ruled = placeMic.placements(*m);
            CHECK(std::none_of(ruled.begin(), ruled.end(), [](const StreamPlacement& p) { return p.stream == 1; }));
            // The transcript takes the first stream that can be played, not a dead "Mic"
            CHECK(Transcript::defaultStreams(*m) == QVector<int>{0});
        }
        // The long analysis ran once (in vain: the PID stays empty), not again for the same file
        CHECK_EQ(MediaProbe::longAnalysesForTest(), before + 1);
        CHECK(MediaProbe::read(in("dead_mic.ts"), rate).has_value());
        CHECK_EQ(MediaProbe::longAnalysesForTest(), before + 1);

        // The sound reader: the good stream plays, the dead one fails without reading the file to its end, and at
        // once the next time
        CHECK(Audio::Reader::open(in("dead_mic.ts"), 0, Audio::Reader::Options{48000, 2}) != nullptr);
        QElapsedTimer t;
        t.start();
        CHECK(Audio::Reader::open(in("dead_mic.ts"), 1, Audio::Reader::Options{48000, 2}) == nullptr);
        const qint64 first = t.restart();
        CHECK(Audio::Reader::open(in("dead_mic.ts"), 1, Audio::Reader::Options{48000, 2}) == nullptr);
        const qint64 again = t.elapsed();
        std::printf("dead stream: %lld ms, again %lld ms\n", first, again);
        CHECK(again < 20);
    }

    // 2. Sound that starts 12 s in: the long analysis found it (remembered: opened with it at once next time); a quick
    // look never runs it and says its result is partial; the sound reader plays it
    if (CHECK(!Check::makeMedia(in("late.ts"), QStringList{"-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25:duration=20",
                                                           "-itsoffset", "12", "-f", "lavfi", "-i", "sine=duration=5", "-map",
                                                           "0", "-map", "1"}
                                                   + x264 + QStringList{"-c:a", "aac"})
                   .isEmpty())) {
        MediaProbe::forgetForTest();
        const int before = MediaProbe::longAnalysesForTest();
        bool partial = false;
        MediaProbe::ReadOptions quick;
        quick.longAnalysis = false;
        quick.partial = &partial;
        const std::optional<MediaInfo> q = MediaProbe::read(in("late.ts"), rate, quick);
        CHECK(partial);
        CHECK(q && q->hasVideo);
        CHECK_EQ(MediaProbe::longAnalysesForTest(), before); // the quick look did not
        const std::optional<MediaInfo> m = MediaProbe::read(in("late.ts"), rate);
        CHECK(m && m->hasVideo && m->hasAudio && m->audioStreamPlayable(0));
        CHECK_EQ(MediaProbe::longAnalysesForTest(), before + 1);
        const std::optional<MediaInfo> again = MediaProbe::read(in("late.ts"), rate);
        CHECK(again && again->hasAudio);
        CHECK_EQ(MediaProbe::longAnalysesForTest(), before + 1); // opened with the long analysis at once
        partial = true;
        CHECK(MediaProbe::read(in("late.ts"), rate, quick).has_value());
        CHECK(!partial || q.has_value()); // (a quick look may stay partial; it never reads long)
        CHECK_EQ(MediaProbe::longAnalysesForTest(), before + 1);
        const std::unique_ptr<Audio::Reader> r = Audio::Reader::open(in("late.ts"), 0, Audio::Reader::Options{48000, 2});
        if (CHECK(r != nullptr)) CHECK(r->info().start > 48000 * 10); // the sound from 12 s on
    }

    // 3. A cover picture beside sound in a sample format the sound path does not play (64-bit float), decided from the
    // header alone: nothing to import
    if (CHECK(!Check::makeMedia(in("cover.png"), QStringList{"-f", "lavfi", "-i", "testsrc2=size=64x64", "-frames:v", "1"})
                   .isEmpty())
        && CHECK(!Check::makeMedia(in("f64_cover.mkv"), a + QStringList{"-c:a", "pcm_f64le", "-attach", in("cover.png"),
                                                                        "-metadata:s:t", "mimetype=image/png"})
                      .isEmpty())) {
        CHECK(!MediaProbe::read(in("f64_cover.mkv"), rate).has_value());
    }

    // 4. The project file keeps "cannot be played"; older files: every stream can
    {
        MediaInfo m{"/x/a.ts", "a.ts", 100, true, true, false};
        m.audioStreams = {AudioStreamInfo{2, "Game", true}, AudioStreamInfo{1, "Mic", false}};
        ProjectData data;
        data.media << m;
        const QString project = "/x/p.schneidi";
        const QByteArray json = ProjectFile::toJson(data, project);
        ProjectData back;
        QString error;
        if (CHECK(ProjectFile::fromJson(json, project, &back, &error)) && CHECK(back.media.size() == 1)) {
            CHECK(back.media[0].audioStreamPlayable(0));
            CHECK(!back.media[0].audioStreamPlayable(1));
        }
        QByteArray old = json;
        old.replace("\"playable\": false", "\"x\": 0").replace("\"playable\":false", "\"x\":0");
        CHECK(old != json);
        ProjectData older;
        if (CHECK(ProjectFile::fromJson(old, project, &older, &error)) && CHECK(older.media.size() == 1))
            CHECK(older.media[0].audioStreamPlayable(1));
    }

    return Check::result();
}
