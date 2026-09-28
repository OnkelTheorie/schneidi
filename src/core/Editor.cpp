#include "core/Editor.h"

#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"

#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <climits>
#include <cstdlib>
#include <algorithm>

Editor::Editor(Project* project, Selection* selection, QObject* parent)
    : QObject(parent), m_project(project), m_selection(selection) {}

QVector<int> Editor::withLinked(const QVector<int>& ids) const
{
    if (!m_linkedSelection) return ids;
    QVector<int> out;
    for (int id : ids)
        for (int g : TimelineOps::linkedGroup(m_project->timeline(), id))
            if (!out.contains(g)) out << g;
    return out;
}

void Editor::addMediaAt(const QStringList& paths, int frame, int track)
{
    Project* p = m_project;
    QVector<const MediaInfo*> infos;
    for (const QString& path : paths)
        if (const MediaInfo* info = p->mediaInfo(path); info && info->length > 0) infos << info;
    if (infos.isEmpty()) return;

    const QString text = infos.size() == 1 ? T("Einfügen: %1").arg(infos.first()->name)
                                           : T("%1 Clips einfügen").arg(infos.size());
    p->edit(text, [&](Timeline& tl) {
        auto newId = [p] { return p->newClipId(); };
        const int idx = std::max(0, track);
        int start = std::max(0, frame);
        for (const MediaInfo* info : infos) {
            Clip c;
            c.mediaPath = info->path;
            c.start = start;
            c.in = 0;
            c.out = info->length - 1;
            c.linkId = (info->hasVideo && info->hasAudio) ? p->newLinkId() : 0;
            if (info->hasVideo) {
                TimelineOps::ensureTracks(tl, TrackKind::Video, idx + 1);
                Clip v = c;
                v.id = newId();
                TimelineOps::placeClip(tl.video[idx], v, newId);
            }
            if (info->hasAudio) {
                TimelineOps::ensureTracks(tl, TrackKind::Audio, idx + 1);
                Clip a = c;
                a.id = newId();
                TimelineOps::placeClip(tl.audio[idx], a, newId);
            }
            start += info->length;
        }
    });
}

void Editor::addTitle(int frame, int track)
{
    Project* p = m_project;
    const Timeline& cur = p->timeline();
    Clip c;
    c.kind = ClipKind::Title;
    c.title.text = T("Titel");
    c.start = std::max(0, frame);
    c.in = 0;
    c.out = 5 * p->fps() - 1;
    if (track < 0) {
        track = 0;
        for (int i = 0; i < cur.video.size(); ++i)
            for (const Clip& x : cur.video[i].clips)
                if (x.start < c.end() && x.end() > c.start) track = i + 1;
    }
    c.id = p->newClipId();
    p->edit(T("Titel einfügen"), [&](Timeline& tl) {
        TimelineOps::ensureTracks(tl, TrackKind::Video, track + 1);
        TimelineOps::placeClip(tl.video[track], c, [p] { return p->newClipId(); });
    });
    m_selection->set({c.id});
}

void Editor::toggleTrackMute(TrackRef ref)
{
    m_project->edit(T("Spur stumm"), [&](Timeline& tl) { tl.track(ref).muted = !tl.track(ref).muted; });
}

void Editor::toggleTrackHidden(TrackRef ref)
{
    m_project->edit(T("Spur ausblenden"), [&](Timeline& tl) { tl.track(ref).hidden = !tl.track(ref).hidden; });
}

void Editor::moveClips(const QVector<int>& ids, int deltaFrames, TrackKind kind, int trackDelta)
{
    if (ids.isEmpty() || (deltaFrames == 0 && trackDelta == 0)) return;
    Project* p = m_project;
    p->edit(T("Clips verschieben"), [&](Timeline& tl) {
        TimelineOps::detachTransitions(tl, ids);
        TimelineOps::moveClips(tl, ids, deltaFrames, kind, trackDelta,
                               [p] { return p->newClipId(); });
    });
}

TimelineOps::SourceLength Editor::sourceLength() const
{
    return [p = m_project](const QString& path) {
        const MediaInfo* m = p->mediaInfo(path);
        return m && !m->isImage ? m->length : 0; // Standbilder beliebig lang ziehbar
    };
}

int Editor::clampTrim(int clipId, TimelineOps::Edge edge, int delta) const
{
    return TimelineOps::clampTrim(m_project->timeline(), withLinked({clipId}), edge, delta, sourceLength());
}

