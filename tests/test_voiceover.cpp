// SPDX-License-Identifier: AGPL-3.0-only
// Voice-over recording (0.16): the sound input into a WAV file (a stand-in input paced by a timer, never the
// microphone), lining the file up with the timeline, placing it on the armed track (one undo step), file names,
// settings
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
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>

#include <cmath>

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
    if (w.bits != 24 || 44 + qint64(bytes) != b.size()) return w; // sizes in the header must match the file
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
                worst = std::max(worst, std::abs(w.samples[i] - 0.25 * std::sin(2 * M_PI * 1000.0 * i / 48000)));
            CHECK(worst < 1e-5);
            // The file reads as audio in the project's frame rate
            const MediaInfo m = Engine::probe(ProjectFormat{}, path);
            CHECK(m.hasAudio && !m.hasVideo);
            CHECK(std::abs(m.length - int(std::lround(double(frames) / 48000 * ProjectFormat{}.rate.fps()))) <= 1);
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
    }

    // ---- Editor::placeRecording: onto the armed track at the playhead, overwrites, one undo step
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
        const int id = ed.placeRecording("/x/vo.wav", 1, 19.5, 20);
        CHECK(id > 0);
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
        // Recording over existing sound on A1 overwrites that range
        const int id2 = ed.placeRecording("/x/vo.wav", 0, 30.0, 30);
        CHECK(id2 > 0);
        CHECK_EQ(p.timeline().audio[0].clips.size(), qsizetype(3)); // a[0-30] vo[30-90] a[90-100]
        p.undoStack()->undo();
        CHECK_EQ(p.timeline().audio[0].clips.size(), qsizetype(1));
        p.undoStack()->undo();
        CHECK(p.timeline().audio[1].clips.isEmpty());
        // Not in the pool, locked track, nothing after the start: no clip, no step
        const int index = p.undoStack()->index();
        CHECK_EQ(ed.placeRecording("/x/none.wav", 0, 0, 0), 0);
        CHECK_EQ(ed.placeRecording("/x/vo.wav", 0, 0, 200), 0);
        p.edit("lock", [](Timeline& t) { t.audio[0].locked = true; });
        CHECK_EQ(ed.placeRecording("/x/vo.wav", 0, 0, 0), 0);
        CHECK_EQ(p.undoStack()->index(), index + 1);
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

    // ---- Settings round trip (defaults: system input, no offset, no pre-roll, track not muted)
    {
        QSettings().clear();
        CHECK(Voiceover::Settings::load() == Voiceover::Settings{});
        Voiceover::Settings s;
        s.input = "alsa_input.usb-Mic-00.mono-fallback";
        s.offsetMs = -12;
        s.prerollSeconds = 2;
        s.muteTrack = true;
        s.save();
        CHECK(Voiceover::Settings::load() == s);
        QSettings().setValue("voiceover/preroll", 99); // out of range from a hand-edited file
        CHECK_EQ(Voiceover::Settings::load().prerollSeconds, 10);
        QSettings().clear();
    }
    return Check::result();
}
