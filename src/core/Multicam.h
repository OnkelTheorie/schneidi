#pragma once
// Multicam like DaVinci (docs/archiv/plan-0.8.md): a multicam clip is a sequence (Sequence::multicam) with one angle
// per recording, each on its own tracks in sync. In a timeline its segments (ClipKind::Multicam) show one angle each;
// cutting/switching (Editor::multicamCut ...) writes normal edits. Here: sync placement of the recordings and
// lookups shared by editor, engine and viewer. Edits are in core/EditorMulticam.cpp.

#include "core/AudioSync.h"
#include "core/Types.h"

#include <QSet>
#include <QStringList>

#include <optional>

class Project;

namespace Multicam {

// Which part a cut/switch changes (DaVinci: Video + Audio, Video only, Audio only)
enum class Mode { Both, Video, Audio };
// How the recordings are lined up: by sound, by their first source marker, by their source In point, by their start
enum class SyncMethod { Sound, Marker, InPoint, Start };

// Recordings of one angle as chosen by the user (several files = recording split after a restart)
struct Source {
    QString name; // empty = file name
    QStringList files;
};

// A file of an angle with its position: `offset` frames after the reference (first file of the first angle)
struct FilePlacement {
    QString path;
    int offset = 0;
    bool synced = true;                 // false = placed without sync info (at the start / after the previous file)
    SyncMethod via = SyncMethod::Start; // how it was placed
    double confidence = 0;              // sound only (AudioSync::Match::confidence)
};

struct Angle {
    QString name; // empty = file name of the first file
    QVector<FilePlacement> files;
};

// Placement of the sources by `method`. sound = AudioSync matches per file (all files of all sources in order, e.g.
// from SoundSync::align); a file the sound did not find falls back to the first source markers (if `fallback` and
// the reference and the file have one), otherwise it goes to the start of its angle (or right after the previous
// file of the angle) and counts as not synced. Offsets are whole frames (picture and sound of an angle move together).
QVector<Angle> resolve(const Project& project, const QVector<Source>& sources, SyncMethod method,
                       const QVector<AudioSync::Match>& sound = {}, bool fallback = true);

// Default name of an angle: file name without extension
QString defaultAngleName(const QString& path);

// Source position of an angle at a frame of the multicam sequence (picture: its video track, sound: its first audio
// track); nullopt = no such angle/part, or the recording does not cover the frame (gap, before/after it)
struct SourcePos {
    QString path;
    int frame = 0;  // frame in the file
    int clipId = 0; // clip in the multicam sequence
};
std::optional<SourcePos> sourceAt(const Sequence& sequence, int angle, int frame, TrackKind kind = TrackKind::Video);

// Frame of the multicam sequence that segment `segment` shows at timeline frame `frame`
int sequenceFrame(const Clip& segment, int frame);

// The multicam clip the viewer and the cut keys act on at `frame`: a selected one under the playhead, otherwise
// the top-most enabled video segment there, otherwise an audio segment; nullptr = none
const Clip* clipAt(const Timeline& timeline, int frame, const QSet<int>& selected = {});

// Does angle `angle` of the sequence have picture (Video) or sound (Audio)?
bool angleHas(const Sequence& sequence, int angle, TrackKind kind);

// What one part of an angle shows: the multicam sequence's content reduced to the angle's picture track (Video) or
// its sound tracks (Audio); the other kind and the subtitles are left out. No tracks = the angle has no such part.
Timeline angleTimeline(const Timeline& content, const MulticamAngle& angle, TrackKind kind);

} // namespace Multicam
