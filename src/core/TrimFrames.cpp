#include "core/TrimFrames.h"

#include "core/Retime.h"

#include <algorithm>

namespace TrimFrames {

namespace {

using TimelineOps::Edge;
using TimelineOps::TrimKind;

struct Located {
    const Clip* clip = nullptr;
    int track = -1; // video track index
};

// First video clip of `ids` in the timeline; prefers one on `preferTrack` (Roll: same track as the left side)
Located firstVideo(const Timeline& tl, const QVector<int>& ids, int preferTrack = -1)
{
    Located found;
    for (int id : ids) {
        TrackRef where;
        const Clip* c = TimelineOps::findClip(tl, id, &where);
        if (!c || where.kind != TrackKind::Video) continue;
        if (where.index == preferTrack) return {c, where.index};
        if (!found.clip) found = {c, where.index};
    }
    return found;
}

// Nearest clip before/after c on its track (tracks are sorted by start and never overlap)
const Clip* neighbour(const Timeline& tl, int track, const Clip& c, bool after)
{
    if (track < 0 || track >= tl.video.size()) return nullptr;
    const QVector<Clip>& clips = tl.video[track].clips;
    const Clip* best = nullptr;
    for (const Clip& o : clips) {
        if (o.id == c.id) continue;
        if (after && o.start >= c.end()) return &o;
        if (!after && o.end() <= c.start) best = &o;
    }
    return best;
}

Pane emptyPane(bool out)
{
    Pane p;
    p.out = out;
    return p;
}

Pane paneOf(const Clip* c, bool out, const MediaLookup& media, const ClipName& name)
{
    return c ? pane(*c, out, media, name) : emptyPane(out);
}

} // namespace

Pane pane(const Clip& c, bool out, const MediaLookup& media, const ClipName& name)
{
    Pane p;
    p.clipId = c.id;
    p.out = out;
    p.name = name ? name(c) : c.displayName();
    p.sourceFrame = out ? c.out : c.in;
    p.timelineFrame = out ? c.end() - 1 : c.start;
    if (c.isTitle() || c.isCompound() || c.mediaPath.isEmpty()) return p; // nothing to decode, pane shows the name
    p.path = c.mediaPath;
    const MediaInfo* m = media ? media(c.mediaPath) : nullptr;
    if (m && m->isImage) {
        p.fileFrame = 0;
        return p;
    }
    const int material = c.freeze ? c.stillFrame() : p.sourceFrame;
    const int length = m ? m->length : 0;
    double file = material;
    if (c.isRetimed() && length > 0) file = RetimeMap(c, length).fileFrameAt(material);
    p.fileFrame = std::max(0, int(file));
    if (length > 0) p.fileFrame = std::min(p.fileFrame, length - 1);
    return p;
}

View compute(const Timeline& preview, const TimelineOps::TrimEdit& edit, int delta, const MediaLookup& media,
             const ClipName& name)
{
    View v;
    v.kind = edit.kind;
    v.delta = delta;
    if (edit.kind == TrimKind::Roll) {
        const Located left = firstVideo(preview, edit.ids);
        const Located right = firstVideo(preview, edit.rightIds, left.track);
        if (!left.clip && !right.clip) return v;
        v.main = {paneOf(left.clip, true, media, name), paneOf(right.clip, false, media, name)};
        return v;
    }
    const Located c = firstVideo(preview, edit.ids);
    if (!c.clip) return v;
    const Clip* prev = neighbour(preview, c.track, *c.clip, false);
    const Clip* next = neighbour(preview, c.track, *c.clip, true);
    switch (edit.kind) {
    case TrimKind::Ripple:
        if (edit.edge == Edge::End)
            v.main = {pane(*c.clip, true, media, name), paneOf(next, false, media, name)};
        else
            v.main = {paneOf(prev, true, media, name), pane(*c.clip, false, media, name)};
        break;
    case TrimKind::Slip: // what changes is the content of the clip itself: its In and Out large
        v.main = {pane(*c.clip, false, media, name), pane(*c.clip, true, media, name)};
        v.small = {paneOf(prev, true, media, name), paneOf(next, false, media, name)};
        break;
    case TrimKind::Slide: // what changes are the neighbours' edges: their Out/In large
        v.main = {paneOf(prev, true, media, name), paneOf(next, false, media, name)};
        v.small = {pane(*c.clip, false, media, name), pane(*c.clip, true, media, name)};
        break;
    case TrimKind::Roll: break; // handled above
    }
    return v;
}

} // namespace TrimFrames
