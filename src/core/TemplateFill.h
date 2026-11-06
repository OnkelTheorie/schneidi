// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Filling a template's placeholders (ClipSlot in core/Types.h): pure functions on a ProjectData, used by `template
// fill` (CLI/MCP), the app's drag from the Media Pool onto a slot (Editor::fillSlot) and "check fields".
//
// A slot instance is a clip with ClipSlot::id set, together with the clips right after it on the same track that carry
// the same id (one slot can become several clips: a frozen tail, loop repeats, a song cut on its phrases). Clips with
// the same id elsewhere are further instances and get the same value. All timelines and compound clips count.
//
// Rules per kind:
// - Clip (a video or still clip): the value's media replaces the clip's, from `from`. Too long = trimmed to the slot.
//   Too short: SlotFit Trim = left shorter (Fixed: a gap stays, warning; Flexible: later clips move up), Speed = slowed
//   down to fill it (at least 25 %, below that also left shorter), Freeze = its last frame holds for the rest, Loop =
//   repeated. A Fixed slot left shorter keeps its own length (ClipSlot::nominalLen): the next value fills all of it
//   again, as far as the gap after it is still free. Flexible slots take the value's length within [minLen, maxLen]; everything after the slot on all
//   unlocked tracks (subtitles, markers, notes, sections too) ripples by the difference; titles and stills that cover
//   exactly the slot (a topic title or a logo over the whole clip) grow with it. Inside a compound clip's timeline the
//   compound clips that show the slot follow (longer/shorter, or moved in their source when they show what comes
//   after it) and their own timeline ripples in turn. Linked audio follows the picture (removed when the new file
//   has no sound); a placeholder without sound of its own (a still, a silent file) gets the new file's sound, linked,
//   on the audio track of its number if free there, else the first free one, else a new one. A picture of another shape than the timeline is reframed
//   (ClipSlot::reframe; Reframe Center, or Auto when Context::reframePath gives a camera path); a clip that was
//   reframed for its old file follows the new shape (Reframe::reshape). Speed ramps, reverse and freeze of the
//   placeholder are reset (they belong to the old file), its own constant speed stays.
// - Text (a text title): the title gets the text (every part of a title cut in two: each keeps its own look). Over maxChars: an error, unless `shrink`: then a warning. With
//   `shrink` a text wider than the placeholder's text was gets a smaller font so it fits that width (font-independent
//   width estimate titleWidth() from core/ProjectFormat.h, the same in app and CLI; at most down to 40 %).
// - Music (an audio clip): the value's song replaces it. With fitMusic and beats from Context::beats it is cut to the
//   slot's length (Flexible: the song's length within [minLen, maxLen]) on its phrases (core/MusicFit); otherwise it plays from `from`, trimmed with a 1 s fade-out (Loop
//   repeats a too short song, other fits leave it shorter). snapCuts is not done here (op `snap_to_beat`).
// - Logo (a still or video clip): the media is replaced, the clip keeps its time and its width on screen (zoom
//   follows the new picture's shape).
//
// Analyses (beats, reframe paths) and probing files are done by the caller (engine) and passed in through Context.

#include "core/Beats.h"
#include "core/ProjectFile.h"
#include "core/Reframe.h"
#include "core/Types.h"

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <optional>

