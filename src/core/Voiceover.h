// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Voice-over recording (0.16): speak over the timeline while it plays, the recording lands as a clip on the armed
// audio track where the playhead stood. The sound input itself is Audio::Capture (engine); this part is the pure
// bookkeeping the app and the tests share: where recordings go, how the file lines up with the timeline and where
// the clip goes (Editor::placeRecording places it as one undo step).
//
// Lining up: the recording starts a moment before the timeline sound is heard (the player waits for the picture)
// and arrives in blocks. While recording, the app pairs "frames the input delivered" with "timeline time heard now"
// (the player's clock already counts the output's buffer); the smallest difference is the most exact pair (taken
// right after an input block arrived). A user offset (what the hardware adds unreported) is subtracted on top.

#include <QDateTime>
#include <QString>

#include <optional>

namespace Voiceover {

// Settings (QSettings, per user, not in the project)
struct Settings {
    QString input;          // Audio::Capture input id, "" = the system default input
    int offsetMs = 0;       // extra latency to take off (positive = the voice was recorded later than heard)
    int prerollSeconds = 0; // timeline plays from that much before the playhead first (0 = off)
    static Settings load();
    void save() const;
    bool operator==(const Settings&) const = default;
};

// Folder for recordings of a project: "<project folder>/<project name>-voiceover" next to a saved project, else
// "voiceover" in the app's data folder (an unsaved project has no folder; the file must outlive any cache)
QString folder(const QString& projectPath);
// Free file name in `dir` for a recording made at `when`: "voiceover-yyyyMMdd-HHmmss.wav", "-2", "-3" … when taken
QString uniquePath(const QString& dir, const QDateTime& when);

// Where a recorded file goes in the timeline
struct Placement {
    int start = 0;    // timeline frame of the clip
    int subframe = 0; // 1/100 frame after start (Clip::subframe)
    int in = 0;       // first file frame used
    bool operator==(const Placement&) const = default;
};
// File frame 0 lies at timeline frame `fileStart` (fractional, may be before 0 or before `from`); the clip starts at
// the first file frame at or after timeline frame `from` (the pre-roll is cut off) so that the file sits exactly
// (to 1/100 frame) where it was heard. nullopt = nothing of a `fileFrames` long file lies at or after `from`.
std::optional<Placement> place(double fileStart, int from, int fileFrames);

// Pairs of (frames the input delivered, timeline seconds heard) while recording (see above)
class Alignment {
public:
    // Pairs only count while the clock moves (heardSeconds larger than at the call before)
    void add(long long capturedFrames, double heardSeconds, int rate);
    bool empty() const { return !m_best; }
    // Timeline seconds at file sample 0: best pair minus an input delay (samples) and the user offset
    std::optional<double> fileStartSeconds(int inputLatency, int rate, int offsetMs) const;

private:
    std::optional<double> m_best, m_lastHeard;
};

} // namespace Voiceover
