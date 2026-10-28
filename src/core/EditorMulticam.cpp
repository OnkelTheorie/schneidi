// Multicam clips like DaVinci (docs/archiv/plan-0.8.md): create from synced recordings, cut/switch angles, flatten,
// resync and change angles afterwards. Model and lookups in core/Multicam.h.
#include "core/Editor.h"

#include "core/EditorDetail.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"

#include <QSet>
#include <algorithm>
#include <climits>

namespace {

using Mode = Multicam::Mode;

bool modeHas(Mode m, TrackKind k) { return m == Mode::Both || (m == Mode::Video) == (k == TrackKind::Video); }

struct UsableFile {
    MediaInfo info;
    int offset = 0;
};

// Files of an angle that can be placed (in the Media Pool, video or audio, not a still), by offset
QVector<UsableFile> usableFiles(const Project* p, const Multicam::Angle& a)
{
    QVector<UsableFile> out;
    for (const Multicam::FilePlacement& f : a.files) {
        const MediaInfo* m = p->mediaInfo(f.path);
        if (!m || m->isImage || m->length <= 0 || (!m->hasVideo && !m->hasAudio)) continue;
        out << UsableFile{*m, f.offset};
    }
    std::stable_sort(out.begin(), out.end(), [](const UsableFile& x, const UsableFile& y) { return x.offset < y.offset; });
    return out;
}

// Name of the audio track of a placed stream of an angle (several streams: "<angle> – <track label or stream title>")
QString audioTrackName(const QString& angle, const MediaInfo& m, const StreamPlacement& s)
{
    if (m.audioStreamCount() < 2) return angle;
    return QString("%1 – %2").arg(angle, s.trackLabel.isEmpty() ? m.audioStreamName(s.stream) : s.trackLabel);
}

// Puts the files of one angle onto new tracks on top of `inner`: one video track (if any file has a picture) and one
// audio track per placed audio stream (import presets); each file at offset + shift. A file overlapping the previous
// one of the angle is cut at its start, as is the part before frame 0. Picture and sound of a file are linked.
MulticamAngle placeAngle(Timeline& inner, const QString& name, const QVector<UsableFile>& files, int shift, Project* p,
                         const StreamPresets& presets)
{
    MulticamAngle a;
    a.name = name;
    QVector<QVector<StreamPlacement>> placed;
    int streams = 0, widest = 0;
    bool video = false;
    for (const UsableFile& f : files) {
        video = video || f.info.hasVideo;
        placed << presets.placements(f.info);
        if (placed.last().size() > streams) {
            streams = placed.last().size();
            widest = placed.size() - 1;
        }
    }
    if (video) {
        Track t;
        t.kind = TrackKind::Video;
        t.name = name;
        inner.video << t;
        a.videoTrack = inner.video.size() - 1;
    }
    for (int k = 0; k < streams; ++k) {
        Track t;
        t.kind = TrackKind::Audio;
        t.name = audioTrackName(name, files[widest].info, placed[widest][k]);
        inner.audio << t;
        a.audioTracks << inner.audio.size() - 1;
    }
    int prevEnd = INT_MIN;
    for (int i = 0; i < files.size(); ++i) {
        const UsableFile& f = files[i];
        int start = f.offset + shift;
        int cut = std::max(0, -start);
        if (prevEnd > start) cut = std::max(cut, prevEnd - start);
        if (cut >= f.info.length) continue;
        Clip c;
        c.mediaPath = f.info.path;
        c.start = start + cut;
        c.in = cut;
        c.out = f.info.length - 1;
        const bool picture = f.info.hasVideo && a.videoTrack >= 0;
        const int n = std::min<int>(placed[i].size(), a.audioTracks.size());
        c.linkId = picture && n > 0 ? p->newLinkId() : 0;
        if (picture) {
            Clip x = c;
            x.id = p->newClipId();
            inner.video[a.videoTrack].clips << x;
        }
        for (int k = 0; k < n; ++k) {
            Clip x = c;
            x.id = p->newClipId();
            x.audioStream = placed[i][k].stream;
            x.enabled = placed[i][k].enabled;
            x.volumeDb = placed[i][k].volumeDb;
            inner.audio[a.audioTracks[k]].clips << x;
        }
        prevEnd = c.end();
    }
    return a;
}

// Path of the first recording of an angle (picture, else sound)
QString firstFile(const Sequence& s, const MulticamAngle& a, int* position = nullptr)
{
    QVector<const Track*> tracks;
    if (a.videoTrack >= 0 && a.videoTrack < s.timeline.video.size()) tracks << &s.timeline.video[a.videoTrack];
    for (int t : a.audioTracks)
        if (t >= 0 && t < s.timeline.audio.size()) tracks << &s.timeline.audio[t];
    for (const Track* t : tracks)
        if (!t->clips.isEmpty()) {
            if (position) *position = t->clips.first().start - t->clips.first().in;
            return t->clips.first().mediaPath;
        }
    return {};
}

Sequence* sequenceIn(QVector<Sequence>& list, int id)
{
    for (Sequence& s : list)
        if (s.id == id) return &s;
    return nullptr;
}

// What replaces segment `seg` for the part [from, to) (frames of the multicam sequence) of the angle's clip `n`:
// the file section, with what was set on the segment (transform, effects, volume, fades/transitions at its edges)
Clip flatClip(const Clip& seg, const Clip& n, int from, int to)
{
    Clip x = n; // file, stream, speed and what was set on the recording inside the multicam clip
    x.start = seg.start + (from - seg.in);
    x.in = n.in + (from - n.start);
    x.out = x.in + (to - from) - 1;
    x.enabled = seg.enabled && n.enabled;
    const bool atStart = from == seg.in, atEnd = to == seg.out + 1;
    EditorDetail::takeOuter(x, seg, n.in - n.start, atStart, atEnd);
    x.renderCache = seg.renderCache;
    // A recording's own edges inside the multicam clip are no cuts of the timeline: only the segment's edges count
    x.transIn = atStart ? seg.transIn : 0;
    x.transInAlone = atStart && seg.transInAlone;
    x.transInStyle = seg.transInStyle;
    x.fadeIn = atStart ? seg.fadeIn : 0;
    x.transOut = atEnd ? seg.transOut : 0;
    x.transOutAlone = atEnd && seg.transOutAlone;
    x.transOutStyle = seg.transOutStyle;
    x.fadeOut = atEnd ? seg.fadeOut : 0;
    return x;
}

} // namespace

