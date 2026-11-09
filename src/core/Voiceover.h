// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Voice-over recording (0.16, 0.20 straight onto a track): speak over the timeline while it plays, the recording
// lands as a clip where the playhead stood: on the armed (or selected) audio track when it is free there, else on a
// new audio track. The sound input itself is Audio::Capture (engine); this part is the pure
// bookkeeping the app and the tests share: where recordings go, how the file lines up with the timeline and where
// the clip goes (Editor::placeRecording places it as one undo step).
//
// Lining up: the recording starts a moment before the timeline sound is heard (the player waits for the picture)
// and arrives in blocks. While recording, the app pairs "frames the input delivered" with "timeline time heard now"
// (the player's clock already counts the output's buffer); the smallest difference is the most exact pair (taken
// right after an input block arrived). A user offset (what the hardware adds unreported) is subtracted on top.

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <climits>
#include <optional>

struct Timeline;
struct Track;

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

// Which audio track a take goes to (0.20 "record where you are"; reworked after a user's 24 takes made 10 new tracks):
// - an armed track is the user's explicit choice: the take goes there and replaces what lies in its range (punch-in);
//   a locked armed track refuses (-1)
// - else the track of the selected clips (selectedTrack) when it is free from `from` to `end` and may take a take
// - else the first track named Voiceover (the ones a take creates): new takes replace earlier ones there
// - else the first free track without a user-given name
// - else one new track (= tl.audio.size(); placing creates it, named Voiceover)
// "May take a take": not locked, not muted, no name or the name Voiceover - a track the user named (Discord, Mix,
// Mikrofon …) only gets a take when it is armed. `end`: the take's end when known (CLI), else open.
int recordTrack(const Timeline& tl, int armed, int selected, int from, int end = INT_MAX);
// The one audio track the selected clips (ids) lie on; clips on several audio tracks or none: -1
int selectedTrack(const Timeline& tl, const QVector<int>& ids);
// A track a take created (named Voiceover, in German or the UI language)
bool isVoiceoverTrack(const Track& t);
// Which audio track a mark (the armed track, a running take's track) means after the timeline changed: tracks have no
// ids, so a track added or removed above it (an undo, an AI's edit) would shift a plain index onto another track.
// A key remembers the index and what every audio track looked like then (name and clip ids); findTrack gives the same
// index while the number of tracks is the same, else lines the tracks then up with the tracks now (longest common
// run; a track matches one with a clip of it, an empty one an empty one of the same name) and gives where the marked
// one is now, -1 = not there any more (removed, or emptied together with tracks added or removed).
// Two empty tracks without a name look alike: one added next to an empty marked track may take the mark - both are
// empty, nothing is lost. A key of a track that is not there yet (index >= the count: one new track) always means one
// new track below the others (the count now) - never one the user added meanwhile.
struct TrackKey {
    int index = -1;
    QStringList names;          // per audio track
    QVector<QVector<int>> clips; // per audio track: the ids of its clips
    bool operator==(const TrackKey&) const = default;
};
TrackKey trackKey(const Timeline& tl, int track);
int findTrack(const Timeline& tl, const TrackKey& key);
// How far before `from` a take on `track` may reach with what the input recorded before the timeline was heard (the
// player waits for the first picture; words said right at the key press lie there): back to the end of the clip
// before it on the track, not before 0; a clip lying over `from` (punch-in) keeps it at `from`
int headLimit(const Timeline& tl, int track, int from);
// Where a take from `from` starts on `track`: a clip there ending within `tolerance` frames of `from` (before or after:
// the playhead stops a frame or two off the previous take's end) -> right at its end, so takes in a row butt against
// each other without overlap or gap; else `from`
int buttStart(const Timeline& tl, int track, int from, int tolerance);

// The take while it is being recorded (the growing clip in the timeline): how far it reaches and its waveform, from
// the blocks the input delivers. File sample 0 lies at timeline frame `fileStart` (the live estimate; the final place
// comes from the Alignment), the clip shows from `from` on (the playhead at the start; the pre-roll is not shown).
class LiveTake {
public:
    static constexpr int kPeaksPerSecond = 100; // waveform resolution (a bucket = 10 ms)
    LiveTake() = default;
    LiveTake(double fileStart, int from, double fps, int rate);
    // The input has delivered `totalFrames` sample frames so far; `peak` (linear 0..1) is the loudest of the frames
    // since the call before (Audio::Capture::takePeak). Frames that went back are ignored.
    void add(long long totalFrames, float peak);
    // A block of mono samples (a stand-in source, tests)
    void addSamples(const float* samples, int count);
    long long samples() const { return m_samples; }
    int rate() const { return m_rate; }
    int from() const { return m_from; }
    double fileStart() const { return m_fileStart; }
    // A better estimate of where file sample 0 lies (the Alignment's pairs while the timeline plays)
    void setFileStart(double frame) { m_fileStart = frame; }
    // Timeline frame (fractional) the take reaches now
    double end() const;
    // Timeline frame the clip shows from: `from`, or the file's start when that lies after it (the take reaches back
    // to `from` at most)
    double start() const { return std::max(double(m_from), m_fileStart); }
    // Frames shown from start() on (0 while the pre-roll runs)
    double lengthFrames() const;
    // Loudest peak (0..1) in timeline frames [a, b); 0 where nothing was recorded
    float peak(double a, double b) const;
    const QVector<float>& peaks() const { return m_peaks; }

private:
    double m_fileStart = 0, m_fps = 25;
    int m_from = 0, m_rate = 48000;
    long long m_samples = 0;
    QVector<float> m_peaks; // per 1/kPeaksPerSecond s of the file
};

// Pairs of (frames the input delivered, timeline seconds heard) while recording (see above)
class Alignment {
public:
    // Pairs only count while the clock moves (heardSeconds larger than at the call before). Once a pair counted, a
    // clock going back (a seek or a loop back, J/reverse play) ends the pairing: later pairs would sit earlier in
    // the file than the sound they belong to and pull the take forward; the take keeps the place from before.
    void add(long long capturedFrames, double heardSeconds, int rate);
    bool empty() const { return !m_best; }
    // Timeline seconds at file sample 0: best pair minus an input delay (samples) and the user offset
    std::optional<double> fileStartSeconds(int inputLatency, int rate, int offsetMs) const;

private:
    std::optional<double> m_best, m_lastHeard;
    bool m_ended = false;
};

} // namespace Voiceover