namespace TemplateFill {

// One instance of a slot in a project
struct SlotRef {
    ClipSlot slot;        // as on its first clip
    int sequence = 0;     // sequence id of its timeline (ProjectData::currentSequence for the open one)
    TrackRef track;
    int clip = 0;         // id of the first clip
    QVector<int> parts;   // ids of all its clips (first one first)
    int start = 0;        // timeline frames
    int length = 0;       // of all parts
    int slotLength = 0;   // what a fill fills: `length`, or a Fixed slot's own length (ClipSlot::nominalLen) when a
                          // shorter value left it shorter, as far as its track is free after it
    QString media;        // Clip/Music/Logo: the file it shows now
    QString text;         // Text: the title's text now
};
// Every slot instance of every timeline (the open one first, then the sequences in their order; per timeline video
// tracks V1 up, then audio tracks, by start)
QVector<SlotRef> listSlots(const ProjectData& d);
// The slot ids in that order, each once
QStringList slotIds(const ProjectData& d);

// What fills a slot
struct Value {
    QString file;           // Clip/Music/Logo: media file (absolute path)
    QString text;           // Text
    int from = -1, to = -1; // part of the file in source frames of the project rate, [from, to); -1 = the file's
                            // source in/out from the Media Pool if set, else its start / end
    static Value ofFile(const QString& file, int from = -1, int to = -1) { return {file, {}, from, to}; }
    static Value ofText(const QString& text) { return {{}, text, -1, -1}; }
};
using Values = QMap<QString, Value>; // slot id -> value

struct Context {
    // A file that is not in ProjectData::media yet (it is added there when used); empty or nullopt = unknown file
    std::function<std::optional<MediaInfo>(const QString& path)> probe;
    // Picture shape (width / height as shown) of a file; empty = MediaAspect::of (0 without an engine = unknown)
    std::function<double(const QString& path)> aspect;
    // Beat analysis of a song (whole file) for Music slots with fitMusic; empty or nullopt = trim + fade-out
    std::function<std::optional<Beats::Result>(const QString& path, int stream)> beats;
    // Camera path (Reframe::path, clip frames) for a clip of `file` from source frame `from`, `length` frames;
    // empty function or no keys = Reframe Center
    std::function<QVector<Reframe::Key>(const QString& file, int from, int length)> reframePath;
};

// An error about one slot (or the values as a whole: slot empty). code: UNKNOWN_SLOT, MISSING_VALUE, FILE_MISSING,
// UNREADABLE, WRONG_TYPE (no picture for a Clip/Logo, no sound for Music), BAD_RANGE, EMPTY_TEXT, TOO_LONG (text over
// maxChars without shrink), SLOT_MISMATCH (the slot's clip cannot hold its kind), LOCKED (its track is locked)
struct Problem {
    QString slot;
    QString code;
    QString message; // English
    bool operator==(const Problem&) const = default;
};

// What filling one instance did
struct SlotReport {
    QString slot;
    int sequence = 0;
    QVector<int> clips;   // ids of its clips afterwards
    bool ok = true;
    // Machine-readable: "replaced", "trimmed", "shortened" (left shorter than the slot), "slowed", "frozen", "looped",
    // "rippled", "compound" (compound clips showing it followed), "reframed", "reshaped", "no_sound" (linked audio
    // removed), "sound_added" (the file's sound added to a placeholder without), "shrunk", "fit_music", "faded",
    // "scaled"
    QStringList adjusted;
    QJsonObject details;  // numbers (seconds, percent, px): value_s, slot_s, length_s, speed_pct, ripple_s, size_px …
    QString text;         // short English summary: "trimmed 12.0s → 3.0s; reframed (center)"
    QStringList warnings; // English
    Problem error;        // !ok
};

struct Report {
    QVector<SlotReport> results; // per filled instance (also failed ones)
    QVector<Problem> errors;    // every error (also those of failed instances)
    QStringList unfilled;       // slot ids of the project without a value
    bool ok() const { return errors.isEmpty(); }
    QJsonObject toJson() const; // {ok, results:[{slot, sequence, clips, ok, adjusted, details, text, warnings, error?}], errors, unfilled}
};

// Can `c` on a track of kind `track` be a slot of `kind`? Empty = yes, else why not (English)
QString canHold(const Clip& c, TrackKind track, SlotKind kind);

// Can a slot of `kind` take the file `m`? Empty = yes, else why not (English; a text slot takes no file)
QString canFill(SlotKind kind, const MediaInfo& m);

// One instance checked against `v` (what apply() checks before filling it)
std::optional<Problem> checkSlot(const ProjectData& d, const SlotRef& r, const Value& v, const Context& ctx = {});

// The values checked against the slots without changing anything (files, types, ranges, text lengths)
QVector<Problem> check(const ProjectData& d, const Values& values, const Context& ctx = {});

// Fills the slots with `values` (instances whose value fails the check stay as they were). New files go into
// d.media (Context::probe); new clip/link ids come from d.lastClipId / d.lastLinkId.
Report apply(ProjectData& d, const Values& values, const Context& ctx = {});

} // namespace TemplateFill
