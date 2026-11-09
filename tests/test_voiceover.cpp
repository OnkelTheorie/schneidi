// SPDX-License-Identifier: AGPL-3.0-only
// Voice-over recording (0.16, 0.20 straight onto a track): the sound input into a WAV file (a stand-in input paced by
// a timer, never the microphone), a level meter without a file, lining the file up with the timeline, the track a
// take goes to (armed, selected, none or sound in the way -> a new one), placing it (one undo step), the growing take
// (live waveform, exact length), file names, settings
#include "core/Editor.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/Voiceover.h"
#include "engine/Engine.h"
#include "engine/audio/Capture.h"

#include "check.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>

#include <cmath>
#include <numbers>

namespace {

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

    // ---- Which track a take goes to: armed, else the selected clips' audio track, else none (= a new one)
    {
        Timeline tl;
        tl.audio.resize(3);
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
        CHECK_EQ(Voiceover::preferredTrack(tl, 1, {}), 1);      // armed
        CHECK_EQ(Voiceover::preferredTrack(tl, 1, {1}), 1);     // armed before the selection
        CHECK_EQ(Voiceover::preferredTrack(tl, -1, {1}), 0);    // selected clip on A1
        CHECK_EQ(Voiceover::preferredTrack(tl, -1, {}), -1);    // nothing
        CHECK_EQ(Voiceover::preferredTrack(tl, -1, {1, 2}), -1); // clips on two tracks: none of them
        CHECK_EQ(Voiceover::preferredTrack(tl, 7, {2}), 2);     // an armed track that is gone does not count
        // Free there: that track; sound in the way, locked, none or a missing one: a new track (= the count)
        CHECK_EQ(Voiceover::recordTrack(tl, 0, 10, 50), 0);  // ends right where the clip starts
        CHECK_EQ(Voiceover::recordTrack(tl, 0, 10, 51), 3);  // reaches into it
        CHECK_EQ(Voiceover::recordTrack(tl, 0, 60, 61), 3);  // starts on it
        CHECK_EQ(Voiceover::recordTrack(tl, 0, 100, 400), 0); // after it
        CHECK_EQ(Voiceover::recordTrack(tl, 1, 0, 1000), 1);
        CHECK_EQ(Voiceover::recordTrack(tl, -1, 0, 10), 3);
        CHECK_EQ(Voiceover::recordTrack(tl, 9, 0, 10), 3);
        tl.audio[1].locked = true;
        CHECK_EQ(Voiceover::recordTrack(tl, 1, 0, 10), 3);
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

    // ---- Editor::placeRecording: onto the track at the playhead, never over sound (a new track instead, one step)
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
        // Recorded on A2 (missing track is created) from frame 20, file 0.5 frames late
        int placed = -9;
        const int id = ed.placeRecording("/x/vo.wav", 1, 19.5, 20, &placed);
        CHECK(id > 0);
        CHECK_EQ(placed, 1);
        const Timeline& tl = p.timeline();
        CHECK(tl.audio.size() >= 2);
        if (tl.audio.size() >= 2 && tl.audio[1].clips.size() == 1) {
            const Clip& c = tl.audio[1].clips[0];
            CHECK_EQ(c.id, id);
            CHECK_EQ(c.start, 20);
            CHECK_EQ(c.subframe, 50);
            CHECK_EQ(c.in, 1);
            CHECK_EQ(c.out, 59);
            CHECK_EQ(c.linkId, 0);
        } else {
            CHECK(false);
        }
        CHECK(sel.ids() == QSet<int>{id});
        CHECK_EQ(p.undoStack()->index(), undoBefore + 1);
        CHECK_EQ(tl.audio[0].clips.size(), qsizetype(1)); // A1 untouched
        p.undoStack()->undo();
        CHECK(p.timeline().audio.size() < 2 || p.timeline().audio[1].clips.isEmpty());
        // A1 has sound at frame 30: a new track below the others, named Voiceover, with the clip in one step;
        // the sound on A1 stays
        const int tracks = int(p.timeline().audio.size());
        const int id2 = ed.placeRecording("/x/vo.wav", 0, 30.0, 30, &placed);
        CHECK(id2 > 0);
        CHECK_EQ(placed, tracks);
        CHECK_EQ(int(p.timeline().audio.size()), tracks + 1);
        CHECK_EQ(p.timeline().audio[0].clips.size(), qsizetype(1));
        if (placed >= 0 && placed < p.timeline().audio.size()) {
            CHECK_EQ(p.timeline().audio[placed].name, QString("Voiceover"));
            CHECK(p.timeline().audio[placed].clips.size() == 1 && p.timeline().audio[placed].clips[0].start == 30);
        }
        CHECK_EQ(p.undoStack()->index(), undoBefore + 1);
        p.undoStack()->undo(); // one step takes the track and the clip away
        CHECK_EQ(int(p.timeline().audio.size()), tracks);
        CHECK_EQ(p.timeline().audio[0].clips.size(), qsizetype(1));
        // No track asked for (nothing armed or selected): a new one
        CHECK(ed.placeRecording("/x/vo.wav", -1, 0, 0, &placed) > 0);
        CHECK_EQ(placed, tracks);
        p.undoStack()->undo();
        // A free track after the clip: that one
        CHECK(ed.placeRecording("/x/vo.wav", 0, 100, 100, &placed) > 0);
        CHECK_EQ(placed, 0);
        CHECK_EQ(p.timeline().audio[0].clips.size(), qsizetype(2));
        p.undoStack()->undo();
        // Not in the pool, nothing after the start: no clip, no step. A locked track: a new one
        const int index = p.undoStack()->index();
        CHECK_EQ(ed.placeRecording("/x/none.wav", 0, 0, 0), 0);
        CHECK_EQ(ed.placeRecording("/x/vo.wav", 0, 0, 200), 0);
        CHECK_EQ(p.undoStack()->index(), index);
        p.edit("lock", [](Timeline& t) { t.audio[0].locked = true; });
        CHECK(ed.placeRecording("/x/vo.wav", 0, 200, 200, &placed) > 0);
        CHECK_EQ(placed, tracks);
        CHECK_EQ(p.undoStack()->index(), index + 2);
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
