#pragma once
// Own audio engine (docs/plan-0.10-ton.md), step 5: the sound output. A device pulls interleaved float samples from a
// callback in its own real-time thread and keeps running while open; what it plays (sound, silence, fades) is the
// player's business, so play/pause/seek never close or reopen it.
//
// Kinds: the sound card through miniaudio (ALSA/PulseAudio/PipeWire/JACK, WASAPI/DirectSound, CoreAudio), a file
// written in real time (tests: what the speaker would get, like SDL's "disk" driver) and silence paced by a timer (no
// sound card, tests). Input (voice-over) goes through the same library later.
//
// A device can vanish while open (headphones unplugged, Bluetooth gone). Backends differ in what they tell: PipeWire
// moves a stream on the system default to the new default by itself, a stream on a chosen device just stops being
// called without any notice (measured, docs/dev/ton.md). So the player watches the callbacks itself and reopens
// (Audio::Player), the device only says whether it is what was asked for (fallback()).

#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <vector>

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
        QString output;     // Output::id; "" = the system default (follows it where the system moves streams)
    };
    struct Output {
        QString id;   // stable across restarts and languages (PulseAudio: sink name, WASAPI: endpoint id, …)
        QString name; // for people, translated by the system
        bool isDefault = false;
    };

    virtual ~Device() = default;
    // Samples between handing over the end of a callback's block and hearing it (the device's buffer)
    virtual int latency() const = 0;
    virtual QString name() const = 0;
    // Not what the config asked for: the chosen output is missing (system default instead) or no sound card opened
    // (silence). The player tries to get the asked one back now and then.
    virtual bool fallback() const { return false; }
    // How often the system moved the stream to another output by itself (PipeWire, WASAPI default device)
    virtual int reroutes() const { return 0; }
    // Never call the callback again (at once, from any thread). Closing a vanished device can take long or hang in
    // the backend; detached, it can be closed in another thread while a new one plays.
    virtual void detach() {}

    // Opens and starts the output. The environment variable SCHNEIDI_AUDIO_DEVICE chooses a stand-in: "null" =
    // silence paced by a timer, "file:<path>" = 16-bit stereo raw file in real time (tests); "null:stall=<ms>" = like
    // "null", but the first device opened in the process stops calling back after <ms> (a vanished device, tests).
    // Otherwise the sound card: the chosen output, if it is missing the system default; if none opens, silence (the
    // clock must run: picture follows sound) and *error says why.
    static std::unique_ptr<Device> open(const Config& config, Callback callback, QString* error = nullptr);
    // Whether open() would now get what `config` asks for (the chosen output, or any sound card for "")
    static bool available(const Config& config);
    // miniaudio backends that can be chosen on this system (for the menu), "" first
    static QStringList backends();
    // Outputs of a backend ("" = automatic) right now
    static std::vector<Output> outputs(const QString& backend);
};

} // namespace Audio
