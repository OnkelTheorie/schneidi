#pragma once
// Subtitles (DaVinci Subtitle Tracks): read/write SRT and helpers for subtitle tracks.
// Pure data functions, editing goes through the Editor (undo).

#include "core/Types.h"

#include <QByteArray>
#include <QPair>
#include <QSize>

#include <functional>

namespace Subtitles {

// SRT file -> entries in timeline frames (fps = project frame rate, e.g. 29.97). ids stay 0.
// Encoding UTF-8 (with/without BOM), otherwise Windows-1252/Latin-1; formatting (<i>, {\an8}) is removed,
// broken blocks skipped (skipped = count). Sorted, overlaps shortened at the end of the previous one.
QVector<SubtitleCue> parseSrt(const QByteArray& data, double fps, int* skipped = nullptr);
// Entries -> SRT (UTF-8, CRLF as most programs expect). offset is subtracted from all times
// (export of an In/Out range starts at 00:00:00,000); entries before it are dropped or shortened.
QByteArray toSrt(const QVector<SubtitleCue>& cues, double fps, int offset = 0, int until = -1);
QString srtTime(qint64 ms); // "HH:MM:SS,mmm"

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
