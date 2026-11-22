// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Subtitles (DaVinci Subtitle Tracks): read/write SRT and helpers for subtitle tracks.
// Pure data functions, editing goes through the Editor (undo).

#include "core/Types.h"

#include <QByteArray>
#include <QPair>
#include <QSize>

#include <functional>

namespace Subtitles {

// SRT or WebVTT file -> entries in timeline frames (fps = project frame rate, e.g. 29.97). ids stay 0.
// WebVTT: header/NOTE/STYLE/REGION blocks are left out, "MM:SS.mmm" times read, &amp; &lt; &gt; &nbsp; unescaped.
// Encoding UTF-8 (with/without BOM), otherwise Windows-1252/Latin-1; formatting (<i>, {\an8}) is removed,
// broken blocks skipped (skipped = count). Sorted, overlaps shortened at the end of the previous one.
QVector<SubtitleCue> parseSrt(const QByteArray& data, double fps, int* skipped = nullptr);
// Entries -> SRT (UTF-8, CRLF as most programs expect). offset is subtracted from all times
// (export of an In/Out range starts at 00:00:00,000); entries before it are dropped or shortened.
QByteArray toSrt(const QVector<SubtitleCue>& cues, double fps, int offset = 0, int until = -1);
QString srtTime(qint64 ms); // "HH:MM:SS,mmm"
// The same as WebVTT (web players, HTML <track>): "WEBVTT" header, "HH:MM:SS.mmm" times, no numbers needed
QByteArray toVtt(const QVector<SubtitleCue>& cues, double fps, int offset = 0, int until = -1);
// SRT or WebVTT by the file's extension (".vtt" = WebVTT)
QByteArray toFile(const QString& path, const QVector<SubtitleCue>& cues, double fps, int offset = 0, int until = -1);

// A subtitle track as plain text with times from `offset` (ms), for a text stream in an exported file
struct TextCue {
    qint64 fromMs = 0, toMs = 0;
    QString text;
};
struct TextTrack {
    QString name; // track name, empty = unnamed
    bool main = false; // the visible track (players show it by default)
    QVector<TextCue> cues;
};
// Every track with entries in [offset, until) in the order of srtFiles (main first); empty lines left out
QVector<TextTrack> textTracks(const Timeline& tl, double fps, int offset = 0, int until = -1);

// The SRT files a subtitle export writes, one per track with entries: the visible track (otherwise the first with
// entries) into `path`, every other one next to it as "<name>.<track name>.srt" (players offer such files as further
// subtitles; an unnamed or clashing track gets its number). Main file first; empty = no track has entries.
struct SrtFile {
    QString path;
    int track = 0; // index in Timeline::subtitles
};
QVector<SrtFile> srtFiles(const Timeline& tl, const QString& path);

// ---- Readable cues (0.19) ----
// Regroups short cues (whisper's one-line pieces, a word list) into cues people can read (the cheapest split of each
// stretch between pauses: no short remainder of a sentence, its pieces about equally long, at punctuation): at most `lines` lines of
// `lineChars` characters (two lines split evenly; at a comma/phrase end when that is at most 16 characters less even), ended at sentence ends once
// slow enough, never across a pause longer than `pause` seconds; each one stays at least `minSeconds` and long
// enough for `cps` characters per second where the next cue leaves room. Word times: a cue with one CueWord per word
// keeps them (the new cue gets them all, when every source cue had them); otherwise words share their cue's time by
// length. Overlapping input is cut apart first (a cue ends where the next starts; two starting together are one);
// cues without text are left out. ids 0 in the result.
// The defaults are the "normal" length; rulesFor(CueLength) gives the short ones (one line, a few words).
struct ReadableRules {
    int lines = 2;
    int lineChars = 42;
    double cps = 17;
    double minSeconds = 0.8;
    double pause = 0.7;
    int maxWords = 0;          // most words per cue (one longer word alone may still be more characters); 0 = any
    int shortChars = 20;       // a piece of a sentence under this many characters ...
    int shortWords = 2;        // ... or of at most this many words costs a lot (an orphan "dazu gehört.")
    double tooShortCost = 0;   // cost of a cue the next word leaves less than minSeconds (0 = none: one more cue is
                               // fine); a stretch of speech that short is shown as it is
    double conjunctionCost = 12; // cost of a break before a conjunction ("und", "weil", "but"); mid-phrase is 12
    double danglingCost = 0;     // extra cost of a cue ending on an article or short word ("die", "zu", "einen", "the")
};
QVector<SubtitleCue> readable(const QVector<SubtitleCue>& cues, double fps, const ReadableRules& rules = {});

// ---- Cue length (0.26): "Untertitel-Länge" ----
// Normal: today's 1-2 line cues (ReadableRules{}). Short: one line of at most 25 characters and 6 words, about one
// short sentence or less (split at sentence ends, then commas and conjunctions, never shown under 0.5 s unless the
// speech is that short) - for TikTok/Shorts. VeryShort: 2-3 words (at most 16 characters) at a time. Auto: Short for a
// vertical picture (taller than wide), Normal otherwise.
enum class CueLength { Auto, Normal, Short, VeryShort };
CueLength resolve(CueLength length, QSize format);
ReadableRules rulesFor(CueLength length); // Auto counts as Normal (resolve first)
QString cueLengthKey(CueLength length);   // "auto", "normal", "short", "very_short" (CLI, settings)
// From cueLengthKey (also "very-short", "veryshort", any case); false = unknown
bool parseCueLength(const QString& key, CueLength* length);

// Default style for a project format (sizes relative to the picture height, basis 1080 pixels)
TitleStyle defaultStyle(QSize format);

// Place an entry on the track; overwrites what is there (like clips: fully covered ones are dropped,
// partly covered ones are shortened or split). Track stays sorted.
void place(SubtitleTrack& track, const SubtitleCue& cue);
// Move the entries `ids` by `delta` frames and `trackDelta` tracks (the target tracks must exist; locks and frame 0
// are the caller's business), placed like `place`; split remains get newId(). Other ids are ignored.
void move(Timeline& tl, const QVector<int>& ids, int delta, int trackDelta, const std::function<int()>& newId);
// Find an entry by id (trackIndex = track), nullptr = none
const SubtitleCue* find(const Timeline& tl, int id, int* trackIndex = nullptr);
SubtitleCue* find(Timeline& tl, int id, int* trackIndex = nullptr);
// Index of the entry under the frame, -1 = none
int cueAt(const SubtitleTrack& track, int frame);
// Last frame (exclusive) of all subtitles
int endFrame(const Timeline& tl);

// ---- Ripple/split (subtitle tracks follow the clip tracks as in DaVinci) ----
// Splits the entry that lies across `frame` (start < frame < end): left keeps the id, right gets newId(),
// both the text. Returns: id of the right part, 0 = none lay there.
int splitAt(SubtitleTrack& track, int frame, const std::function<int()>& newId);
// Like TimelineOps::rippleTracks for a subtitle track. Offset (frame, d): d > 0 shifts every entry with
// start >= frame by d; d < 0 removes the time [frame + d, frame) - entries in it are shortened or deleted,
// everything after it moves up (subtitles belong to the picture, so they never block). Returns: true = changed.
bool ripple(SubtitleTrack& track, const QVector<QPair<int, int>>& shifts);
// Insert: split the entry across `frame`, everything from `frame` on by `length` to the right
void insertGap(SubtitleTrack& track, int frame, int length, const std::function<int()>& newId);

} // namespace Subtitles
