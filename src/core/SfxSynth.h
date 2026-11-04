// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Effect sounds of schneidi's own (op 'sfx', the Effects Library), computed, no recordings: C++ ports of earshot's
// sound recipes (Leon Lin, MIT; core/SfxWhoosh: whoosh, swish; core/SfxForge: riser, hit, impact, click) with their
// bank's parameter sets. Each kind has variants by seed (1-3: picked takes); the same kind, seed and length give the
// same samples. Levels are mild on purpose (a sound under the game sound, never on top of it): the loudest sample of
// every sound lies at its kind's peak level, at most -1 dBFS.

#include <QString>
#include <QStringList>

#include <QVector>

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
// Where the moment lies in a sound of this kind and length without making it (seconds from its start; a recording's
// from the table)
double anchorOf(Kind kind, unsigned seed, double length = 0);

double peakDb(const std::vector<float>& samples); // -inf for silence
// 16-bit PCM WAV, mono, kRate; samples quantized (with a fixed dither) the same way every time
bool writeWav(const QString& path, const std::vector<float>& samples, int rate = kRate);
// The sound as a file in the cache, made on first use: a recording (no length asked) copied to
// CacheLocation/sfx/cc0/<name>, else computed into CacheLocation/sfx/<kind>-<seed>-<real ms>ms[-a<asked ms>]-v<kVersion>.wav;
// empty = could not write it
QString cachedFile(Kind kind, unsigned seed = 1, double length = 0);
// ---------- Recorded sounds (shipped CC0 recordings, assets/sfx/cc0) ----------
// Whoosh, swish, hit, impact and click are recordings by default (they sound real; the computed ones are the fallback
// when the data is missing); the riser stays computed. The files are kept as they are: anchor, peak and length come
// from a table measured once (sounds.json), op 'sfx' levels them at placement (levelGainDb).
struct Recorded {
    QString file;   // relative to the data folder ("swish/oga-swish-1.wav")
    Kind kind = Kind::Swish;
    double anchor = 0, peakDb = 0, length = 0; // seconds, dBFS (decoded at 48 kHz), seconds
    QString author, licence, licenceFile;
};
// The data folder: SCHNEIDI_SFX_DATA (tests), else the program's resources (":/sfx/cc0")
QString recordedRoot();
QVector<Recorded> recorded(Kind k);              // in the table's order; empty = none (computed)
std::optional<Recorded> recordedFor(Kind k, unsigned seed); // seed 1, 2 … pick the file in turn
std::optional<Recorded> recordedOfFile(const QString& path); // a placed copy (CacheLocation/sfx/cc0/<name>)
// Where a sound lands and how long it is without making it (recorded: from the table)
double soundLength(Kind kind, unsigned seed, double length = 0);
// Clip volume that brings a sound to its kind's peak level (recorded files keep their own peak; computed: 0)
double levelGainDb(Kind kind, unsigned seed, double length = 0);

// What a cache file name says (the clips op 'sfx' put on a timeline); nullopt = not one of them
struct CachedName {
    Kind kind;
    unsigned seed;
    double length; // real, seconds
    double asked;  // the length it was made with (0 = the kind's own)
    int version;   // kVersion when it was made
};
std::optional<CachedName> parseCachedFile(const QString& path);
// A built-in sound of a project that is missing (cache cleared, project from another computer): made again in this
// computer's cache when its name is one of this version's; the path it has now (empty = not one, or another version)
QString restoreCachedFile(const QString& missingPath);

} // namespace SfxSynth
