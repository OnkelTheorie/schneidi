#pragma once
// Own audio engine (docs/plan-0.10-ton.md), step 5: the sound output. A device pulls interleaved float samples from a
// callback in its own real-time thread and keeps running while open; what it plays (sound, silence, fades) is the
// player's business, so play/pause/seek never close or reopen it.
//
// Kinds: the sound card through miniaudio (ALSA/PulseAudio/PipeWire/JACK, WASAPI/DirectSound, CoreAudio), a file
// written in real time (tests: what the speaker would get, like SDL's "disk" driver) and silence paced by a timer (no
// sound card, tests). Input (voice-over) goes through the same library later.

#include <QString>

#include <functional>
#include <memory>

namespace Audio {

class Device {
public:
    // Fills `frames` interleaved samples; runs in the device thread: no locks, no allocation, no file access
    using Callback = std::function<void(float* out, int frames)>;
    struct Config {
        int rate = 48000;
        int channels = 2;
        int buffer = 1024;  // samples the device buffers (two periods); more = fewer dropouts, more latency
        QString backend;    // "" = automatic; "pulseaudio", "alsa", "jack", "wasapi", "directsound", "coreaudio", "null"
    };

    virtual ~Device() = default;
    // Samples between handing over the end of a callback's block and hearing it (the device's buffer)
    virtual int latency() const = 0;
    virtual QString name() const = 0;

    // Opens and starts the output. The environment variable SCHNEIDI_AUDIO_DEVICE chooses a stand-in: "null" =
    // silence paced by a timer, "file:<path>" = 16-bit stereo raw file in real time (tests). Otherwise the sound card;
    // if none opens, silence (the clock must run: picture follows sound) and *error says why.
    static std::unique_ptr<Device> open(const Config& config, Callback callback, QString* error = nullptr);
    // miniaudio backends that can be chosen on this system (for the menu), "" first
    static QStringList backends();
};

} // namespace Audio
