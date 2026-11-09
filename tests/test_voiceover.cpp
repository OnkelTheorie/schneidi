// SPDX-License-Identifier: AGPL-3.0-only
// Voice-over recording (0.16, 0.20 straight onto a track): the sound input into a WAV file (a stand-in input paced by
// a timer, never the microphone), a level meter without a file, lining the file up with the timeline, the track a
// take goes to (armed, selected, none or sound in the way -> a new one), placing it (one undo step), the growing take
// (live waveform, exact length), file names, settings
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/Editor.h"
#include "core/ProjectFile.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "core/Voiceover.h"
#include "engine/Engine.h"
#include "engine/audio/Capture.h"

#include "check.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>

#include <cmath>
#include <numbers>

namespace {

QJsonObject cli(const QString& name, const QJsonObject& raw0, QString* error = nullptr)
{
    QJsonObject raw = raw0;
    if (name == "edit") raw["full"] = true;
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        if (error) *error = e.code;
        return {};
    }
}

struct Wav {
    int rate = 0, channels = 0, bits = 0;
    QVector<float> samples; // interleaved
};

Wav readWav(const QString& path)
{
    Wav w;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return w;
    const QByteArray b = f.readAll();
    if (b.size() < 44 || !b.startsWith("RIFF") || b.mid(8, 4) != "WAVE") return w;
    w.channels = qFromLittleEndian<quint16>(b.constData() + 22);
    w.rate = int(qFromLittleEndian<quint32>(b.constData() + 24));
    w.bits = qFromLittleEndian<quint16>(b.constData() + 34);
    const quint32 bytes = qFromLittleEndian<quint32>(b.constData() + 40);
    // Sizes in the header must match the file (an odd data chunk has a pad byte the RIFF size counts)
    const qint64 padded = qint64(bytes) + (bytes & 1);
    if (w.bits != 24 || 44 + padded != b.size() || qFromLittleEndian<quint32>(b.constData() + 4) != 36 + padded)
        return w;
    for (quint32 i = 0; i + 2 < bytes; i += 3) {
        const auto* p = reinterpret_cast<const unsigned char*>(b.constData() + 44 + i);
        qint32 v = p[0] | (p[1] << 8) | (p[2] << 16);
        if (v & 0x800000) v -= 0x1000000;
        w.samples << float(v / 8388607.0);
    }
    return w;
}

