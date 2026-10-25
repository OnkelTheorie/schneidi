#pragma once
// Import presets for files with several audio streams (docs/archiv/plan-0.8.md; OBS records "Mix" plus single streams,
// which plays everything twice): rules per stream title (place / mute = clip disabled / leave out, volume, track name)
// and the switch "mute the Mix stream when single streams exist" (on by default). Applied wherever clips come from
// files (Editor: insert, source edits, drag from the viewer, multicam angles; CLI). Kept in the settings (all
// projects), not in the project.

#include "core/Types.h"

#include <QJsonObject>
#include <QString>
#include <QVector>

struct StreamRule {
    enum class Action { Place, Mute, Skip };
    QString title;            // stream title (case and surrounding spaces don't matter)
    Action action = Action::Place;
    double volumeDb = 0.0;    // clip volume
    QString trackName;        // name for a new audio track (empty = the stream title)
    bool operator==(const StreamRule&) const = default;
};

// How one audio stream of a file is placed (StreamPresets::placements, streams left out are missing)
struct StreamPlacement {
    int stream = 0;
    bool enabled = true;
    double volumeDb = 0.0;
    QString trackLabel; // label for an unnamed audio track (empty = default "Audio N")
};

struct StreamPresets {
    // On by default: a Mix stream next to its parts would play everything twice (and clip)
    bool muteMix = true;
    QVector<StreamRule> rules;
    bool operator==(const StreamPresets&) const = default;

    // No presets: every stream as in the file (CLI presets:false)
    static StreamPresets none() { return {false, {}}; }

    // "Mix", "mix 1", "Mix (alles)", "Mix-Spur", "Mix2" – not "Mixer", "Remix", "Discord Mix"
    static bool isMixTitle(const QString& title);

    // One entry per audio stream of the file (title = the file's), empty without sound. Only files with at least two
    // streams are changed; untitled streams stay as they are; an explicit rule wins over the Mix switch; if every
    // stream would be left out, they are muted instead.
    QVector<StreamRule> plan(const MediaInfo& m) const;
    // The placed streams in file order (what plan() does not leave out)
    QVector<StreamPlacement> placements(const MediaInfo& m) const;

    // Rules from how the clips of one placement (linkId) of file m are set up in the timeline ("remember these track
    // settings"): deleted stream = leave out, disabled = mute, clip volume, renamed track. Titled streams only.
    static QVector<StreamRule> learn(const Timeline& tl, const MediaInfo& m, int linkId);
    // Takes over rules: a rule with the same title (any case) is replaced, the others are kept
    void merge(const QVector<StreamRule>& rules);

    QJsonObject toJson() const;
    static StreamPresets fromJson(const QJsonObject& o);
    // QSettings "import/streamPresets" (JSON; missing or broken = defaults)
    static StreamPresets load();
    void save() const;
};
