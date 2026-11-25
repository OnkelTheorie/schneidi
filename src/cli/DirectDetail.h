// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Shared parts of the `direct` command (CommandsDirect.cpp) and its moment collector (DirectMoments.cpp); `short`
// (CommandsShort.cpp) runs the recipe short through them.

#include "cli/CommandsDetail.h"
#include "core/Direction.h"

#include <QJsonArray>
#include <QImage>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <array>
#include <optional>
#include <vector>

namespace Cli::detail {

// One moment of one recording, before picking
struct DirectMoment {
    int file = 0;      // index into the material
    double from = 0, to = 0; // seconds
    double score = 0;
    Direction::Reason reason;
    QStringList labels;
    QString source;    // "highlights" (kill feed / loud), "events" (AudioEvents), "picture" (pictureScores)
    double first = -1; // highlights with kills: the first hit (seconds), before it only lead-in; -1 = unknown
    // How far a short may grow it (seconds; -1 = up to the recording's edge): from shortly before its first hit,
    // up to a little after its last one or the player's death (game footage)
    double lo = -1, hi = -1;
    QVector<double> hits; // the hits inside (kills' hit_s)
};

// The player's deaths in samples of the middle of a CS2 picture (mean R, G, B each, `rate` per second): the red
// tint over everything that fades into a black screen -> [{s: the tint begins, end_s: where a moment ends}]
QJsonArray deathScreensOf(const std::vector<std::array<double, 3>>& rgb, int rate);

// Where the game picture ends at the end of a recording (the recorder stopped from the task bar: its last seconds
// show the desktop): pictures `rate` per second from `firstS` on; the end is a hard change of the whole picture after
// which it stays (nearly) still for at least 1 s with at most 1 s after that (the recorder's stop may move it once
// more), or still up to the last picture (at least 0.2 s) - a desktop, a window, a menu. Changes from one still
// picture to another (a second window) belong to the tail too. -1 = the game goes on to the end
double gameEndOf(const QVector<QImage>& pictures, double firstS, int rate);

// The game of recordings that do not name it (a clip tool's "Desktop_clip_…"): the first of up to `upTo` files whose
// pictures show a known game's kill feed lines staying (KillFeedLines::recognise), confirmed by the player's own kills
// in it (CS2: their red frame, as `highlights` counts them). Per file in AnalysisCache. Empty = none recognised
QString gameInPicture(const QStringList& files, Context& ctx, int upTo = 3);

// The moments of one file (args: file, project?, game: id or "none", no_index?): its material line, the highlights'
// candidates (best first, as `highlights` gives them) and, for material that is no game, the moments of sound events
// (and of picture movement when nothing else stood out). A source that fails counts as none (not cached). Every
// source through its cache, the whole answer in AnalysisCache ("direct-moments"); the material index's face/sharpness
// added afterwards when it is cached (MediaIndex::cached, never built here). Throws CANCELED when canceled.
QJsonObject directMomentsFile(const QJsonObject& a, Context& ctx);

struct CollectedMoments {
    QVector<Direction::Material> material;
    QVector<DirectMoment> highlights; // per file in highlights' order, files in order
    QVector<DirectMoment> extra;      // events + picture, best first
};
// directMomentsFile for every file (in parallel, perFile); project may be empty; game: id, "none" or empty (none)
// index: the material index's bonus (not for recipe short)
CollectedMoments collectMoments(const QStringList& files, const QString& project, const QString& game, bool index,
                                Context& ctx);

// The session of a suggest made in memory (not stored): for `short`
// (found: what the collector found, before picking)
Direction::Session directSuggest(const QJsonObject& a, Context& ctx, CollectedMoments* found = nullptr);
// Applies a session as one edit (apply = {on, adjust, order, variant}); args: project?, plan?, dry_run?, and for
// `short` the private _created (the project was made just before: its empty first timeline becomes the cut)
QJsonObject directApply(const Direction::Session& s, const QJsonObject& apply, const QJsonObject& args, Context& ctx);

// The recipes on what other commands find (DirectRecipesTalk.cpp): talk (cut_pauses), template (templates auto),
// script (rough_cut). suggest* fill the session's material/suggestions/order/notes; apply* run the one edit
void suggestTalk(Direction::Session& s, const QJsonObject& a, Context& ctx);
void suggestTemplate(Direction::Session& s, const QStringList& files, Context& ctx);
void suggestScript(Direction::Session& s, const QStringList& files, Context& ctx);
QJsonObject applyTalk(const Direction::Session& s, const QVector<const Direction::Suggestion*>& list,
                      const QJsonObject& args, Context& ctx);
QJsonObject applyTemplate(const Direction::Session& s, const QVector<const Direction::Suggestion*>& list,
                          const QJsonObject& args, Context& ctx);

// The timeline as it is now, as a fingerprint (its clips): ranges and words measured on it fit only that state
QString timelineStamp(const QString& project, const QString& timeline);

// The words of one audio stream of a file, transcribed alone (whisper through `transcribe`), cached per file, stream,
// language, verbatim and word list (AnalysisCache "speech-words"): {stream, language, words:[[from_ms, to_ms, text]…]};
// no speech = no words. nullopt = the file or stream cannot be read (not cached). vocabulary: words for whisper on top
// of the settings' list (core/Vocabulary, e.g. a game's terms)
std::optional<QJsonObject> streamTranscript(const QString& file, int stream, const QString& language, bool verbatim,
                                            Context& ctx, const QStringList& vocabulary = {});
// A stream of the player's microphone or of a voice chat (not the mix, not music), by its title
bool voiceStream(const QString& title);

// What `short` does after its cut, on its vertical timeline: word captions (style karaoke|word|off; whisper when
// installed - never installed from here) and effect sounds (sfx 'mild': whooshes on the cuts, hits on the kills of
// `game`). shortFinishOps measures everything (speech, the sound's peak, the kill feed) and answers the ops, so the app
// gets only numbers; shortFinishEdit runs them as ONE edit (true = captions were put on: the render burns them in)
struct ShortFinish {
    QString captions = "karaoke";
    QString sfx = "off";
    QString game;     // empty = none
    QString language; // empty = detect
};
// Word captions of a timeline's voice tracks (audible tracks of microphone or voice chat streams), each (file, stream)
// transcribed on its own (streamTranscript) and mapped onto the timeline, each phrase moved onto the sound of its
// track (Captions::snapToSound), merged in time order, every word at its own time (Captions::mergeSpeakers):
// [{from, to, text, speaker}] in timeline frames for op 'captions' (speaker n = voices[n]). *voices: the tracks'
// names; empty (and no words) when the timeline has no voice track - then the timeline's sound is the source.
// game ("cs2", …): its terms go to whisper with the word list (Vocabulary::gameTerms)
QJsonArray voiceCaptionWords(const QString& project, const QString& timeline, const QString& language, Context& ctx,
                             QStringList* voices, const QString& game = {});
// noteKeys (optional): the notes' fixed keys (Direction::noteText), in the order of notes, for a page to translate
QJsonArray shortFinishOps(const QString& project, const QString& timeline, const ShortFinish& f, Context& ctx,
                          QJsonArray* done, QJsonArray* notes, QJsonArray* noteKeys = nullptr);
bool shortFinishEdit(const QString& project, const QString& timeline, const QJsonArray& ops, Context& ctx,
                     QJsonArray* done);
bool shortFinish(const QString& project, const QString& timeline, const ShortFinish& f, Context& ctx, QJsonArray* done,
                 QJsonArray* notes);

} // namespace Cli::detail
