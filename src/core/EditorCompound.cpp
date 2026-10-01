// Compound Clips und verschachtelte Timelines (DaVinci „New Compound Clip“, „Decompose in Place“,
// Timeline aus dem Media Pool in eine andere Timeline ziehen)
#include "core/Editor.h"

#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"

#include <QSet>
#include <algorithm>

namespace {

Timeline* timelineOf(QVector<Sequence>& list, int id)
{
    for (Sequence& s : list)
        if (s.id == id) return &s.timeline;
    return nullptr;
}

bool hasClips(const QVector<Track>& tracks)
{
    return std::any_of(tracks.begin(), tracks.end(), [](const Track& t) { return !t.clips.isEmpty(); });
}

} // namespace

int Editor::createCompoundClip(const QString& name)
{
    Project* p = m_project;
    const Timeline& cur = p->timeline();
    const QVector<int> ids = withLinked(clipIdsOf(m_selection->ids()));
    if (ids.isEmpty()) return 0;

    // Spanne und unterste/oberste Spur je Art
    int from = INT_MAX, to = 0;
    int lo[2] = {INT_MAX, INT_MAX}, hi[2] = {-1, -1}; // [0] = Video, [1] = Audio
    for (int id : ids) {
        TrackRef ref;
        const Clip* c = TimelineOps::findClip(cur, id, &ref);
        if (!c) continue;
        from = std::min(from, c->start);
        to = std::max(to, c->end());
        const int k = ref.kind == TrackKind::Video ? 0 : 1;
        lo[k] = std::min(lo[k], ref.index);
        hi[k] = std::max(hi[k], ref.index);
    }
    if (from >= to) return 0;

    QString n = name.trimmed();
    if (n.isEmpty()) { // wie DaVinci „Compound Clip 1“ … (erste freie Nummer)
        QSet<QString> used;
        for (const Sequence& s : p->sequences()) used.insert(s.name);
        for (int i = 1;; ++i) {
            n = T("Compound Clip %1").arg(i);
            if (!used.contains(n)) break;
        }
    }
    const int seqId = p->newSequenceId();
    const int parentId = p->currentSequence();
    QSet<int> selectIds;
    p->editSequences(T("Compound Clip erstellen"), [&](QVector<Sequence>& list, int&) {
        Timeline* tl = timelineOf(list, parentId);
        if (!tl) return;
        TimelineOps::detachTransitions(*tl, ids); // Überblendung zu einem Clip außerhalb löst sich
        Timeline inner;
        for (int k = 0; k < 2; ++k) {
            const TrackKind kind = k == 0 ? TrackKind::Video : TrackKind::Audio;
            QVector<Track>& dst = inner.tracks(kind);
            const QVector<Track>& src = tl->tracks(kind);
            const int first = hi[k] < 0 ? 0 : lo[k], last = hi[k] < 0 ? 0 : hi[k];
            for (int i = first; i <= last; ++i) {
                Track t;
                t.kind = kind;
                if (i < src.size()) { // Name/Farbe/Mixer der Spur mitnehmen, Clips nur die ausgewählten
                    t = src[i];
                    t.clips.clear();
                    t.locked = false;
                    t.solo = false;
                    for (const Clip& c : src[i].clips) {
                        if (!ids.contains(c.id)) continue;
                        Clip x = c;
                        x.start -= from;
                        t.clips << x;
                    }
                }
                dst << t;
            }
            // mindestens zwei Spuren je Art wie eine neue Timeline
            while (dst.size() < 2) {
                Track t;
                t.kind = kind;
                dst << t;
            }
        }
        for (int id : ids) TimelineOps::removeClip(*tl, id);

        Clip c;
        c.kind = ClipKind::Compound;
        c.sequenceId = seqId;
        c.start = from;
        c.in = 0;
        c.out = to - from - 1;
        c.linkId = hi[0] >= 0 && hi[1] >= 0 ? p->newLinkId() : 0;
        auto newId = [p] { return p->newClipId(); };
        for (int k = 0; k < 2; ++k) {
            if (hi[k] < 0) continue;
            const TrackKind kind = k == 0 ? TrackKind::Video : TrackKind::Audio;
            // Unterste Spur ab der Auswahl, auf der die ganze Spanne frei und die Spur nicht gesperrt ist
            // (nie andere Clips überschreiben); sonst eine neue Spur oben
            int target = lo[k];
            for (;; ++target) {
                if (target >= tl->tracks(kind).size()) {
                    TimelineOps::ensureTracks(*tl, kind, target + 1);
                    break;
                }
                const Track& t = tl->tracks(kind)[target];
                if (!t.locked && std::none_of(t.clips.begin(), t.clips.end(),
                                              [&](const Clip& x) { return x.start < to && x.end() > from; }))
                    break;
            }
            Clip x = c;
            x.id = newId();
            TimelineOps::placeClip(tl->tracks(kind)[target], x, newId);
            selectIds.insert(x.id);
        }
        list << Sequence{seqId, n, true, 0, inner};
    });
    if (!p->sequence(seqId)) return 0;
    m_selection->set(selectIds);
    return seqId;
}

