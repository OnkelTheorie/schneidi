// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Material index: per shot of a recording one line of knowledge - time, quality of the picture (sharp or blurry, too
// dark or bright, how much it moves, a camera pan, a face likely), hearable events (engine/AudioEvents), the words
// spoken in it (from the media transcripts of a project) and a description with tags that a person or the calling AI
// wrote. The measured part comes from one pass over the picture (engine/video/Scan, time ranges decoded in parallel
// like `scenes`) and is cached per file in CacheLocation/index (key: path, size, modification time, kVersion).
// What is written (descriptions, tags) is not cache: it lives in AppDataLocation/index and survives clearing the
// cache; it is matched to the shots by time, so a re-measured file keeps it. Thread-safe, no GUI dependency.
//
// Measures, all on a 320 px wide copy of every picture:
//  - shots: the scene score of `scenes` (ffmpeg's select score, threshold 0.3, at least 0.5 s apart)
//  - sharpness: variance of the Laplacian relative to the picture's own contrast (a blurred picture loses its edges
//    but keeps its contrast), 0..1; brightness: mean luma 0..1
//  - motion: mean change of brightness per pixel between neighbouring pictures (0 = still, 5+ = a lot)
//  - pan: most pictures of the shot match their predecessor best when shifted as a whole
//  - face: share of skin-coloured pixels (YCbCr box), "likely" / "maybe" / "no" - a hint, not a detector

#include "core/IndexSearch.h"
#include "core/Types.h"
#include "engine/AudioEvents.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <optional>

namespace MediaIndex {

inline constexpr int kVersion = 1; // bump when a measure changes (old cache entries are measured anew)

struct Quality {
    double sharp = 0;      // 0..1 (blurry below 0.3)
    double bright = 0;     // mean luma 0..1
    double motion = 0;     // mean change per pixel and picture, 0..255
    double skin = 0;       // share of skin-coloured pixels 0..1
    bool dark = false;     // bright < 0.2
    bool overexposed = false; // bright > 0.85
    bool blurry = false;
    bool pan = false;
    QString face;          // "likely", "maybe", "no"
};

struct Shot {
    int n = 0;              // 1-based
    double from = 0, to = 0; // seconds in the file, [from, to)
    double thumb = 0;       // seconds of a picture that shows the shot (its middle)
    Quality quality;
    QVector<AudioEvents::Event> events; // inside the shot
    QString speech;          // words spoken in it (addSpeech), may be empty
    QString desc;            // written (annotate)
    QStringList tags;
};

struct Entry {
    QString file;            // absolute path
    double duration = 0;     // seconds
    bool video = false;
    QVector<Shot> shots;
    bool cached = false;     // came from the cache without measuring
};

struct Options {
    bool refresh = false;                 // measure again even when cached
    std::function<bool(double)> progress; // 0..1; false = cancel
};

// Cached or measured now (blocking). nullopt with *error when the file cannot be read (or cancelled). Notes
// (annotations) are attached already; speech is not (it comes from a project: addSpeech).
std::optional<Entry> build(const QString& file, const Options& options = {}, QString* error = nullptr);
// Only what is cached (no measuring); notes attached
std::optional<Entry> cached(const QString& file);

// The words of the transcript (ms of the source file) into the shots they start in
void addSpeech(Entry& e, const QVector<TranscriptWord>& words);

// ---------- Written by people / the AI (AppDataLocation/index) ----------

struct Note {
    double from = 0, to = 0; // the shot it was written for (seconds)
    int shot = 0;            // its number then
    QString desc;
    QStringList tags;
};
QVector<Note> notes(const QString& file);
// The note for shot `shot` of e (replaces an earlier one of that shot; desc or tags nullopt = keep what it has).
// Updates e. false + *error when it cannot be saved or the shot does not exist.
bool annotate(Entry& e, int shot, const std::optional<QString>& desc, const std::optional<QStringList>& tags,
              QString* error = nullptr);

// The shots for core/IndexSearch
QVector<IndexSearch::Shot> searchable(const Entry& e);

QString cacheDir();
QString notesDir();
// For tests: forget what is cached in memory
void clearMemoryCache();
// For tests (until engine/AudioEvents measures for real): where the events of a file come from (empty = AudioEvents)
void setEventSource(std::function<QVector<AudioEvents::Event>(const QString& file)> source);

} // namespace MediaIndex