bool Editor::inMulticamSequence() const
{
    const Sequence* s = m_project->sequence(m_project->currentSequence());
    return s && s->multicam;
}

int Editor::createMulticamClip(const QString& name, const QVector<Multicam::Angle>& angles, int bin)
{
    Project* p = m_project;
    struct Usable {
        QString name;
        QVector<UsableFile> files;
    };
    QVector<Usable> list;
    int earliest = INT_MAX;
    for (const Multicam::Angle& a : angles) {
        const QVector<UsableFile> files = usableFiles(p, a);
        if (files.isEmpty()) continue;
        QString n = a.name.trimmed();
        if (n.isEmpty()) n = Multicam::defaultAngleName(files.first().info.path);
        list << Usable{n, files};
        for (const UsableFile& f : files) earliest = std::min(earliest, f.offset);
    }
    if (list.isEmpty()) return 0;

    // The earliest recording starts at frame 0
    Timeline inner;
    QVector<MulticamAngle> table;
    for (const Usable& u : list) table << placeAngle(inner, u.name, u.files, -earliest, p, m_streamPresets);
    for (TrackKind kind : {TrackKind::Video, TrackKind::Audio}) // e.g. only sound: still one track (like loading)
        if (inner.tracks(kind).isEmpty()) TimelineOps::ensureTracks(inner, kind, 1);

    QString n = name.trimmed();
    if (n.isEmpty()) { // like DaVinci "Multicam Clip 1" ... (first free number)
        QSet<QString> used;
        for (const Sequence& s : p->sequences()) used.insert(s.name);
        for (int i = 1;; ++i) {
            n = T("Multicam-Clip %1").arg(i);
            if (!used.contains(n)) break;
        }
    }
    if (bin != 0 && !p->bin(bin)) bin = 0;
    const int seqId = p->newSequenceId();
    p->editSequences(T("Multicam-Clip erstellen"), [&](QVector<Sequence>& seqs, int&) {
        Sequence s{seqId, n, true, bin, inner};
        s.multicam = true;
        s.angles = table;
        seqs << s;
    });
    return p->sequence(seqId) ? seqId : 0;
}

