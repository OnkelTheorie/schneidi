// SPDX-License-Identifier: AGPL-3.0-only
#include "core/Story.h"

#include "core/TimelineOps.h"
#include "core/TimelineStory.h"

#include <QHash>

#include <algorithm>

namespace Story {

QString add(QVector<StorySection>& sections, const StorySection& s)
{
    if (s.from < 0 || s.to <= s.from) return QStringLiteral("a section needs from < to");
    for (const StorySection& o : sections)
        if (o.from < s.to && s.from < o.to)
            return QString("overlaps the %1 section %2-%3 (frames; remove it first or choose another range)")
                .arg(TimelineStory::kindName(o.kind))
                .arg(o.from)
                .arg(o.to);
    const auto pos = std::upper_bound(sections.begin(), sections.end(), s,
                                      [](const StorySection& a, const StorySection& b) { return a.from < b.from; });
    sections.insert(pos, s);
    return {};
}

int at(const QVector<StorySection>& sections, int frame)
{
    for (int i = 0; i < sections.size(); ++i)
        if (sections[i].from <= frame && frame < sections[i].to) return i;
    return -1;
}

QVector<StorySection> suggest(int length, QVector<int> cuts, int fps, int hookMax)
{
    QVector<StorySection> out;
    if (length <= 0) return out;
    fps = std::max(1, fps);
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    cuts.erase(std::remove_if(cuts.begin(), cuts.end(), [&](int c) { return c <= 0 || c >= length; }), cuts.end());
    hookMax = std::clamp(hookMax, 1, length);
    // Hook: up to the first cut after a second, else as long as a hook may be
    int hookEnd = hookMax;
    for (int c : cuts)
        if (c >= fps) {
            hookEnd = std::min(c, hookMax);
            break;
        }
    out << StorySection{StoryKind::Hook, {}, 0, hookEnd};
    if (hookEnd >= length) return out;
    // Resolution: the last shot, within the last quarter and at least the last 15 %
    int resolution = cuts.isEmpty() ? length : cuts.last();
    resolution = std::max(resolution, length - length / 4);
    resolution = std::min(resolution, length - std::max(1, length * 15 / 100));
    if (resolution - hookEnd < fps || length - resolution < 1) { // too short for three parts
        out << StorySection{StoryKind::Build, {}, hookEnd, length};
        return out;
    }
    out << StorySection{StoryKind::Build, {}, hookEnd, resolution}
        << StorySection{StoryKind::Resolution, {}, resolution, length};
    return out;
}

void shift(QVector<StorySection>& sections, QVector<TimelineNote>& notes, QVector<int>& markers, int frame, int length)
{
    if (length == 0) return;
    if (length > 0) {
        for (StorySection& s : sections) {
            if (s.from >= frame) s.from += length;
            if (s.to > frame) s.to += length;
        }
        for (TimelineNote& n : notes)
            if (n.at >= frame) n.at += length;
        for (int& m : markers)
            if (m >= frame) m += length;
        return;
    }
    const int end = frame - length; // [frame, end) goes
    const auto moved = [&](int x) { return x >= end ? x + length : std::min(x, frame); };
    QVector<StorySection> kept;
    for (StorySection s : sections) {
        s.from = moved(s.from);
        s.to = moved(s.to);
        if (s.to > s.from) kept << s;
    }
    sections = kept;
    notes.erase(std::remove_if(notes.begin(), notes.end(), [&](const TimelineNote& n) { return n.at >= frame && n.at < end; }),
                notes.end());
    for (TimelineNote& n : notes) n.at = moved(n.at);
    markers.erase(std::remove_if(markers.begin(), markers.end(), [&](int m) { return m >= frame && m < end; }), markers.end());
    for (int& m : markers) m = moved(m);
}

bool prependTeaser(Timeline& tl, int from, int to, const std::function<int()>& newClipId,
                   const std::function<int()>& newLinkId)
{
    if (from < 0 || to <= from) return false;
    const int length = to - from;
    // The pieces: a scratch copy cut at both ends, the clips inside the range
    Timeline cut = tl;
    QVector<int> all;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const Track& t : cut.tracks(k))
            for (const Clip& c : t.clips) all << c.id;
    all << TimelineOps::splitAt(cut, all, from, newClipId, newLinkId);
    TimelineOps::splitAt(cut, all, to, newClipId, newLinkId);
    struct Piece {
        TrackRef ref;
        Clip clip;
    };
    QVector<Piece> pieces;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio}) {
        const QVector<Track>& tracks = cut.tracks(k);
        for (int i = 0; i < tracks.size(); ++i)
            for (const Clip& c : tracks[i].clips)
                if (c.start >= from && c.end() <= to) pieces << Piece{{k, i}, c};
    }
    if (pieces.isEmpty()) return false;
    // The timeline moves back (unlocked or not: the whole edit makes room), then the pieces go in front
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (Track& t : tl.tracks(k)) TimelineOps::shiftFrom(t, 0, length);
    QVector<SubtitleTrack> cues = tl.subtitles;
    for (SubtitleTrack& st : tl.subtitles)
        for (SubtitleCue& c : st.cues) {
            c.start += length;
            c.end += length;
        }
    for (int i = 0; i < cues.size() && i < tl.subtitles.size(); ++i) {
        QVector<SubtitleCue> front;
        for (SubtitleCue c : cues[i].cues) {
            if (c.end <= from || c.start >= to) continue;
            const int a = std::max(c.start, from), b = std::min(c.end, to);
            if (a != c.start || b != c.end) c.words.clear(); // cut: the words share the rest
            c.id = newClipId();
            c.start = a - from;
            c.end = b - from;
            front << c;
        }
        tl.subtitles[i].cues = front + tl.subtitles[i].cues;
    }
    shift(tl.sections, tl.notes, tl.markers, 0, length);
    if (tl.markIn >= 0) tl.markIn += length;
    if (tl.markOut >= 0) tl.markOut += length;
    QHash<int, int> links;
    for (Piece p : pieces) {
        p.clip.id = newClipId();
        p.clip.start -= from;
        if (p.clip.linkId) {
            if (!links.contains(p.clip.linkId)) links[p.clip.linkId] = newLinkId();
            p.clip.linkId = links[p.clip.linkId];
        }
        TimelineOps::placeClip(tl.track(p.ref), p.clip, newClipId);
    }
    return true;
}

} // namespace Story
