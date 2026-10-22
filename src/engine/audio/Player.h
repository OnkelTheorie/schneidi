#pragma once
// Own audio engine (docs/plan-0.10-ton.md), step 5: live playback. The same Audio::Mixer as export and analyses mixes
// the timeline ahead in a feeder thread into a ring buffer; the device callback (real time: no locks, no allocation)
// only copies from it and fades. The device stays open – play, pause, jumps and speed changes start a new
// "generation": the callback fades the old sound out within 5 ms, drops the rest and fades the new one in.
//
// The clock is the sound: position() = the timeline sample heard right now, counted from the samples the device
// really took (minus its buffer), so the picture follows the sound. Playback starts silent with the clock standing
// until release() says the picture is ready (or 0.3 s have passed).
//
// Positions are samples of the timeline at the output rate. The clock sits between samples: forward, after hearing
// sample k it is k + 1; backward it is k. A frame f plays forward over [start(f), start(f + 1)).
//
// The device can vanish while playing (headphones unplugged): a watchdog notices when it stops taking samples and
// reopens – the chosen output, else the system default, else silence (the clock runs on) – and goes back to the
// chosen output once it is there again. The picture stands for that moment (~0.3 s), then both run on in sync.
//
// Speeds: 1 and -1 sound normal; 2 and 4 (and backwards) play the sound faster like tape; other speeds are silent
// while the clock runs.

#include "core/ProjectFormat.h"
#include "core/Types.h"

#include <QHash>
#include <QString>

#include <memory>
#include <vector>

struct SharedLoudness;

namespace Audio {

class Player {
public:
    struct Options {
        int rate = 48000;
        int buffer = 1024; // samples the device buffers
        QString backend;   // Device::Config::backend
        QString output;    // Device::Config::output ("" = system default)
    };
    explicit Player(const Options& options);
    Player() : Player(Options{}) {}
    ~Player();

    // What plays. While playing the sound continues at the same place: mixer panel values glide to the new ones,
    // anything else crossfades to the new mix (10 ms). fps = frame rate of the timeline.
    void setTimeline(const Timeline& timeline, FrameRate fps);
    // Lengths of media files in frames as the project knows them (the export's ExportSettings::mediaLengths): the
    // mix must place reversed clips and transition handles like the export. Files missing here are measured.
    void setFileLengths(const QHash<QString, int>& lengths);
    // Device settings; an open device is reopened with them at once (a short gap while playing)
    void setDevice(int buffer, const QString& backend, const QString& output);

    // Starts playing at clock position `position` with `speed` (negative = backwards); while playing it is a jump or
    // a speed change. The sound fades in once release() is called or 0.3 s have passed.
    void play(double position, double speed);
    void pause(); // fades out; position() stays where the sound stopped
    bool playing() const;
    double speed() const;
    // The picture is ready: the clock may start
    void release();
    // Dragging the playhead while playing: silent (fading) until unmuted, the clock runs on
    void setMuted(bool muted);

    // The clock (see above). Paused: where the sound stopped (or the last play() position)
    double position() const;

    // Peak levels of the sound heard since the last call (linear): per audio track L, R after fader/pan, then the
    // master L, R as true peak; empty = nothing heard
    std::vector<float> takeLevels();
    // Master loudness while it is active (only speed 1); nullptr = none. The meter must outlive the player.
    void setLoudness(SharedLoudness* loudness);

    QString deviceName() const;
    QString deviceError() const; // why the sound card did not open (playing silent), empty = fine

    struct Stats {
        int underruns = 0;  // the sound ran dry while playing (device callbacks)
        int mixers = 0;     // mixers built (timeline changes that needed more than updateMix)
        int liveUpdates = 0; // timeline changes taken over by Mixer::updateMix
        int fadeOuts = 0;   // generations faded out (jumps, stops, speed changes)
        int reopens = 0;    // device reopened because it went silent or the chosen output came back
        int reroutes = 0;   // the system moved the sound to another output by itself
    };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace Audio