namespace {

struct Part {
    int id = 0;
    TrackRef ref;
    int angle = 0;
    int start = 0;
};

} // namespace

// Segment the cut/switch acts on at `frame` and its linked parts of the same multicam clip there (not locked)
static QVector<Part> segmentAt(const Editor& ed, const Project& p, const Selection& sel, int frame, const Sequence** seq)
{
    const Timeline& tl = p.timeline();
    const Clip* target = Multicam::clipAt(tl, frame, sel.ids());
    *seq = target ? p.sequence(target->sequenceId) : nullptr;
    if (!target || !*seq) return {};
    QVector<Part> parts;
    for (int id : TimelineOps::linkedGroup(tl, target->id)) {
        TrackRef ref;
        const Clip* c = TimelineOps::findClip(tl, id, &ref);
        if (!c || !c->isMulticam() || c->sequenceId != target->sequenceId || frame < c->start || frame >= c->end()
            || ed.isTrackLocked(ref))
            continue;
        parts << Part{id, ref, c->angle, c->start};
    }
    return parts;
}

bool Editor::multicamCut(int frame, int angle, Multicam::Mode mode)
{
    Project* p = m_project;
    const Sequence* s = nullptr;
    const QVector<Part> parts = segmentAt(*this, *p, *m_selection, frame, &s);
    if (parts.isEmpty() || angle < 0 || angle >= s->angles.size()) return false;
    if (mode != Mode::Both && !Multicam::angleHas(*s, angle, mode == Mode::Video ? TrackKind::Video : TrackKind::Audio))
        return false;
    auto wants = [&](const Part& x) {
        return modeHas(mode, x.ref.kind) && Multicam::angleHas(*s, angle, x.ref.kind) && x.angle != angle;
    };
    if (std::none_of(parts.begin(), parts.end(), wants)) return false;
    const QString angleName = s->angles[angle].name;
    p->edit(T("Multicam-Schnitt: %1").arg(angleName), [&](Timeline& tl) {
        QVector<int> split;
        for (const Part& x : parts)
            if (x.start < frame) split << x.id;
        TimelineOps::splitAt(tl, split, frame, [p] { return p->newClipId(); }, [p] { return p->newLinkId(); });
        for (const Part& x : parts) {
            if (!wants(x)) continue;
            for (Clip& c : tl.track(x.ref).clips)
                if (c.start == frame && c.isMulticam()) c.angle = angle; // the right piece (or the part itself)
        }
    });
    return true;
}

bool Editor::multicamSwitch(int frame, int angle, Multicam::Mode mode)
{
    Project* p = m_project;
    const Sequence* s = nullptr;
    const QVector<Part> parts = segmentAt(*this, *p, *m_selection, frame, &s);
    if (parts.isEmpty() || angle < 0 || angle >= s->angles.size()) return false;
    auto wants = [&](const Part& x) {
        return modeHas(mode, x.ref.kind) && Multicam::angleHas(*s, angle, x.ref.kind) && x.angle != angle;
    };
    if (std::none_of(parts.begin(), parts.end(), wants)) return false;
    p->edit(T("Multicam-Winkel: %1").arg(s->angles[angle].name), [&](Timeline& tl) {
        for (const Part& x : parts)
            if (wants(x))
                if (Clip* c = TimelineOps::findClip(tl, x.id)) c->angle = angle;
    });
    return true;
}

