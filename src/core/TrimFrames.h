#pragma once
// Trim view like DaVinci Resolve: which frames the viewer shows while an edit is being trimmed.
// Ripple/Roll/edge trim: two-up (left = Out frame of the outgoing clip, right = In frame of the incoming clip).
// Slip: four-up (large = In/Out of the slipped clip, small above = neighbours' Out/In).
// Slide: four-up (large = left neighbour's Out / right neighbour's In, small above = In/Out of the slid clip).
// Pure model logic (no MLT, no widgets) so it can be tested; the viewer fetches the frames.

#include "core/TimelineOps.h"
#include "core/Types.h"

#include <QString>
#include <QVector>
#include <functional>

namespace TrimFrames {

struct Pane {
    int clipId = 0;        // 0 = no clip on this side (black pane)
    QString path;          // media file to read; empty for titles/compound clips (pane shows the name only)
    int fileFrame = 0;     // frame in the file (project frame rate, speed/reverse/freeze applied)
    int sourceFrame = 0;   // material frame (Clip::in/out space), shown as source timecode
    int timelineFrame = 0; // where this frame sits on the timeline
    bool out = false;      // true = last frame of the clip (Out), false = first frame (In)
    QString name;          // clip name for the label
    bool operator==(const Pane&) const = default;
};

struct View {
    TimelineOps::TrimKind kind = TimelineOps::TrimKind::Ripple;
    QVector<Pane> main;  // two large panes (left, right); empty = no trim view
    QVector<Pane> small; // slip/slide only: two small panes above (left, right)
    int delta = 0;       // current trim amount (frames) for the readout
    bool isNull() const { return main.isEmpty(); }
    bool operator==(const View&) const = default;
};

// Media lookup (Project::mediaInfo); nullptr = unknown file
using MediaLookup = std::function<const MediaInfo*(const QString& path)>;
// Clip name for the label (Project::clipName etc.); empty function = Clip::displayName()
using ClipName = std::function<QString(const Clip& c)>;

// Pane for the first (In) or last (Out) frame of clip c
Pane pane(const Clip& c, bool out, const MediaLookup& media, const ClipName& name = {});

// Frames for an edit while dragging. `preview` = timeline with the trim already applied (Editor::previewTrimEdit,
// or the edge trim of the selection tool). Only video clips are shown (audio-only edits give a null view).
// For an edge trim with the selection tool pass kind Ripple with the dragged clips and their edge.
View compute(const Timeline& preview, const TimelineOps::TrimEdit& edit, int delta, const MediaLookup& media,
             const ClipName& name = {});

} // namespace TrimFrames