void Editor::trimClip(int clipId, TimelineOps::Edge edge, int delta)
{
    const QVector<int> ids = withLinked({clipId});
    if (TimelineOps::clampTrim(m_project->timeline(), ids, edge, delta, sourceLength()) == 0) return;
    m_project->edit(T("Trimmen"), [&](Timeline& tl) { TimelineOps::trimClips(tl, ids, edge, delta, sourceLength()); });
}

void Editor::setClipVolume(int clipId, double db)
{
    db = std::clamp(db, kMinVolumeDb, kMaxVolumeDb);
    const Clip* c = TimelineOps::findClip(m_project->timeline(), clipId);
    if (!c || c->volumeDb == db) return;
    m_project->edit(T("Lautstärke"), [&](Timeline& tl) {
        if (Clip* clip = TimelineOps::findClip(tl, clipId)) clip->volumeDb = db;
    });
}

void Editor::bladeAt(int clipId, int frame)
{
    Project* p = m_project;
    const QVector<int> ids = withLinked({clipId});
    p->edit(T("Schnitt"), [&](Timeline& tl) {
        TimelineOps::splitAt(tl, ids, frame, [p] { return p->newClipId(); },
                             [p] { return p->newLinkId(); });
    });
}

QVector<int> Editor::targetIds(int frame) const
{
    if (!m_selection->isEmpty()) return withLinked(m_selection->ids().values().toVector());
    QVector<int> ids;
    const Timeline& tl = m_project->timeline();
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const auto& t : tl.tracks(k))
            for (const auto& c : t.clips)
                if (frame > c.start && frame < c.end()) ids << c.id;
    return ids;
}

void Editor::splitAtPlayhead(int frame)
{
    // Wie DaVinci: mit Auswahl nur die ausgewählten Clips, sonst alle unter dem Playhead.
    // Optional: alle Clips auf den Spuren der Auswahl (Playhead steht schon auf dem Nachbarclip).
    QVector<int> ids;
    if (m_splitOnSelectedTracks && !m_selection->isEmpty()) {
        const Timeline& tl = m_project->timeline();
        const QVector<int> selected = withLinked(m_selection->ids().values().toVector());
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (const auto& t : tl.tracks(k)) {
                const bool hasSelected = std::any_of(t.clips.begin(), t.clips.end(),
                                                     [&](const Clip& c) { return selected.contains(c.id); });
                if (!hasSelected) continue;
                for (const auto& c : t.clips)
                    if (frame > c.start && frame < c.end()) ids << c.id;
            }
        ids = withLinked(ids);
    } else {
        ids = targetIds(frame);
    }
    if (ids.isEmpty()) return;
    Project* p = m_project;
    p->edit(T("Schnitt am Playhead"), [&](Timeline& tl) {
        TimelineOps::splitAt(tl, ids, frame, [p] { return p->newClipId(); },
                             [p] { return p->newLinkId(); });
    });
}

void Editor::setClipFade(int clipId, TimelineOps::Edge edge, int frames, const QString& mergeKey)
{
    const Clip* c = TimelineOps::findClip(m_project->timeline(), clipId);
    if (!c) return;
    const bool in = edge == TimelineOps::Edge::Start;
    frames = std::clamp(frames, 0, c->length() - (in ? c->fadeOut : c->fadeIn));
    if (frames == (in ? c->fadeIn : c->fadeOut)) return;
    m_project->edit(in ? T("Einblenden") : T("Ausblenden"), [&](Timeline& tl) {
        if (Clip* x = TimelineOps::findClip(tl, clipId)) (in ? x->fadeIn : x->fadeOut) = frames;
    }, mergeKey);
}

void Editor::modifyClips(const QVector<int>& ids, const QString& text, const std::function<void(Clip&)>& fn,
                         const QString& mergeKey)
{
    if (ids.isEmpty()) return;
    m_project->edit(text, [&](Timeline& tl) {
        for (int id : ids)
            if (Clip* c = TimelineOps::findClip(tl, id)) fn(*c);
    }, mergeKey);
}

void Editor::rippleDeleteSelection()
{
    if (m_selection->isEmpty()) return;
    const QVector<int> ids = m_selection->ids().values().toVector();
    m_project->edit(T("Löschen mit Ripple"), [&](Timeline& tl) {
        TimelineOps::detachTransitions(tl, ids);
        TimelineOps::rippleDelete(tl, ids);
    });
    m_selection->clear();
}

void Editor::selectAll()
{
    QSet<int> ids;
    const Timeline& tl = m_project->timeline();
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const auto& t : tl.tracks(k))
            for (const auto& c : t.clips) ids.insert(c.id);
    m_selection->set(ids);
}

