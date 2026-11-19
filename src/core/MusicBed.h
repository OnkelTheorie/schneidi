// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Music beds of schneidi's own (edit op 'music_bed'): background music computed from simple synth voices, no samples,
// no recordings, so it is free to use and its beat grid is known exactly (cuts snap to it without an analysis).
//
// Three moods: calm (pad, soft keys, round bass, a gentle half-time groove), drive (pad, pulsing eighth-note bass,
// plucked stabs, a pop/rock beat) and hype (pumping supersaw pad, off-beat bass, a 16th arp with echoes, four on the
// floor, risers before the drops). A chord progression per mood (one chord per bar, phrases of four bars), the
// arrangement on bar boundaries: intro, main parts (a lift in every second eight bars), short breaks in long beds, a
// final hit on the tonic at a bar one and its ring-out, faded so the bed ends exactly at the asked length.
//
// Voices: wavetable oscillators (band-limited harmonics, a bright and a dark table blended, two detuned copies for the
// stereo width), raised-cosine attacks and releases (no clicks at note boundaries), drums from pitched sines and
// filtered noise, a small reverb, side-chain pumping on the beat. The master is DC-free, normalized to kTargetLufs
// (BS.1770) and held under kCeilingDbtp true peak. Deterministic: the same options give the same samples; `seed` picks
// another progression and patterns.

#include "core/Beats.h"

#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace MusicBed {

constexpr int kRate = 48000;
constexpr int kChannels = 2;
// Bumped when the sound changes: part of the file names (a bed made by an older version is made again)
constexpr int kVersion = 1;
constexpr double kMinLength = 1.0, kMaxLength = 600.0; // seconds
// Lengths this far outside are taken as they are: a timeline length in frames is up to half a frame off whole
// seconds (600 s at 23.976 fps are 14386 frames = 600.008 s)
constexpr double kLengthSlack = 0.05;
constexpr double kMinBpm = 60, kMaxBpm = 180;
constexpr double kTargetLufs = -16.0;  // integrated loudness of the bed alone
constexpr double kCeilingDbtp = -1.0;  // true peak at most

enum class Mood { Calm, Drive, Hype };
constexpr Mood kMoods[] = {Mood::Calm, Mood::Drive, Mood::Hype};

QString moodId(Mood m);                         // "calm", "drive", "hype"
std::optional<Mood> moodFrom(const QString& s); // the ids, also "ruhig", "treibend" (case does not matter)
QStringList moodIds();
const char* moodLabel(Mood m);   // German UI key for T(): "Ruhig", "Treibend", "Hype"
QString description(Mood m);     // one line, English (CLI/MCP)
double defaultBpm(Mood m);

// A key: tonic pitch class (0 = C … 11 = B) and mode
struct Key {
    int root = 0;
    bool minor = false;
    bool operator==(const Key&) const = default;
};
// "A", "a minor", "F#m", "Bb", "Eb major", "c-moll", "D-Dur"; nullopt = not a key
std::optional<Key> keyFrom(const QString& text);
QString keyName(const Key& k); // "A minor", "F# major"
Key defaultKey(Mood m);

struct Options {
    Mood mood = Mood::Drive;
    double length = 30;          // seconds (clamped to kMinLength..kMaxLength, kLengthSlack beyond)
    double bpm = 0;              // 0 = the mood's tempo (else clamped to kMinBpm..kMaxBpm)
    std::optional<Key> key;      // none = the mood's key
    unsigned seed = 1;           // 1 = the mood's first progression and patterns; others vary them
};

struct Section {
    enum Kind { Intro, Main, Break, Ending };
    Kind kind = Main;
    int fromBar = 0, toBar = 0; // [from, to); the ending runs from the final hit to the end of the bed
};
QString sectionName(Section::Kind k); // "intro", "main", "break", "ending"

struct Result {
    std::vector<float> samples; // interleaved stereo, kRate
    double length = 0;          // seconds (= frames / kRate)
    Mood mood = Mood::Drive;
    double bpm = 0;
    Key key;
    unsigned seed = 1;
    QStringList chords;          // the progression ("Am", "F", "C", "G")
    std::vector<Section> sections;
    int endBar = -1;             // bar of the final hit (-1: too short for one, the bed fades out)
    Beats::Result beats;         // the known grid: beat k at phase + k * period, bar ones at k % 4 == 0
    double lufs = 0, truePeakDb = 0; // measured on the result
};

// Whole sample frames of a length in seconds (rounded)
long long frameCount(double seconds);
// The bed
Result make(const Options& o);
// The same without making the sound (no samples, no levels per bar, no breaks/drops, lufs/truePeakDb 0): fast
Result describe(const Options& o);
// 24-bit PCM WAV, stereo, kRate (the same file every time for the same result)
bool writeWav(const QString& path, const Result& r);
// File name of a bed: mood, tempo, key, seed, length and kVersion ("music-bed-drive-112bpm-a-minor-s1-30000ms-v1.wav")
QString fileName(const Options& o);

} // namespace MusicBed