bool Editor::decomposeCompoundClips(const QVector<int>& ids)
{
    Project* p = m_project;
    const Timeline& cur = p->timeline();
    QVector<int> compounds;
    for (int id : withLinked(ids))
        if (const Clip* c = TimelineOps::findClip(cur, id); c && c->isCompound() && p->sequence(c->sequenceId))
            compounds << id;
    if (compounds.isEmpty()) return false;

    QSet<int> selectIds;
    p->edit(T("Compound Clip auflösen"), [&](Timeline& tl) {
        auto newId = [p] { return p->newClipId(); };
        // Verknüpfungen im Inneren bleiben erhalten, bekommen aber eigene ids (je Compound-Gruppe)
        QHash<QPair<int, int>, int> links; // (Gruppe, alte linkId) -> neue
        // Gruppen: Video- und Audioteil eines Compound Clips; fehlt ein Teil, bringt der andere beide Arten mit
        QSet<int> done;
        for (int id : compounds) {
            if (done.contains(id)) continue;
            QVector<int> group;
            for (int g : TimelineOps::linkedGroup(tl, id))
                if (compounds.contains(g)) group << g;
            if (group.isEmpty()) group << id;
            bool haveKind[2] = {false, false};
            QVector<QPair<Clip, TrackRef>> parts;
            for (int g : group) {
                TrackRef ref;
                if (const Clip* c = TimelineOps::findClip(tl, g, &ref)) {
                    parts.append({*c, ref});
                    haveKind[ref.kind == TrackKind::Video ? 0 : 1] = true;
                }
                done.insert(g);
            }
            for (const auto& part : parts) {
                const Clip& cc = part.first;
                const Timeline* inner = nullptr;
                if (const Sequence* s = p->sequence(cc.sequenceId)) inner = &s->timeline;
                if (!inner) continue;
                TimelineOps::removeClip(tl, cc.id);
                QVector<TrackKind> kinds{part.second.kind};
                const TrackKind other = part.second.kind == TrackKind::Video ? TrackKind::Audio : TrackKind::Video;
                if (!haveKind[other == TrackKind::Video ? 0 : 1]) kinds << other;
                const int offset = cc.start - cc.in;
                for (TrackKind kind : kinds) {
                    const QVector<Track>& src = inner->tracks(kind);
                    const int base = kind == part.second.kind ? part.second.index : 0;
                    int nextTrack = base; // innere Spuren behalten ihre Reihenfolge
                    for (int j = 0; j < src.size(); ++j) {
                        if (std::none_of(src[j].clips.begin(), src[j].clips.end(),
                                         [&](const Clip& n) { return n.end() > cc.in && n.start <= cc.out; }))
                            continue;
                        // Wie beim Erstellen: unterste Spur ab nextTrack, die im Bereich frei und nicht gesperrt ist
                        // (Clips auf den Spuren darüber, z. B. Titel, nie überschreiben); sonst neue Spur
                        int target = nextTrack;
                        for (;; ++target) {
                            if (target >= tl.tracks(kind).size()) break;
                            const Track& t = tl.tracks(kind)[target];
                            if (!t.locked && std::none_of(t.clips.begin(), t.clips.end(), [&](const Clip& x) {
                                    return x.start < cc.end() && x.end() > cc.start;
                                }))
                                break;
                        }
                        nextTrack = target + 1;
                        for (const Clip& n : src[j].clips) {
                            if (n.end() <= cc.in || n.start > cc.out) continue;
                            Clip x = n;
                            if (x.start < cc.in) { // vorne abgeschnitten: Anfang nachziehen
                                const int d = cc.in - x.start;
                                x.start += d;
                                x.in += d;
                                x.transIn = 0;
                                x.transInAlone = false;
                                x.fadeIn = 0;
                            }
                            if (x.end() > cc.out + 1) {
                                x.out -= x.end() - (cc.out + 1);
                                x.transOut = 0;
                                x.transOutAlone = false;
                                x.fadeOut = 0;
                            }
                            x.start += offset;
                            x.id = newId();
                            if (x.linkId) {
                                const QPair<int, int> key{group.first(), n.linkId};
                                if (!links.contains(key)) links.insert(key, p->newLinkId());
                                x.linkId = links.value(key);
                            }
                            if (!cc.enabled) x.enabled = false;
                            TimelineOps::ensureTracks(tl, kind, target + 1);
                            TimelineOps::placeClip(tl.tracks(kind)[target], x, newId);
                            selectIds.insert(x.id);
                        }
                    }
                }
            }
        }
    });
    m_selection->set(selectIds);
    return true;
}

int Editor::selectedCompoundSequence() const
{
    for (int id : m_selection->ids())
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id); c && c->isCompound())
            return c->sequenceId;
    return 0;
}

bool Editor::addSequenceAt(int sequenceId, int frame, int track)
{
    Project* p = m_project;
    const Sequence* s = p->sequence(sequenceId);
    if (!s || !p->canNest(sequenceId, p->currentSequence())) return false;
    const int len = TimelineOps::endFrame(s->timeline);
    if (len <= 0) return false;
    const int idx = std::max(0, track);
    const bool video = hasClips(s->timeline.video) && !isTrackLocked({TrackKind::Video, idx});
    const bool audio = hasClips(s->timeline.audio) && !isTrackLocked({TrackKind::Audio, idx});
    if (!video && !audio) return false;
    QSet<int> selectIds;
    p->edit(T("Einfügen: %1").arg(s->name), [&](Timeline& tl) {
        auto newId = [p] { return p->newClipId(); };
        Clip c;
        c.kind = ClipKind::Compound;
        c.sequenceId = sequenceId;
        c.start = std::max(0, frame);
        c.in = 0;
        c.out = len - 1;
        c.linkId = video && audio ? p->newLinkId() : 0;
        for (TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
            if (kind == TrackKind::Video ? !video : !audio) continue;
            TimelineOps::ensureTracks(tl, kind, idx + 1);
            Clip x = c;
            x.id = newId();
            TimelineOps::placeClip(tl.tracks(kind)[idx], x, newId);
            selectIds.insert(x.id);
        }
    });
    m_selection->set(selectIds);
    return true;
}