void Editor::nudgeSelection(int frames)
{
    if (m_selection->isEmpty()) return;
    const QVector<int> ids = m_selection->ids().values().toVector();
    int minStart = INT_MAX;
    for (int id : ids)
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id)) minStart = std::min(minStart, c->start);
    frames = std::max(frames, -minStart); // nicht vor Frame 0
    if (frames == 0) return;
    Project* p = m_project;
    p->edit("Nudge", [&](Timeline& tl) {
        TimelineOps::detachTransitions(tl, ids);
        TimelineOps::moveClips(tl, ids, frames, TrackKind::Video, 0, [p] { return p->newClipId(); });
    });
}

void Editor::trimToPlayhead(TimelineOps::Edge edge, int frame)
{
    const Timeline& cur = m_project->timeline();
    QVector<int> ids;
    for (int id : targetIds(frame))
        if (const Clip* c = TimelineOps::findClip(cur, id); c && frame > c->start && frame < c->end()) ids << id;
    if (ids.isEmpty()) return;
    const auto len = sourceLength();
    m_project->edit(edge == TimelineOps::Edge::Start ? T("Anfang trimmen") : T("Ende trimmen"), [&](Timeline& tl) {
        for (int id : ids) {
            const Clip* c = TimelineOps::findClip(tl, id);
            if (!c) continue;
            const int delta = frame - (edge == TimelineOps::Edge::Start ? c->start : c->end());
            TimelineOps::trimClips(tl, {id}, edge, delta, len);
        }
    });
}

void Editor::toggleSelectionEnabled()
{
    if (m_selection->isEmpty()) return;
    const QVector<int> ids = m_selection->ids().values().toVector();
    // Wie DaVinci: ist einer aktiv, werden alle deaktiviert
    bool anyEnabled = false;
    for (int id : ids)
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id)) anyEnabled |= c->enabled;
    modifyClips(ids, anyEnabled ? T("Clip deaktivieren") : T("Clip aktivieren"),
                [anyEnabled](Clip& c) { c.enabled = !anyEnabled; });
}

void Editor::toggleLinkSelection()
{
    const QVector<int> ids = m_selection->ids().values().toVector();
    if (ids.isEmpty()) return;
    bool anyLinked = false;
    for (int id : ids)
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id)) anyLinked |= c->linkId != 0;
    if (!anyLinked && ids.size() < 2) return;
    const int link = anyLinked ? 0 : m_project->newLinkId();
    modifyClips(ids, anyLinked ? T("Verknüpfung lösen") : T("Clips verknüpfen"), [link](Clip& c) { c.linkId = link; });
}

void Editor::toggleMarker(int frame)
{
    m_project->edit("Marker", [&](Timeline& tl) {
        if (tl.markers.contains(frame)) {
            tl.markers.removeAll(frame);
        } else {
            tl.markers << frame;
            std::sort(tl.markers.begin(), tl.markers.end());
        }
    });
}

void Editor::setMarkIn(int frame)
{
    if (frame == m_project->timeline().markIn) return;
    m_project->edit(frame < 0 ? T("In-Punkt entfernen") : T("In-Punkt setzen"), [&](Timeline& tl) {
        tl.markIn = frame;
        if (frame >= 0 && tl.markOut >= 0 && tl.markOut < frame) tl.markOut = -1;
    });
}

void Editor::setMarkOut(int frame)
{
    if (frame == m_project->timeline().markOut) return;
    m_project->edit(frame < 0 ? T("Out-Punkt entfernen") : T("Out-Punkt setzen"), [&](Timeline& tl) {
        tl.markOut = frame;
        if (frame >= 0 && tl.markIn > frame) tl.markIn = -1;
    });
}

void Editor::clearMarks()
{
    const Timeline& t = m_project->timeline();
    if (t.markIn < 0 && t.markOut < 0) return;
    m_project->edit(T("In/Out entfernen"), [](Timeline& tl) { tl.markIn = tl.markOut = -1; });
}

void Editor::copySelection()
{
    const Timeline& tl = m_project->timeline();
    QVector<ClipboardItem> items;
    int minStart = INT_MAX;
    for (int id : m_selection->ids()) {
        TrackRef ref;
        if (const Clip* c = TimelineOps::findClip(tl, id, &ref)) {
            items << ClipboardItem{*c, ref};
            minStart = std::min(minStart, c->start);
        }
    }
    if (items.isEmpty()) return;
    for (auto& it : items) it.clip.start -= minStart;
    m_clipboard = items;
}

