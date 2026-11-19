// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The suggestions of the Direct page and the `direct` command (cli/CommandsDirect.cpp): a recipe (short, highlights,
// talk, template, script) looks at the material and suggests moments/pauses/slots/takes, each with a reason; the user
// (or the AI) switches them on and off, moves their edges and orders them, then applies the session as one edit.
// Pure data + JSON (the fixed contract in the 0.25 plan, "Vertrag `direct`"), no Qt beyond containers and JSON ->
// testable, the same in the app, the CLI and the project file (key "direction", the last session).
//
// Reasons are a fixed key with values ({key:"kills", values:{n:2}}), never a score for people: the page shows the
// German key text translated (reasonText(..., Lang::Ui)), the CLI adds an English sentence for the AI (text).

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace Direction {

// ---------- Reasons and notes ----------

struct Reason {
    QString key;        // one of reasonKeys()
    QJsonObject values; // placeholders of its text: n, s, db, extra ("multi"), word, slot, sentence
    bool operator==(const Reason&) const = default;
};

// The fixed reason keys: kills, loud, laughter, applause, cheering, scream, motion, scene_change, face, pause, filler,
// slot_match, take, fallback_end
const QStringList& reasonKeys();
// Note keys (Session::notes): no_whisper, running, no_moments, fallback, music_missing, not_yet, few_moments,
// fewer_variants, no_shot, not_said, no_pause, never_quiet, no_transcript_fillers, fillers_transcript, room_tone
const QStringList& noteKeys();

enum class Lang { English, Ui };
// The reason as a sentence: English (for the AI, the CLI's "text") or in the UI language (the German key through T(),
// numbers with I18n::decimalPoint()). An unknown key gives the key itself.
QString reasonText(const Reason& r, Lang lang = Lang::English);
// The same for a note key with values (an unknown key: empty)
QString noteText(const QString& key, const QJsonObject& values = {}, Lang lang = Lang::English);
struct Note;
// A note of a fixed key: its English text and its values (the page translates it)
Note makeNote(const QString& key, const QJsonObject& values = {});

// ---------- Session ----------

struct Suggestion {
    int n = 0;          // 1-based, stable within the session
    QString kind = "moment"; // moment | pause | filler | slot | take
    QString file;       // absolute
    int stream = 0;     // audio stream the reason was heard in
    double from = 0, to = 0; // seconds of the source (from_s/to_s)
    double score = 0;   // for sorting only (never shown as a number)
    Reason reason;
    QStringList labels; // "kill", "loud", "laughter", "motion" …
    bool on = true;
    int variant = 0;    // short: 1..3 (0 = none, not written)
    QString track;      // pause/filler: the timeline track
    QString slot;       // slot: the template's field id
    int shot = 0;       // slot: index shot number
    int sentence = 0;   // take: the script's sentence (1-based)
    bool operator==(const Suggestion&) const = default;
};

struct Material {
    QString file;
    double length = 0; // seconds
    bool hasVideo = false, hasAudio = false;
    QJsonObject analysis; // {sound, picture, events, speech}: "done" | "running" | "none"
    // Not in the contract's example, used by apply: the file as looked at (stale check), its shape and sound
    qint64 size = -1, modified = 0;
    double fps = 0;
    int width = 0, height = 0;
    QStringList audioStreams; // stream titles as `probe` names them
    int moments = 0;          // moments found in it (before picking)
    bool operator==(const Material&) const = default;
};

struct Note {
    QString key;
    QString text;       // English
    QJsonObject values; // the key's placeholders (noteText), written only when set
    bool operator==(const Note&) const = default;
};

struct Session {
    QString id;      // "d3"
    QString recipe;  // short | highlights | talk | template | script
    QString made;    // ISO time (UTC)
    QJsonObject options;
    QString project; // the project it was made for (may be empty: files only)
    QString timeline; // timeline material ('<name>'), empty = files
    QVector<Material> material;
    QVector<Suggestion> suggestions;
    QVector<int> order; // n of the switched-on suggestions in the result's order
    double target = 0;  // target length (target_s), 0 = none
    QVector<Note> notes;
    bool operator==(const Session&) const = default;

    const Suggestion* find(int n) const;
    // Sum of the switched-on suggestions (moments; pauses count what is taken out)
    double length() const;
};

QJsonObject toJson(const Suggestion& s);
QJsonObject toJson(const Material& m);
// Reasons get their English text added (reason.text) - read back it is ignored
QJsonObject toJson(const Session& s);
// nullopt (+ *error) when it is not a session (no recipe, a suggestion without n/kind)
std::optional<Session> fromJson(const QJsonObject& o, QString* error = nullptr);

// The order of the switched-on suggestions: `order` where it names them, then the others in their n order; switched-off
// and unknown ones left out
QVector<int> resolvedOrder(const Session& s);

} // namespace Direction
