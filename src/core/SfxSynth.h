// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Effect sounds of schneidi's own (op 'sfx', the Effects Library), computed, no recordings: C++ ports of earshot's
// sound recipes (Leon Lin, MIT; core/SfxWhoosh: whoosh, swish; core/SfxForge: riser, hit, impact, click) with their
// bank's parameter sets. Each kind has variants by seed (1-3: picked takes); the same kind, seed and length give the
// same samples. Levels are mild on purpose (a sound under the game sound, never on top of it): the loudest sample of
// every sound lies at its kind's peak level, at most -1 dBFS.

#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace SfxSynth {

constexpr int kRate = 48000; // mono, 48 kHz
// Bumped when a sound changes: part of the cache file names
constexpr int kVersion = 6;

enum class Kind { Whoosh, Riser, Hit, Impact, Click, Swish };
constexpr Kind kKinds[] = {Kind::Whoosh, Kind::Riser, Kind::Hit, Kind::Impact, Kind::Click, Kind::Swish};

QString kindName(Kind k); // "whoosh", "riser" …
std::optional<Kind> kindFrom(const QString& name);
QStringList kindNames();
QString description(Kind k); // one line, English (CLI/MCP)
double defaultLength(Kind k); // seconds
// Length range a kind takes (riser: 0.3-8 s to its peak; the others keep their shape, 0.5x-2x of the default). Whoosh and swish
// end where their tail has faded: their length is about the asked one (Sound::length says it)
double minLength(Kind k);
double maxLength(Kind k);
double peakLevelDb(Kind k); // the loudest sample, dBFS

struct Sound {
    Kind kind = Kind::Hit;
    unsigned seed = 1;
    double length = 0;    // seconds
    double anchor = 0;    // seconds into the sound that lands on the moment: whoosh peak, riser end, hit attack
    std::vector<float> samples; // mono, kRate
};

// The sound; length <= 0 = the kind's default (clamped to its range)
Sound make(Kind kind, unsigned seed = 1, double length = 0);
// Where the moment lies in a sound of this kind and length without making it (seconds from its start)
double anchorOf(Kind kind, unsigned seed, double length);

double peakDb(const std::vector<float>& samples); // -inf for silence
// 16-bit PCM WAV, mono, kRate; samples quantized (with a fixed dither) the same way every time
bool writeWav(const QString& path, const std::vector<float>& samples, int rate = kRate);
// The sound as a file in the cache (CacheLocation/sfx/<kind>-<seed>-<real length ms>ms[-a<asked ms>]-v<kVersion>.wav),
// made on first use; empty = could not write it
QString cachedFile(Kind kind, unsigned seed = 1, double length = 0);
// What a cache file name says (the clips op 'sfx' put on a timeline); nullopt = not one of them
struct CachedName {
    Kind kind;
    unsigned seed;
    double length; // real, seconds
    double asked;  // the length it was made with (0 = the kind's own)
};
std::optional<CachedName> parseCachedFile(const QString& path);

} // namespace SfxSynth