void Editor::cutSelection()
{
    copySelection();
    deleteSelection();
}

void Editor::paste(int frame)
{
    if (m_clipboard.isEmpty()) return;
    Project* p = m_project;
    QSet<int> pasted;
    p->edit(T("Einfügen"), [&](Timeline& tl) {
        QHash<int, int> linkMap; // Kopie bekommt eigene Verknüpfung
        for (const auto& it : m_clipboard) {
            Clip c = it.clip;
            c.id = p->newClipId();
            c.start += frame;
            if (c.linkId) {
                if (!linkMap.contains(c.linkId)) linkMap[c.linkId] = p->newLinkId();
                c.linkId = linkMap[c.linkId];
            }
            TimelineOps::ensureTracks(tl, it.ref.kind, it.ref.index + 1);
            TimelineOps::placeClip(tl.track(it.ref), c, [p] { return p->newClipId(); });
            pasted.insert(c.id);
        }
    });
    m_selection->set(pasted);
}

void Editor::setKeyframes(const QVector<int>& ids, const QVector<AnimParam>& params, int frame, bool on)
{
    if (ids.isEmpty() || params.isEmpty()) return;
    m_project->edit(on ? T("Keyframe setzen") : T("Keyframe entfernen"), [&](Timeline& tl) {
        for (int id : ids) {
            Clip* c = TimelineOps::findClip(tl, id);
            if (!c) continue;
            const int t = std::clamp(frame - c->start, 0, c->length() - 1);
            for (AnimParam p : params) {
                if (on) Keys::setKey(*c, p, t, Keys::valueAt(*c, p, t));
                else Keys::removeKey(*c, p, t);
            }
        }
    });
}

void Editor::setKeyframeEase(const QVector<int>& ids, const QVector<AnimParam>& params, int frame, KeyEase ease)
{
    if (ids.isEmpty()) return;
    m_project->edit(T("Keyframe-Verlauf"), [&](Timeline& tl) {
        for (int id : ids)
            if (Clip* c = TimelineOps::findClip(tl, id))
                Keys::setEase(*c, {std::clamp(frame - c->start, 0, c->length() - 1)}, ease, params);
    });
}

void Editor::moveKeyframes(int clipId, const QVector<int>& times, int delta)
{
    if (times.isEmpty() || delta == 0) return;
    m_project->edit(T("Keyframes verschieben"), [&](Timeline& tl) {
        if (Clip* c = TimelineOps::findClip(tl, clipId)) Keys::move(*c, times, delta);
    });
    QSet<int> moved;
    for (int t : times) moved.insert(t + delta);
    m_selection->setKeyframes(clipId, moved);
}

void Editor::removeKeyframes(int clipId, const QVector<int>& times)
{
    if (times.isEmpty()) return;
    m_project->edit(T("Keyframes löschen"), [&](Timeline& tl) {
        if (Clip* c = TimelineOps::findClip(tl, clipId)) Keys::removeAt(*c, times);
    });
    m_selection->setKeyframes(0, {});
}

void Editor::deleteSelection()
{
    if (m_selection->keyClip()) { // ausgewählte Keyframe-Rauten gehen vor (wie DaVinci)
        removeKeyframes(m_selection->keyClip(), m_selection->keyTimes().values().toVector());
        return;
    }
    if (const TransitionKey t = m_selection->transition(); !t.isNull()) {
        removeTransition(t.leftId, t.rightId);
        m_selection->clear();
        return;
    }
    if (m_selection->isEmpty()) return;
    const QVector<int> ids = m_selection->ids().values().toVector();
    // Löschen ohne Ripple: es bleibt eine Lücke, nichts rutscht nach
    m_project->edit(T("Löschen"), [&](Timeline& tl) {
        TimelineOps::detachTransitions(tl, ids);
        for (int id : ids) TimelineOps::removeClip(tl, id);
    });
    m_selection->clear();
}

