// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Own audio engine: sound input (voice-over). A capture device hands interleaved float samples to a callback in its
// real-time thread; that only copies them into a ring buffer, a writer thread puts them into a WAV file (24-bit PCM)
// as they come. Nothing is lost while the file grows: the header gets its sizes when the recording stops.
//
// Kinds like Audio::Device: an input through miniaudio (PulseAudio/PipeWire/ALSA/JACK, WASAPI/DirectSound,
// CoreAudio) or a stand-in paced by a timer (tests, no microphone): the environment variable SCHNEIDI_AUDIO_INPUT =
// "null" (silence) or "sine:<Hz>" (a tone at -12 dBFS), or the backend "null". The input is never heard (no
// monitoring: a microphone next to speakers would feed back).

#include "engine/audio/Device.h"

#include <QString>

#include <memory>
#include <vector>

namespace Audio {

class Capture {
public:
    struct Config {
        int rate = 48000;
        int channels = 1;  // a voice: mono; the device's channels are mixed down by miniaudio
        QString backend;   // Device::Config::backend
        QString input;     // Device::Output::id of an input; "" = the system default input
        int buffer = 1024; // samples the device buffers (two periods)
    };

    // Opens the input and records into `path` (created/overwritten) until stop() or destruction. nullptr = no input
    // opened or the file cannot be written; *error says why.
    static std::unique_ptr<Capture> start(const Config& config, const QString& path, QString* error = nullptr);
    ~Capture();

    // Ends the recording: no samples taken any more, everything taken is written, the WAV header is completed.
    // false = writing failed (*error says why). Further calls do nothing.
    bool stop(QString* error = nullptr);

    // Sample frames the input has delivered so far (any thread). The file holds exactly these once stopped, minus
    // dropped().
    long long frames() const;
    // Frames lost because the writer fell behind by more than the ring buffer holds (a stalled disk); 0 normally
    long long dropped() const;
    // Peak level (linear, over all channels) since the last call (any thread; the meter)
    float takePeak();
    // Samples between the sound reaching the input and the callback getting it (the device's buffer)
    int latency() const;
    int rate() const;
    int channels() const;
    QString name() const;
    QString path() const;

    // Inputs of a backend ("" = automatic) right now
    static std::vector<Device::Output> inputs(const QString& backend);

    struct Impl;

private:
    explicit Capture(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> d;
};

} // namespace Audio