QString text(const std::optional<Voiceover::Placement>& p)
{
    return p ? QString("%1+%2 in %3").arg(p->start).arg(p->subframe).arg(p->in) : QString("nichts");
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    qputenv("SCHNEIDI_AUDIO_INPUT", "sine:1000");
    QCoreApplication app(argc, argv);
    Check::initApp("voiceover");
    QTemporaryDir tmp;

    // ---- Capture: a 1 kHz tone for about one second into a WAV file
    {
        const QString path = tmp.filePath("take.wav");
        QString error;
        Audio::Capture::Config config;
        config.rate = 48000;
        config.channels = 1;
        auto capture = Audio::Capture::start(config, path, &error);
        CHECK(capture != nullptr);
        if (capture) {
            QElapsedTimer t;
            t.start();
            float peak = 0.f;
            while (t.elapsed() < 1000) {
                QThread::msleep(50);
                peak = std::max(peak, capture->takePeak());
            }
            CHECK(capture->stop(&error));
            CHECK(capture->stop()); // a second stop does nothing
            CHECK_EQ(capture->dropped(), 0LL);
            CHECK(std::abs(peak - 0.25f) < 0.01f); // the meter saw the tone
            const long long frames = capture->frames();
            // Paced in real time: ~1 s (one period of 512 either way, a busy machine a bit more)
            CHECK(frames >= 48000 - 1024 && frames <= 48000 + 6000);
            const Wav w = readWav(path);
            CHECK_EQ(w.rate, 48000);
            CHECK_EQ(w.channels, 1);
            CHECK_EQ(qint64(w.samples.size()), qint64(frames)); // every frame delivered is in the file
            // No frame lost or doubled: the tone continues sample by sample over the block edges
            double worst = 0;
            for (int i = 0; i < w.samples.size(); ++i)
                worst = std::max(worst, std::abs(w.samples[i] - 0.25 * std::sin(2 * std::numbers::pi * 1000.0 * i / 48000)));
            CHECK(worst < 1e-5);
            // The file reads as audio in the project's frame rate
            const MediaInfo m = Engine::probe(ProjectFormat{}, path);
            CHECK(m.hasAudio && !m.hasVideo);
            CHECK(std::abs(m.length - int(std::lround(double(frames) / 48000 * ProjectFormat{}.rate.fps()))) <= 1);
        }
    }
    // ---- Review 2026-11-09: the header's sizes follow while recording (a crash mid-take leaves a readable file)
    {
        const QString path = tmp.filePath("growing.wav");
        Audio::Capture::Config config;
        config.rate = 48000;
        config.channels = 1;
        auto capture = Audio::Capture::start(config, path);
        if (CHECK(capture != nullptr)) {
            QThread::msleep(2600);
            QFile f(path);
            quint32 data = 0, riff = 0;
            if (CHECK(f.open(QIODevice::ReadOnly))) {
                const QByteArray head = f.read(44);
                if (CHECK(head.size() == 44)) {
                    riff = qFromLittleEndian<quint32>(head.constData() + 4);
                    data = qFromLittleEndian<quint32>(head.constData() + 40);
                }
            }
            std::printf("header while recording: data %u bytes (file %lld)\n", data, f.size());
            CHECK(data >= 48000 * 3 && data % 3 == 0); // at least a second, whole frames
            CHECK(data <= f.size() - 44);
            CHECK_EQ(riff, 36 + data + (data & 1));
            capture->stop();
        }
    }
    // ---- A stalled writer (a disk that hangs): blocks lost beyond the ring buffer become silence of the same length,
    // the file keeps frames() and everything after a gap lies where it was heard. Two gaps in one take (mono), then a
    // take that stops while the writer still stalls (stereo: the lost end is silence too)
    for (const int channels : {1, 2}) {
        const QString path = tmp.filePath(QString("stall%1.wav").arg(channels));
        Audio::Capture::Config config;
        config.rate = 48000;
        config.channels = channels;
        config.ringSeconds = 0.1;
        auto capture = Audio::Capture::start(config, path);
        if (!CHECK(capture != nullptr)) break;
        QThread::msleep(150);
        capture->holdWriter(true);
        QThread::msleep(400);
        capture->holdWriter(false);
        QThread::msleep(150);
        capture->holdWriter(true);
        QThread::msleep(400);
        if (channels == 1) {
            capture->holdWriter(false);
            QThread::msleep(150);
        }
        CHECK(capture->stop());
        const long long frames = capture->frames(), dropped = capture->dropped();
        CHECK(dropped >= 2 * 48000 / 10); // each stall lost more than the 0.3 s past the ring
        const Wav w = readWav(path);
        CHECK_EQ(qint64(w.samples.size()), qint64(frames * channels)); // the file is as long as the take
        // Every sample is the tone at its own index or a lost one (silence); exactly the lost frames are silent
        long long wrong = 0, silent = 0;
        for (int i = 0; i < w.samples.size(); ++i) {
            const double want = 0.25 * std::sin(2 * std::numbers::pi * 1000.0 * (i / channels) / 48000);
            if (std::abs(w.samples[i] - want) < 1e-5) continue;
            if (w.samples[i] == 0.f) ++silent;
            else ++wrong;
        }
        CHECK_EQ(wrong, 0LL);
        // A lost sample where the tone crosses zero matches it anyway: allow those few
        CHECK(silent <= dropped * channels && silent >= dropped * channels * 9 / 10);
        if (channels == 1) { // the tone after the last gap: in time to the end
            double worst = 0;
            for (int i = int(w.samples.size()) - 4800; i < w.samples.size(); ++i)
                worst = std::max(worst, std::abs(w.samples[i] - 0.25 * std::sin(2 * std::numbers::pi * 1000.0 * i / 48000)));
            CHECK(worst < 1e-5);
        }
    }

    // Silence through the backend "null" (no tone), stereo
    {
        qputenv("SCHNEIDI_AUDIO_INPUT", "");
        Audio::Capture::Config config;
        config.backend = "null";
        config.channels = 2;
        auto capture = Audio::Capture::start(config, tmp.filePath("silence.wav"));
        CHECK(capture != nullptr);
        if (capture) {
            QThread::msleep(200);
            CHECK(capture->stop());
            const Wav w = readWav(tmp.filePath("silence.wav"));
            CHECK_EQ(w.channels, 2);
            CHECK_EQ(qint64(w.samples.size()), qint64(capture->frames() * 2));
            CHECK(capture->takePeak() == 0.f);
        }
        qputenv("SCHNEIDI_AUDIO_INPUT", "null");
        // A file that cannot be written: no recording, a reason
        QString error;
        CHECK(Audio::Capture::start(config, tmp.filePath("missing/dir/x.wav"), &error) == nullptr);
        CHECK(!error.isEmpty());
    }

    // A level meter only (an armed track before recording): no file, the level and the frame count still come
    {
        qputenv("SCHNEIDI_AUDIO_INPUT", "sine:440");
        Audio::Capture::Config config;
        auto meter = Audio::Capture::start(config, QString());
        if (CHECK(meter != nullptr)) {
            QThread::msleep(150);
            CHECK(meter->frames() > 0);
            CHECK(meter->takePeak() > 0.2f);
            CHECK(meter->stop());
        }
        qputenv("SCHNEIDI_AUDIO_INPUT", "null");
    }

    // An odd number of mono samples: odd data chunk, pad byte after it (RIFF chunks are word aligned). Blocks of 65
    // samples: try until a take ends with an odd count
    {
        bool odd = false;
        for (int i = 0; i < 20 && !odd; ++i) {
            Audio::Capture::Config config;
            config.buffer = 130;
            const QString path = tmp.filePath(QString("odd%1.wav").arg(i));
            auto capture = Audio::Capture::start(config, path);
            if (!CHECK(capture != nullptr)) break;
            QThread::msleep(15 + i);
            CHECK(capture->stop());
            odd = capture->frames() % 2;
            CHECK_EQ(QFileInfo(path).size(), 44 + capture->frames() * 3 + (odd ? 1 : 0));
            CHECK_EQ(qint64(readWav(path).samples.size()), qint64(capture->frames()));
        }
        CHECK(odd);
    }

    // ---- Placement: where the file goes in the timeline
    {
        using Voiceover::place;
        CHECK_EQ(text(place(100.0, 100, 50)), QString("100+0 in 0"));
        // Pre-roll of 2 s at 25 fps: file starts 50 frames before the playhead, that part is cut off
        CHECK_EQ(text(place(50.0, 100, 200)), QString("100+0 in 50"));
        // Latency: the file came 0.3 frames late -> starts 0.3 frames before the playhead: one frame cut, 0.7 after
        CHECK_EQ(text(place(99.7, 100, 50)), QString("100+70 in 1"));
        CHECK_EQ(text(place(100.25, 100, 50)), QString("100+25 in 0"));
        CHECK_EQ(text(place(99.999, 100, 50)), QString("100+0 in 0")); // a hair early counts as on the frame
        CHECK_EQ(text(place(99.996, 100, 50)), QString("100+0 in 0"));
        CHECK_EQ(text(place(99.994, 100, 50)), QString("100+99 in 1"));
        CHECK_EQ(text(place(50.0, 100, 50)), QString("nichts")); // everything before the playhead
    }
    // ---- Alignment: the smallest difference while the clock runs, minus latencies
    {
        Voiceover::Alignment a;
        CHECK(!a.fileStartSeconds(0, 48000, 0));
        a.add(0, 4.0, 48000);         // first pair: nothing to compare with
        a.add(4800, 4.0, 48000);      // clock standing (waits for the picture): ignored
        a.add(9600, 4.1, 48000);      // runs: 4.1 - 0.2 = 3.9
        a.add(9600, 4.15, 48000);     // input block not in yet: 3.95, worse
        a.add(14400, 4.2, 48000);     // 3.9 again
        a.add(14400 + 480, 4.205, 48000); // 3.895: best
        CHECK(std::abs(*a.fileStartSeconds(0, 48000, 0) - 3.895) < 1e-9);
        // Input buffer of 960 samples (20 ms) and a user offset of 30 ms
        CHECK(std::abs(*a.fileStartSeconds(960, 48000, 30) - 3.845) < 1e-9);
        // The playhead jumps back while recording (a click on the ruler, J): the pairs after it do not count
        a.add(48000, 2.0, 48000);
        a.add(48000 + 4800, 2.1, 48000); // would be 1.1
        CHECK(std::abs(*a.fileStartSeconds(0, 48000, 0) - 3.895) < 1e-9);
        // Before the first counted pair a jump back is the start (seek to the pre-roll): pairing goes on
        Voiceover::Alignment b;
        b.add(0, 4.0, 48000);
        b.add(0, 2.0, 48000);
        b.add(4800, 2.1, 48000);
        CHECK(std::abs(*b.fileStartSeconds(0, 48000, 0) - 2.0) < 1e-9);
    }

    // ---- Which track a take goes to (user report: 24 takes made 10 new tracks next to five empty ones)
    {
        Timeline tl;
        tl.audio.resize(4);
        Clip a;
        a.id = 1;
        a.mediaPath = "/x/a.wav";
        a.start = 50;
        a.out = 49; // [50, 100) on A1
        tl.audio[0].clips << a;
        Clip b = a;
        b.id = 2;
        b.start = 0; // [0, 50) on A3
        tl.audio[2].clips << b;
        CHECK_EQ(Voiceover::selectedTrack(tl, {1}), 0);
        CHECK_EQ(Voiceover::selectedTrack(tl, {}), -1);
        CHECK_EQ(Voiceover::selectedTrack(tl, {1, 2}), -1); // clips on two tracks: none of them
        // Armed: there, also over sound (punch-in); locked armed: refused
        CHECK_EQ(Voiceover::recordTrack(tl, 0, -1, 60), 0);
        // Selected and free from the start on: there; not free: the first free unnamed track
        CHECK_EQ(Voiceover::recordTrack(tl, -1, 2, 60), 2);
        CHECK_EQ(Voiceover::recordTrack(tl, -1, 0, 60), 1);
        CHECK_EQ(Voiceover::recordTrack(tl, -1, 0, 100), 0); // after its clip
        CHECK_EQ(Voiceover::recordTrack(tl, -1, -1, 10), 1);
        // Empty tracks the user named (Discord, Mikrofon …) and muted or locked ones get no take unless armed
        tl.audio[1].name = "Discord";
        tl.audio[3].muted = true;
        CHECK_EQ(Voiceover::recordTrack(tl, -1, -1, 10), 4); // nothing fits: one new track
        CHECK_EQ(Voiceover::recordTrack(tl, 1, -1, 10), 1);  // armed: the user's choice
        tl.audio[3].muted = false;
        tl.audio[3].locked = true;
        CHECK_EQ(Voiceover::recordTrack(tl, -1, -1, 10), 4);
        CHECK_EQ(Voiceover::recordTrack(tl, 3, -1, 10), -1);
        // A Voiceover track (made by an earlier take) is the place for takes, also over its earlier ones
        tl.audio[3].locked = false;
        tl.audio[3].name = "Voiceover";
        Clip v = a;
        v.id = 3;
        v.start = 0;
        tl.audio[3].clips << v; // [0, 50)
        CHECK_EQ(Voiceover::recordTrack(tl, -1, -1, 10, INT_MAX, "/x"), 3); // "/x": the take folder
        CHECK_EQ(Voiceover::recordTrack(tl, -1, 3, 10, INT_MAX, "/x"), 3);
        // Review 2026-11-09: anything else on it (the user's own sound) is never replaced by a take nobody armed: the
        // next free spot or a new track; after it the Voiceover track is fine again
        CHECK_EQ(Voiceover::recordTrack(tl, -1, -1, 10), 4);
        CHECK_EQ(Voiceover::recordTrack(tl, -1, -1, 10, INT_MAX, "/elsewhere"), 4);
        CHECK_EQ(Voiceover::recordTrack(tl, -1, -1, 50), 3);
        CHECK(Voiceover::isTake("/x/a.wav", "/x") && !Voiceover::isTake("/x/a.wav", "/y"));
        CHECK(Voiceover::isTake(Voiceover::folder(QString()) + "/voiceover-1.wav", "/y")); // taken before saving
        CHECK(Voiceover::isVoiceoverTrack(tl.audio[3]) && !Voiceover::isVoiceoverTrack(tl.audio[1]));
        // Known length (CLI): a free unnamed track must be free to the end
        Timeline t2;
        t2.audio.resize(2);
        t2.audio[0].clips << a; // [50, 100)
        CHECK_EQ(Voiceover::recordTrack(t2, -1, -1, 10, 40), 0);
        CHECK_EQ(Voiceover::recordTrack(t2, -1, -1, 10, 60), 1);
        // Takes in a row butt: a clip ending within the tolerance before or after the playhead -> its end
        CHECK_EQ(Voiceover::buttStart(tl, 3, 48, 3), 50);
        CHECK_EQ(Voiceover::buttStart(tl, 3, 52, 3), 50);
        CHECK_EQ(Voiceover::buttStart(tl, 3, 54, 3), 54);
        CHECK_EQ(Voiceover::buttStart(tl, 3, 0, 3), 0); // a clip starting there is no previous take
        CHECK_EQ(Voiceover::buttStart(tl, 9, 48, 3), 48);
        // The head recorded before the timeline was heard: back to the clip before on the track, not before 0, not
        // into a clip lying over the start (punch-in)
        CHECK_EQ(Voiceover::headLimit(tl, 3, 80), 50); // Voiceover clip [0, 50)
        CHECK_EQ(Voiceover::headLimit(tl, 3, 30), 30); // inside it
        CHECK_EQ(Voiceover::headLimit(tl, 1, 30), 0);  // empty track
        CHECK_EQ(Voiceover::headLimit(tl, 9, 30), 0);
        // A take that reaches back: shown from the file's start when that lies after the limit
        Voiceover::LiveTake back(46.5, 40, 25.0, 48000);
        CHECK(back.start() == 46.5);
        Voiceover::LiveTake clamped(36.0, 40, 25.0, 48000);
        CHECK(clamped.start() == 40.0);
    }

    // ---- Review 2026-11-09: the armed track / the take's track follows its track when tracks come or go above it
    {
        Timeline tl;
        tl.audio.resize(3);
        for (Track& t : tl.audio) t.kind = TrackKind::Audio;
        Clip a;
        a.id = 1;
        a.out = 49;
        tl.audio[0].clips << a; // A1 [0, 50)
        Clip b = a;
        b.id = 2;
        tl.audio[1].clips << b; // A2: the marked one
        const Voiceover::TrackKey key = Voiceover::trackKey(tl, 1);
        CHECK_EQ(Voiceover::findTrack(tl, key), 1);
        Timeline above = tl; // a track inserted at the top
        above.audio.insert(0, Track{TrackKind::Audio});
        CHECK_EQ(Voiceover::findTrack(above, key), 2);
        Timeline removed = tl; // A1 removed
        removed.audio.removeAt(0);
        CHECK_EQ(Voiceover::findTrack(removed, key), 0);
        Timeline gone = tl; // the marked one removed: not some other track
        gone.audio.removeAt(1);
        CHECK_EQ(Voiceover::findTrack(gone, key), -1);
        Timeline below = tl; // added below: unchanged
        below.audio << Track{TrackKind::Audio};
        CHECK_EQ(Voiceover::findTrack(below, key), 1);
        // An empty marked track (A3) with a track added above it
        const Voiceover::TrackKey empty = Voiceover::trackKey(tl, 2);
        Timeline named = tl;
        Track discord{TrackKind::Audio};
        discord.name = "Discord";
        discord.clips << a;
        named.audio.insert(1, discord);
        CHECK_EQ(Voiceover::findTrack(named, empty), 3);
        // A new track (index = count) stays a new one, also when the user added one meanwhile
        const Voiceover::TrackKey fresh = Voiceover::trackKey(tl, 3);
        CHECK_EQ(Voiceover::findTrack(below, fresh), 4);
        CHECK_EQ(Voiceover::findTrack(tl, Voiceover::trackKey(tl, -1)), -1);
    }

    // ---- The growing take: a fake source feeds blocks, the waveform and the length follow, the end is exact
    {
        // 25 fps, 48 kHz: 1920 samples a frame. File sample 0 at frame 18 (2 frames pre-roll), the clip from 20 on
        Voiceover::LiveTake take(18.0, 20, 25.0, 48000);
        CHECK_EQ(take.samples(), 0LL);
        CHECK(take.lengthFrames() == 0.0);
        std::vector<float> block(480); // 10 ms blocks
        auto feed = [&](int blocks, float level) {
            for (int b = 0; b < blocks; ++b) {
                for (size_t i = 0; i < block.size(); ++i) block[i] = (i % 2 ? -level : level);
                take.addSamples(block.data(), int(block.size()));
            }
        };
        feed(8, 0.5f); // 80 ms = 2 frames: the pre-roll, nothing shown yet
        CHECK(std::abs(take.end() - 20.0) < 1e-9);
        CHECK(take.lengthFrames() == 0.0);
        feed(40, 0.1f); // 400 ms quiet
        CHECK(std::abs(take.lengthFrames() - 10.0) < 1e-9);
        feed(40, 0.8f); // 400 ms loud
        CHECK(std::abs(take.end() - 40.0) < 1e-9);
        CHECK_EQ(take.peaks().size(), qsizetype(88)); // one per 10 ms
        CHECK(std::abs(take.peak(20, 30) - 0.1f) < 1e-6);
        CHECK(std::abs(take.peak(30, 40) - 0.8f) < 1e-6);
        CHECK(std::abs(take.peak(18, 20) - 0.5f) < 1e-6);
        CHECK(take.peak(40, 60) == 0.f); // not recorded yet
        // Blocks as the recorder sees them: a frame count and the peak since the call before; a count that went
        // back is ignored, an odd block end lands in the right bucket
        take.add(take.samples() + 7, 0.3f);
        CHECK_EQ(take.samples(), 88LL * 480 + 7);
        CHECK(std::abs(take.peaks().last() - 0.3f) < 1e-6);
        take.add(100, 1.0f);
        CHECK_EQ(take.samples(), 88LL * 480 + 7);
        // The length is the samples' exact length: 42247 samples = 22.003… frames from frame 18
        CHECK(std::abs(take.end() - (18.0 + 42247.0 / 1920.0)) < 1e-9);
        // The Alignment places the file a frame earlier: the take ends a frame earlier, the waveform moves along
        take.setFileStart(17.0);
        CHECK(std::abs(take.end() - (17.0 + 42247.0 / 1920.0)) < 1e-9);
        CHECK(std::abs(take.peak(29, 39) - 0.8f) < 1e-6);
    }

    // ---- Editor::placeRecording: onto the given track (punch-in), a new one at most one past the last, one step
    {
        Project p;
        Selection sel;
        Editor ed(&p, &sel);
        p.addMedia({"/x/a.mp4", "a.mp4", 100, true, true, false});
        p.addMedia({"/x/vo.wav", "vo.wav", 60, false, true, false});
        p.edit("setup", [&](Timeline& tl) {
            Clip a;
            a.id = p.newClipId();
            a.mediaPath = "/x/a.mp4";
            a.out = 99;
            tl.audio[0].clips << a;
        });
        const int undoBefore = p.undoStack()->index();
        const int tracks = int(p.timeline().audio.size());
        int placed = -9;
        // Onto the track after the last (a new one) from frame 20, file 0.5 frames late
        int nothing = -1;
        const int id = ed.placeRecording("/x/vo.wav", tracks, 19.5, 20, &placed, 0, &nothing);
        CHECK(id > 0);
        CHECK_EQ(nothing, 0);
        CHECK_EQ(placed, tracks);
        if (CHECK_EQ(int(p.timeline().audio.size()), tracks + 1) && p.timeline().audio[tracks].clips.size() == 1) {
            const Clip& c = p.timeline().audio[tracks].clips[0];
            CHECK_EQ(c.id, id);
            CHECK_EQ(c.start, 20);
            CHECK_EQ(c.subframe, 50);
            CHECK_EQ(c.in, 1);
            CHECK_EQ(c.out, 59);
            CHECK_EQ(c.linkId, 0);
            CHECK_EQ(p.timeline().audio[tracks].name, QString("Voiceover"));
        }
        CHECK(sel.ids() == QSet<int>{id});
        CHECK_EQ(p.undoStack()->index(), undoBefore + 1);
        p.undoStack()->undo(); // one step takes the track and the clip away
        CHECK_EQ(int(p.timeline().audio.size()), tracks);
        // Far past the last track: still only one new track
        CHECK(ed.placeRecording("/x/vo.wav", tracks + 5, 0, 0, &placed) > 0);
        CHECK_EQ(placed, tracks);
        CHECK_EQ(int(p.timeline().audio.size()), tracks + 1);
        p.undoStack()->undo();
        // Punch-in on A1 over its sound: a[0-30] vo[30-90] a[90-100], one undo restores it
        int replaced = -1;
        CHECK(ed.placeRecording("/x/vo.wav", 0, 30.0, 30, &placed, 0, &replaced) > 0);
        CHECK_EQ(placed, 0);
        CHECK_EQ(replaced, 1); // said in the status bar
        CHECK_EQ(p.timeline().audio[0].clips.size(), qsizetype(3));
        p.undoStack()->undo();
        CHECK_EQ(p.timeline().audio[0].clips.size(), qsizetype(1));
        // Not in the pool, nothing after the start, a locked track: no clip, no step
        const int index = p.undoStack()->index();
        CHECK_EQ(ed.placeRecording("/x/none.wav", 0, 0, 0), 0);
        CHECK_EQ(ed.placeRecording("/x/vo.wav", 0, 0, 200), 0);
        CHECK_EQ(ed.placeRecording("/x/vo.wav", -1, 0, 0), 0);
        p.edit("lock", [](Timeline& t) { t.audio[0].locked = true; });
        CHECK_EQ(ed.placeRecording("/x/vo.wav", 0, 0, 0), 0);
        CHECK_EQ(p.undoStack()->index(), index + 1);
    }

    // ---- CLI op 'voiceover': a given track = armed (punch-in), else the app's rule, 'new' = one new track
    if (QFileInfo::exists(tmp.filePath("take.wav"))) {
        const QString path = tmp.filePath("cli.schneidi");
        Project p;
        p.addMedia({"/x/a.mp4", "a.mp4", 100, true, true, false});
        p.edit("setup", [&](Timeline& tl) {
            Clip a;
            a.id = p.newClipId();
            a.mediaPath = "/x/a.mp4";
            a.out = 99;
            tl.audio[0].clips << a;
            TimelineOps::ensureTracks(tl, TrackKind::Audio, 2); // A2 empty, no name
        });
        QString error;
        CHECK(ProjectFile::save(p.data(), path, &error));
        auto edit = [&](const QString& ops, QString* err = nullptr) {
            return cli("edit", {{"project", path}, {"ops", ops}}, err);
        };
        auto loaded = [&] {
            ProjectData x;
            QString e;
            ProjectFile::load(path, &x, &e);
            return x.timeline;
        };
        const int tracks = int(p.timeline().audio.size());
        const QString take = tmp.filePath("take.wav");
        // No track: A1 has sound at 2 s, A2 is free and unnamed: there, no new track
        QJsonObject r = edit(QString(R"([{"op":"voiceover","file":"%1","at":"2s"}])").arg(take));
        Timeline tl = loaded();
        CHECK_EQ(int(tl.audio.size()), tracks);
        CHECK(tl.audio[1].clips.size() == 1 && tl.audio[1].clips[0].start == 50);
        // track A1 = armed: punch-in over its sound (a | take | a)
        edit(QString(R"([{"op":"voiceover","file":"%1","at":"1s","track":"A1"}])").arg(take));
        tl = loaded();
        CHECK_EQ(tl.audio[0].clips.size(), qsizetype(3));
        // track 'new': one new track; a video track is refused
        const int now = int(tl.audio.size());
        r = edit(QString(R"([{"op":"voiceover","file":"%1","at":0,"track":"new"}])").arg(take));
        CHECK(QJsonDocument(r).toJson().contains("a new track"));
        CHECK_EQ(int(loaded().audio.size()), now + 1);
        QString code;
        edit(QString(R"([{"op":"voiceover","file":"%1","track":"V1"}])").arg(take), &code);
        CHECK_EQ(code, QString("BAD_ARGUMENT"));
        edit(R"([{"op":"voiceover"}])", &code);
        CHECK_EQ(code, QString("BAD_ARGUMENT"));
    } else {
        CHECK(false);
    }

    // ---- File names: next to the project, never over an earlier take
    {
        CHECK_EQ(Voiceover::folder(tmp.filePath("Mein Short.schneidi")), tmp.filePath("Mein Short-voiceover"));
        CHECK(Voiceover::folder(QString()).endsWith("voiceover"));
        const QDateTime when(QDate(2026, 11, 2), QTime(14, 5, 9));
        const QString dir = tmp.filePath("names");
        QDir().mkpath(dir);
        const QString first = Voiceover::uniquePath(dir, when);
        CHECK_EQ(QFileInfo(first).fileName(), QString("voiceover-20261102-140509.wav"));
        QFile(first).open(QIODevice::WriteOnly);
        const QString second = Voiceover::uniquePath(dir, when);
        CHECK_EQ(QFileInfo(second).fileName(), QString("voiceover-20261102-140509-2.wav"));
        QFile(second).open(QIODevice::WriteOnly);
        CHECK_EQ(QFileInfo(Voiceover::uniquePath(dir, when)).fileName(), QString("voiceover-20261102-140509-3.wav"));
    }

    // ---- Settings round trip (defaults: system input, no offset, no pre-roll)
    {
        QSettings().clear();
        CHECK(Voiceover::Settings::load() == Voiceover::Settings{});
        Voiceover::Settings s;
        s.input = "alsa_input.usb-Mic-00.mono-fallback";
        s.offsetMs = -12;
        s.prerollSeconds = 2;
        s.save();
        CHECK(Voiceover::Settings::load() == s);
        QSettings().setValue("voiceover/preroll", 99); // out of range from a hand-edited file
        CHECK_EQ(Voiceover::Settings::load().prerollSeconds, 10);
        QSettings().clear();
    }
    return Check::result();
}