void Editor::setMulticamAngle(const QVector<int>& ids, int angle, Multicam::Mode mode)
{
    Project* p = m_project;
    const Timeline& cur = p->timeline();
    QVector<int> all;
    for (int id : editable(ids))
        for (int g : TimelineOps::linkedGroup(cur, id))
            if (!all.contains(g)) all << g;
    QVector<int> change;
    for (int id : editable(all)) {
        TrackRef ref;
        const Clip* c = TimelineOps::findClip(cur, id, &ref);
        if (!c || !c->isMulticam() || c->angle == angle || !modeHas(mode, ref.kind)) continue;
        if (const Sequence* s = p->sequence(c->sequenceId); s && Multicam::angleHas(*s, angle, ref.kind)) change << id;
    }
    if (change.isEmpty()) return;
    p->edit(T("Multicam-Winkel ändern"), [&](Timeline& tl) {
        for (int id : change)
            if (Clip* c = TimelineOps::findClip(tl, id)) c->angle = angle;
    });
}

bool Editor::flattenMulticamClips(const QVector<int>& ids)
{
    Project* p = m_project;
    const Timeline& cur = p->timeline();
    // Groups: a segment with its linked parts of the same multicam clip
    QVector<QVector<int>> groups;
    QSet<int> seen;
    for (int id : editable(ids)) {
        const Clip* c0 = TimelineOps::findClip(cur, id);
        if (seen.contains(id) || !c0 || !c0->isMulticam() || !p->sequence(c0->sequenceId)) continue;
        QVector<int> g;
        for (int x : editable(TimelineOps::linkedGroup(cur, id))) {
            const Clip* c = TimelineOps::findClip(cur, x);
            if (c && c->isMulticam() && c->sequenceId == c0->sequenceId && !seen.contains(x)) {
                g << x;
                seen.insert(x);
            }
        }
        if (!g.isEmpty()) groups << g;
    }
    if (groups.isEmpty()) return false;

    QSet<int> created;
    p->edit(T("Multicam-Clip glätten"), [&](Timeline& tl) {
        auto newId = [p] { return p->newClipId(); };
        for (const QVector<int>& g : groups) {
            QVector<QPair<Clip, TrackRef>> parts;
            for (int id : g) {
                TrackRef ref;
                if (const Clip* c = TimelineOps::findClip(tl, id, &ref)) parts.append({*c, ref});
            }
            for (const auto& part : parts) TimelineOps::removeClip(tl, part.first.id);
            QHash<int, int> links; // link inside the multicam clip -> new link
            for (const auto& [seg, ref] : parts) {
                const Sequence* s = p->sequence(seg.sequenceId);
                if (!s || seg.angle < 0 || seg.angle >= s->angles.size()) continue;
                const MulticamAngle& a = s->angles[seg.angle];
                const QVector<int> inner = ref.kind == TrackKind::Audio ? a.audioTracks
                                           : a.videoTrack >= 0          ? QVector<int>{a.videoTrack}
                                                                        : QVector<int>{};
                const QVector<Track>& src = s->timeline.tracks(ref.kind);
                int nextTrack = ref.index; // like decompose: the angle's tracks keep their order
                for (int j : inner) {
                    if (j < 0 || j >= src.size()) continue;
                    QVector<Clip> pieces; // linkId still the one inside
                    for (const Clip& n : src[j].clips) {
                        const int from = std::max(n.start, seg.in), to = std::min(n.end(), seg.out + 1);
                        if (from < to) pieces << flatClip(seg, n, from, to); // gaps of the recording stay empty
                    }
                    if (pieces.isEmpty()) continue;
                    // Lowest track from nextTrack on that is free there and not locked (never overwrite), else a new one
                    int target = nextTrack;
                    for (;; ++target) {
                        if (target >= tl.tracks(ref.kind).size()) break;
                        const Track& t = tl.tracks(ref.kind)[target];
                        if (!t.locked && std::none_of(t.clips.begin(), t.clips.end(), [&](const Clip& x) {
                                return x.start < seg.end() && x.end() > seg.start;
                            }))
                            break;
                    }
                    nextTrack = target + 1;
                    TimelineOps::ensureTracks(tl, ref.kind, target + 1);
                    for (Clip x : pieces) {
                        x.id = newId();
                        if (x.linkId) {
                            if (!links.contains(x.linkId)) links.insert(x.linkId, p->newLinkId());
                            x.linkId = links.value(x.linkId);
                        }
                        TimelineOps::placeClip(tl.tracks(ref.kind)[target], x, newId);
                        created.insert(x.id);
                    }
                }
            }
        }
        // Picture and sound of different angles: a link with a single clip is none
        QHash<int, int> count;
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (const Track& t : tl.tracks(k))
                for (const Clip& c : t.clips)
                    if (created.contains(c.id) && c.linkId) ++count[c.linkId];
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (Track& t : tl.tracks(k))
                for (Clip& c : t.clips)
                    if (created.contains(c.id) && c.linkId && count.value(c.linkId) < 2) c.linkId = 0;
    });
    m_selection->set(created);
    return true;
}