namespace {

// Gespeicherte Längen der Kanten auf die wirksamen setzen (was nicht passt, wird gekürzt bzw. entfernt)
void fitTransitions(Timeline& tl, const QSet<int>& clipIds, const TimelineOps::SourceLength& len)
{
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (Track& t : tl.tracks(k)) {
            QHash<int, int> in, out;
            for (const auto& s : TimelineOps::transitions(t, len)) {
                if (s.leftId) out[s.leftId] = s.length();
                if (s.rightId) in[s.rightId] = s.length();
            }
            for (Clip& c : t.clips) {
                if (!clipIds.contains(c.id)) continue;
                c.transIn = in.value(c.id);
                c.transOut = out.value(c.id);
            }
        }
}

bool sameTransitions(const Timeline& a, const Timeline& b)
{
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (int i = 0; i < a.tracks(k).size(); ++i)
            for (int j = 0; j < a.tracks(k)[i].clips.size(); ++j) {
                const Clip& x = a.tracks(k)[i].clips[j];
                const Clip& y = b.tracks(k)[i].clips[j];
                if (x.transIn != y.transIn || x.transOut != y.transOut || x.transInStyle != y.transInStyle
                    || x.transOutStyle != y.transOutStyle)
                    return false;
            }
    return true;
}

} // namespace

void Editor::addTransitions(int frame, std::optional<TransitionStyle> style, std::optional<TrackKind> onlyKind)
{
    const Timeline& cur = m_project->timeline();
    // Ausgewählter Übergang: nur die Art tauschen (Ausrichtung bleibt)
    if (const TransitionKey t = m_selection->transition(); style && !t.isNull()) {
        TrackRef ref;
        const Clip* c = TimelineOps::findClip(cur, t.leftId ? t.leftId : t.rightId, &ref);
        if (c && (!onlyKind || ref.kind == *onlyKind)) {
            TransitionStyle s = t.leftId ? c->transOutStyle : c->transInStyle;
            s.type = style->type;
            s.audio = style->audio;
            setTransitionStyle(t.leftId, t.rightId, s);
        }
        return;
    }
    struct EdgeRef { int clipId; bool atEnd; };
    QVector<EdgeRef> edges;
    auto kindOk = [&](int clipId) {
        TrackRef ref;
        return !onlyKind || (TimelineOps::findClip(cur, clipId, &ref) && ref.kind == *onlyKind);
    };
    if (!m_selection->isEmpty()) {
        for (int id : withLinked(m_selection->ids().values().toVector()))
            if (kindOk(id)) edges << EdgeRef{id, false} << EdgeRef{id, true};
    } else {
        // Nächster Schnitt zum Playhead; alle Spuren mit einer Kante genau dort
        const QVector<TrackKind> kinds = onlyKind ? QVector<TrackKind>{*onlyKind}
                                                  : QVector<TrackKind>{TrackKind::Video, TrackKind::Audio};
        int best = -1;
        for (TrackKind k : kinds)
            for (const auto& t : cur.tracks(k))
                for (const auto& c : t.clips)
                    for (int f : {c.start, c.end()})
                        if (best < 0 || std::abs(f - frame) < std::abs(best - frame)) best = f;
        if (best < 0) return;
        for (TrackKind k : kinds)
            for (const auto& t : cur.tracks(k))
                for (const auto& c : t.clips) {
                    if (c.start == best) edges << EdgeRef{c.id, false};
                    if (c.end() == best) edges << EdgeRef{c.id, true};
                }
    }
    if (edges.isEmpty()) return;

    // Auf einer Kopie ansetzen und auf die passende Länge bringen; ändert sich nichts -> kein Undo-Schritt
    Timeline tl = cur;
    const int length = std::max(1, m_project->fps()); // Standard 1 s wie DaVinci
    QSet<int> touched;
    for (const EdgeRef& e : edges) {
        TrackRef ref;
        Clip* c = TimelineOps::findClip(tl, e.clipId, &ref);
        if (!c) continue;
        auto& clips = tl.track(ref).clips;
        const int idx = int(c - clips.data());
        touched.insert(c->id);
        // Neue Übergänge starten als Cross Dissolve (bzw. mit der gewählten Art), vorhandene behalten ihre Art
        auto set = [&style](int& len, TransitionStyle& s, int value) {
            if (len <= 0) s = {};
            if (style) {
                s.type = style->type;
                s.audio = style->audio;
            }
            len = value;
        };
        if (e.atEnd) {
            set(c->transOut, c->transOutStyle, length);
            if (idx + 1 < clips.size() && clips[idx + 1].start == c->end()) {
                set(clips[idx + 1].transIn, clips[idx + 1].transInStyle, length);
                touched.insert(clips[idx + 1].id);
            }
        } else {
            set(c->transIn, c->transInStyle, length);
            if (idx > 0 && clips[idx - 1].end() == c->start) {
                set(clips[idx - 1].transOut, clips[idx - 1].transOutStyle, length);
                touched.insert(clips[idx - 1].id);
            }
        }
    }
    fitTransitions(tl, touched, sourceLength());
    if (sameTransitions(cur, tl)) return;
    m_project->edit(T("Übergang hinzufügen"), [&](Timeline& t) { t = tl; });
}

