#pragma once
// Timeline exchange with other editors (Premiere Pro, DaVinci Resolve, Final Cut 7, Avid …): EDL (CMX3600),
// FCP7 XML (xmeml) and OpenTimelineIO (.otio). The formats are read into and written from one neutral model
// (Interchange::Sequence); fromTimeline/toTimeline convert it to and from a schneidi timeline.
// Own project files of other programs (.prproj, .drp) are not documented and change with every version, so
// only the exchange formats both programs write and read are supported.

#include "core/ProjectFormat.h"
#include "core/Types.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <optional>

class Project;

namespace Interchange {

enum class Format { Edl, Fcp7, Otio };
// By file extension: .edl, .xml, .otio; nothing for others
std::optional<Format> formatOf(const QString& path);
QString extension(Format f); // "edl", "xml", "otio"
QString formatName(Format f); // "EDL (CMX3600)", …

struct Key {
    int frame = 0; // like Clip::in (frames of the material, not of the timeline)
    double value = 0;
    bool operator==(const Key&) const = default;
};

// One clip on a track. Times are frames in the sequence's frame rate.
struct Item {
    QString path; // media file
    QString name; // clip name (file name if empty)
    int start = 0;  // in the timeline
    int length = 0; // in the timeline
    // First frame of the material (as Clip::in: with a speed change frames of the retimed material, like
    // FCP7 XML in/out); the file frame is in * speed (forward)
    int in = 0;
    double speed = 1.0; // > 0
    bool reverse = false;
    bool enabled = true;
    int audioStream = 0;  // audio: which stream of the file
    double volumeDb = 0;  // audio
    double opacity = 100; // video, %
    QVector<Key> volumeKeys, opacityKeys; // dB / %
    int link = 0; // items with the same link (> 0) belong together (picture and its sound)
    int mediaLength = 0; // frames of the whole file (0 = unknown), for the file description of FCP7 XML / OTIO
    bool operator==(const Item&) const = default;
};

// A transition on a track: a dissolve between two neighbours (left/right = item indices) or a fade from/to
// black or silence (one of them -1), over [start, end) in the timeline
struct Transition {
    int left = -1, right = -1;
    int start = 0, end = 0;
    TransitionType type = TransitionType::CrossDissolve; // CrossDissolve or DipToColor (others are written as dissolve)
    QColor color = Qt::black;
    bool operator==(const Transition&) const = default;
};

struct TrackData {
    QString name; // empty = default ("Video 1" …)
    bool enabled = true; // video: visible, audio: not muted
    QVector<Item> items; // sorted by start, no overlap
    QVector<Transition> transitions;
    bool operator==(const TrackData&) const = default;
};

struct Sequence {
    QString name;
    ProjectFormat format;
    bool hasSize = false; // the file gave the picture size (EDL does not)
    QVector<TrackData> video, audio;
    QVector<int> markers;
    QStringList warnings; // what was left out or changed (shown to the user)
};

struct Error {
    QString message;
};

// ---- schneidi <-> neutral model ----

// Timeline of a project sequence (0 = the open one). Titles, compound and multicam clips, effects, grades and
// speed ramps cannot be carried over: they are left out or simplified, with a warning each.
Sequence fromTimeline(const Project& project, int sequenceId = 0);

// The timeline for an imported sequence (already in the project's frame rate, see convertRate). `newId` gives
// clip ids, `newLinkId` link ids. Media must be in the project (its length limits handles of transitions).
Timeline toTimeline(const Sequence& seq, const std::function<int()>& newId, const std::function<int()>& newLinkId);

// All frame values to another frame rate (by time); the format's rate becomes `rate`
void convertRate(Sequence& seq, FrameRate rate);

// Audio items of other programs count channels, not streams (Premiere: a stereo file = two items on A1 and A2 with
// source track 1 and 2). An item asking for a stream the file does not have is dropped (length 0) when another item
// of the same file plays at the same time, else it plays the first stream. `streams` = audio streams of a file
// (0 = unknown: left as is).
void fitAudioStreams(Sequence& seq, const std::function<int(const QString&)>& streams);

// Media files the sequence uses (in order of first use)
QStringList mediaFiles(const Sequence& seq);

// ---- Into a project ----

// A media file of the sequence on this computer: the path as given, or a file of the same name in one of the
// folders (or up to 3 levels below); empty = not found
QString findMedia(const QString& path, const QStringList& folders);

struct Imported {
    int sequenceId = 0; // the new timeline (opened)
    QString name;
    int clips = 0;
    QStringList missing;  // media files that are not in the project's media pool: their clips are left out
    QStringList warnings; // of the file and of adding it
};
// The sequence as a new timeline of the project (one undo step, opened). `local`: the local file for media paths
// of the sequence (missing = path as given); clips of files not in the media pool are left out, clips beyond the end
// of their file shortened, channel items of other programs dropped (fitAudioStreams). The timeline is converted to
// the project's frame rate; the caller may adopt the file's format first (while the project is empty).
// Empty name = the sequence's (made unique).
Imported addToProject(Project& p, Sequence seq, const QHash<QString, QString>& local, const QString& name = {});

// ---- Files ----

QByteArray write(Sequence& seq, Format f); // may add warnings (e.g. EDL: only one video track)
// Throws Error if the file cannot be read. EDL has no frame rate of its own: `edlRate` is used.
Sequence read(const QByteArray& data, Format f, FrameRate edlRate = {25, 1});

// The single formats (InterchangeEdl.cpp, InterchangeFcp7.cpp, InterchangeOtio.cpp)
QByteArray writeEdl(Sequence& seq);
Sequence readEdl(const QByteArray& data, FrameRate rate);
QByteArray writeFcp7(Sequence& seq);
Sequence readFcp7(const QByteArray& data);
QByteArray writeOtio(Sequence& seq);
Sequence readOtio(const QByteArray& data);

// Media paths in other programs' files are URLs or paths of another computer: file:///C:/… or C:\… become a
// local path (stays as given if it cannot be resolved)
QString pathFromUrl(const QString& url);
QString urlFromPath(const QString& path);

} // namespace Interchange