void Editor::renameMulticamAngle(int sequenceId, int angle, const QString& name)
{
    Project* p = m_project;
    const Sequence* s = p->sequence(sequenceId);
    if (!s || !s->multicam || angle < 0 || angle >= s->angles.size()) return;
    QString n = name.trimmed();
    if (n.isEmpty()) n = Multicam::defaultAngleName(firstFile(*s, s->angles[angle]));
    const QString old = s->angles[angle].name;
    if (n.isEmpty() || n == old) return;
    p->editSequences(T("Winkel umbenennen"), [&](QVector<Sequence>& list, int&) {
        Sequence* x = sequenceIn(list, sequenceId);
        if (!x) return;
        MulticamAngle& a = x->angles[angle];
        a.name = n;
        // Track names made from the angle name follow it (own names stay)
        if (a.videoTrack >= 0 && a.videoTrack < x->timeline.video.size() && x->timeline.video[a.videoTrack].name == old)
            x->timeline.video[a.videoTrack].name = n;
        for (int t : a.audioTracks) {
            if (t < 0 || t >= x->timeline.audio.size()) continue;
            QString& tn = x->timeline.audio[t].name;
            if (tn == old) tn = n;
            else if (tn.startsWith(old + QStringLiteral(" – "))) tn = n + tn.mid(old.size());
        }
    });
}

void Editor::nudgeMulticamAngle(int sequenceId, int angle, int frames)
{
    Project* p = m_project;
    const Sequence* s = p->sequence(sequenceId);
    if (!s || !s->multicam || angle < 0 || angle >= s->angles.size() || frames == 0) return;
    const MulticamAngle a = s->angles[angle];
    QVector<TrackRef> refs;
    if (a.videoTrack >= 0 && a.videoTrack < s->timeline.video.size()) refs << TrackRef{TrackKind::Video, a.videoTrack};
    for (int t : a.audioTracks)
        if (t >= 0 && t < s->timeline.audio.size()) refs << TrackRef{TrackKind::Audio, t};
    int earliest = INT_MAX;
    for (TrackRef r : refs)
        for (const Clip& c : s->timeline.track(r).clips) earliest = std::min(earliest, c.start);
    if (earliest == INT_MAX) return;
    const int delta = std::max(frames, -earliest); // nothing is cut off at the front
    if (delta == 0) return;
    p->editSequences(T("Winkel verschieben"), [&](QVector<Sequence>& list, int&) {
        if (Sequence* x = sequenceIn(list, sequenceId))
            for (TrackRef r : refs)
                for (Clip& c : x->timeline.track(r).clips) c.start += delta;
    });
}

int Editor::addMulticamAngle(int sequenceId, const Multicam::Angle& angle)
{
    Project* p = m_project;
    const Sequence* s = p->sequence(sequenceId);
    if (!s || !s->multicam) return -1;
    const QVector<UsableFile> files = usableFiles(p, angle);
    if (files.isEmpty()) return -1;
    int reference = 0; // where the reference recording (first file of the first angle) sits in the sequence
    if (!s->angles.isEmpty()) firstFile(*s, s->angles.first(), &reference);
    QString n = angle.name.trimmed();
    if (n.isEmpty()) n = Multicam::defaultAngleName(files.first().info.path);
    Timeline inner = s->timeline;
    const MulticamAngle a = placeAngle(inner, n, files, reference, p, m_streamPresets);
    const int index = s->angles.size();
    p->editSequences(T("Winkel hinzufügen"), [&](QVector<Sequence>& list, int&) {
        if (Sequence* x = sequenceIn(list, sequenceId)) {
            x->timeline = inner;
            x->angles << a;
        }
    });
    return index;
}
