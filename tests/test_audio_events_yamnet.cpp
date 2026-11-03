// SPDX-License-Identifier: AGPL-3.0-only
// Audio events with the real YAMNet model (extension "audio-events", or SCHNEIDI_YAMNET = a yamnet .onnx with its
// class list beside it) and ONNX Runtime. Skipped when either is missing - never downloaded here. On the synthetic
// sounds of tests/synth_sounds.h: music and applause where they are, the result by "yamnet", cached, every event a
// known kind. Loose on purpose: YAMNet was trained on real recordings, not on formant synthesis.
#include "engine/AudioEvents.h"
#include "engine/Inference.h"

#include "check.h"
#include "synth_sounds.h"

#include <QGuiApplication>
#include <QTemporaryDir>

#include <cstdio>

namespace {

double covered(const QVector<AudioEvents::Event>& ev, const QString& kind, double from, double to)
{
    double s = 0;
    for (const AudioEvents::Event& e : ev)
        if (e.kind == kind) s += std::max(0.0, std::min(e.to, to) - std::max(e.from, from));
    return s;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("audio-events-yamnet");
    Check::initEngine();
    QString why;
    if (!AudioEvents::yamnetAvailable(&why)) {
        std::printf("%s\n", qPrintable(why));
        return Check::skip("YAMNet or ONNX Runtime not installed");
    }
    std::printf("ONNX Runtime %s, model %s\n", qPrintable(Inference::version()), qPrintable(qEnvironmentVariable("SCHNEIDI_YAMNET")));
    QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/audio-events").removeRecursively();
    QTemporaryDir tmp;
    Synth::Builder b(48000, 20, 9);
    Synth::music(b, 1.0, 6.0);     // 1-7
    Synth::applause(b, 9.0, 4.0);  // 9-13
    Synth::shot(b, 16.0);          // 16
    b.floor();
    const QString wav = QDir(tmp.path()).filePath("yamnet.wav");
    CHECK(Synth::writeWav(wav, b.s, 48000));
    AudioEvents::Options o;
    o.engine = AudioEvents::Engine::Yamnet;
    const AudioEvents::Result r = AudioEvents::analyzeFile(wav, 0, o);
    CHECK(r.error.isEmpty());
    CHECK_EQ(r.by, QString("yamnet"));
    for (const AudioEvents::Event& e : r.events) {
        std::printf("  %s %.2f-%.2f %.2f %s\n", qPrintable(e.kind), e.from, e.to, e.conf, qPrintable(e.by));
        CHECK(AudioEvents::kinds().contains(e.kind));
    }
    CHECK(covered(r.events, "music", 1, 7) >= 3);
    CHECK(covered(r.events, "applause", 9, 13) + covered(r.events, "cheering", 9, 13) >= 2);
    bool shot = false;
    for (const AudioEvents::Event& e : r.events) shot |= e.kind == "shot" && std::abs(e.from - 16.0) < 0.5;
    CHECK(shot);
    CHECK(AudioEvents::analyzeFile(wav, 0, o).cached);
    return Check::result();
}