std::optional<Timeline> Editor::withTransitionAt(int leftId, int rightId, const TransitionStyle& style,
                                                 TrackRef* where) const
{
    Timeline tl = m_project->timeline();
    TrackRef lRef, rRef;
    Clip* l = leftId ? TimelineOps::findClip(tl, leftId, &lRef) : nullptr;
    Clip* r = rightId ? TimelineOps::findClip(tl, rightId, &rRef) : nullptr;
    if ((leftId && !l) || (rightId && !r) || (!l && !r)) return std::nullopt;
    if (l && r && (!(lRef == rRef) || l->end() != r->start)) return std::nullopt; // kein gemeinsamer Schnitt
    const int length = std::max(1, m_project->fps()); // Standard 1 s wie DaVinci
    if (l) l->transOut = length, l->transOutStyle = style;
    if (r) r->transIn = length, r->transInStyle = style;
    if (where) *where = l ? lRef : rRef;
    QSet<int> touched{leftId, rightId};
    touched.remove(0);
    fitTransitions(tl, touched, sourceLength());
    return tl;
}

void Editor::addTransitionAt(int leftId, int rightId, const TransitionStyle& style)
{
    const auto tl = withTransitionAt(leftId, rightId, style);
    if (!tl || sameTransitions(m_project->timeline(), *tl)) return;
    m_project->edit(T("Übergang hinzufügen"), [&](Timeline& t) { t = *tl; });
}

std::optional<TimelineOps::TransitionSpan> Editor::previewTransitionAt(int leftId, int rightId,
                                                                       const TransitionStyle& style) const
{
    TrackRef ref;
    const auto tl = withTransitionAt(leftId, rightId, style, &ref);
    if (!tl) return std::nullopt;
    for (const auto& s : TimelineOps::transitions(tl->track(ref), sourceLength()))
        if (s.leftId == leftId && s.rightId == rightId) return s;
    return std::nullopt; // passt nicht (keine Handles)
}

void Editor::removeTransition(int leftId, int rightId)
{
    const Clip* l = TimelineOps::findClip(m_project->timeline(), leftId);
    const Clip* r = TimelineOps::findClip(m_project->timeline(), rightId);
    if (!(l && l->transOut) && !(r && r->transIn)) return;
    m_project->edit(T("Übergang löschen"), [&](Timeline& tl) {
        if (Clip* l = TimelineOps::findClip(tl, leftId)) l->transOut = 0, l->transOutStyle = {};
        if (Clip* r = TimelineOps::findClip(tl, rightId)) r->transIn = 0, r->transInStyle = {};
    });
}

void Editor::setTransitionStyle(int leftId, int rightId, const TransitionStyle& style, const QString& mergeKey)
{
    Timeline tl = m_project->timeline();
    Clip* l = TimelineOps::findClip(tl, leftId);
    Clip* r = TimelineOps::findClip(tl, rightId);
    if (!l && !r) return;
    if (l) l->transOutStyle = style;
    if (r) r->transInStyle = style;
    // Andere Ausrichtung braucht andere Handles -> Länge ggf. kürzen
    fitTransitions(tl, {leftId, rightId}, sourceLength());
    if (sameTransitions(m_project->timeline(), tl)) return;
    m_project->edit(T("Übergang ändern"), [&](Timeline& t) { t = tl; }, mergeKey);
}

void Editor::setTransitionLength(int leftId, int rightId, int length, const QString& mergeKey)
{
    Timeline tl = m_project->timeline();
    Clip* l = TimelineOps::findClip(tl, leftId);
    Clip* r = TimelineOps::findClip(tl, rightId);
    if (!l && !r) return;
    length = std::max(1, length);
    if (l) l->transOut = length;
    if (r) r->transIn = length;
    fitTransitions(tl, {leftId, rightId}, sourceLength());
    if (sameTransitions(m_project->timeline(), tl)) return;
    m_project->edit(T("Übergangslänge"), [&](Timeline& t) { t = tl; }, mergeKey);
}

QVector<TimelineOps::TransitionSpan> Editor::transitions(TrackRef ref) const
{
    const Timeline& tl = m_project->timeline();
    if (ref.index < 0 || ref.index >= tl.tracks(ref.kind).size()) return {};
    return TimelineOps::transitions(tl.track(ref), sourceLength());
}
