// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// What can be heard in a recording, as events with times and a confidence: speech, music, silence, laughter,
// applause, cheering, shots (also bangs, explosions, hits) and screams. For the material index (`index` events) and
// `peaks {events}`.
//
// Two ways, the same event kinds:
// - Heuristics, always there, no model: loudness, spectral flux, flatness, periodicity (pitch) and the rhythm of the
//   loudness, measured in 10 ms frames of the sound at 16 kHz mono. Good at silence, shots/bangs, music vs speech,
//   applause; laughter, cheering and screams are educated guesses (see the notes at each rule in AudioEvents.cpp).
// - YAMNet (521 AudioSet classes) through ONNX Runtime (engine/Inference), when the "audio-events" extension and the
//   runtime are installed: the classes are mapped to the same kinds; silence and the exact time of a shot still come
//   from the heuristics (YAMNet's grid is 0.48 s).
//
// Results are cached per file (CacheLocation/audio-events, keyed by path, size, modification time, stream, the way
// and the model), in memory and on disk. Thread-safe; blocks (run it off the GUI thread).

#include <QJsonArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <vector>

namespace AudioEvents {

// Kind ids (fixed, also in the index and the CLI)
inline const QStringList& kinds()
{
    static const QStringList k{"speech", "music", "silence", "laughter", "applause", "cheering", "shot", "scream"};
    return k;
}

struct Event {
    QString kind;     // one of kinds()
    double from = 0;  // seconds in the file's own time (0 = first picture frame, as the timeline counts)
    double to = 0;
    double conf = 0;  // 0..1
    QString by = "heuristic"; // "heuristic" | "yamnet"
};

enum class Engine {
    Auto,      // YAMNet when installed, else the heuristics
    Heuristic, // only the heuristics (fast, no model)
    Yamnet,    // YAMNet required: no events and an error when it is missing
};

struct Options {
    Engine engine = Engine::Auto;
    QStringList only;            // only these kinds (empty = all)
    double minConf = 0;          // drop events below this confidence
    bool useCache = true;        // read/write the cache (false: always measure, nothing stored)
    std::function<bool(double)> progress; // 0..1; false cancels (then no events, nothing cached)
};

struct Result {
    QVector<Event> events; // in time order
    QString by;            // the way that was used: "heuristic" | "yamnet"
    QString error;         // why there are no events (no sound, YAMNet missing, canceled); empty = ok
    bool cached = false;   // came from the cache
    bool incomplete = false; // reading the file failed (a network share gone, the file lost while reading): the
                             // events of what was read, not cached (the next call measures again)
    double duration = 0;   // seconds of sound analysed
};

// The events of audio stream `stream` (0 = first) of a media file
Result analyzeFile(const QString& path, int stream = 0, const Options& options = {});
// The same, only the events (the interface the material index calls)
QVector<Event> analyze(const QString& path, int stream = 0, const Options& options = {});

// The heuristics on samples (mono, any rate; resampled to 16 kHz inside is NOT done: rate must be 16000). Pure, for
// tests and for callers that have the sound already.
constexpr int kRate = 16000;
QVector<Event> heuristics(const std::vector<float>& mono16k, const std::function<bool(double)>& progress = {});

// YAMNet is installed and the runtime loads; why not
bool yamnetAvailable(QString* why = nullptr);
// YAMNet on samples (16 kHz mono). error: why there are no events. Mapped kinds only (shot times as YAMNet's grid).
QVector<Event> yamnet(const std::vector<float>& mono16k, QString* error = nullptr,
                      const std::function<bool(double)>& progress = {});
// A YAMNet-like model and class list given directly (tests: a tiny stand-in model)
QVector<Event> yamnetWith(const QString& model, const QString& labels, const std::vector<float>& mono16k,
                          QString* error = nullptr, const std::function<bool(double)>& progress = {});

// How events are made right now (remembered per process and model file), as a cache key part: "heuristic", or YAMNet with its model file (path, size,
// modification time). A cache of results that contain events (the material index) keys on it too, so installing or
// updating the model measures anew.
QString way(Engine engine = Engine::Auto);
// The same for a given model file (empty = the heuristics); pure, for tests
QString wayKey(const QString& model);

// JSON for the CLI and the index: [{kind, from_s, to_s, conf, by}] (times to the millisecond, conf to 0.01)
QJsonArray toJson(const QVector<Event>& events);

// For tests: forget the memory cache; how often a file was really measured (not taken from a cache)
void clearMemoryCache();
int measuredCount();

} // namespace AudioEvents
