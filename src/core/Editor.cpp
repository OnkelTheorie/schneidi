#include "core/Editor.h"

#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Retime.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"

#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <algorithm>

namespace {

void removeCues(Timeline& tl, const QVector<int>& ids)
{
    for (SubtitleTrack& t : tl.subtitles)
        t.cues.erase(std::remove_if(t.cues.begin(), t.cues.end(), [&](const SubtitleCue& c) { return ids.contains(c.id); }),
                     t.cues.end());
}

} // namespace

QVector<int> Editor::clipIdsOf(const QSet<int>& ids) const
{
    QVector<int> out;
    for (int id : ids)
        if (TimelineOps::findClip(m_project->timeline(), id)) out << id;
    return out;
}

Editor::Editor(Project* project, Selection* selection, QObject* parent)
    : QObject(parent), m_project(project), m_selection(selection) {}

QVector<int> Editor::withLinked(const QVector<int>& ids) const
{
    if (!m_linkedSelection) return editable(ids);
    QVector<int> out;
    for (int id : editable(ids)) // gesperrter Clip nimmt auch seine Partner nicht mit
        for (int g : TimelineOps::linkedGroup(m_project->timeline(), id))
            if (!out.contains(g)) out << g;
    return editable(out); // Partner auf gesperrter Spur bleibt liegen (wie DaVinci)
}

QVector<int> Editor::editable(const QVector<int>& ids) const
{
    return TimelineOps::unlocked(m_project->timeline(), ids);
}

bool Editor::isTrackLocked(TrackRef ref) const
{
    const Timeline& tl = m_project->timeline();
    return ref.index >= 0 && ref.index < tl.tracks(ref.kind).size() && tl.track(ref).locked;
}

void Editor::addMediaAt(const QStringList& paths, int frame, int track)
{
    Project* p = m_project;
    QVector<const MediaInfo*> infos;
    for (const QString& path : paths)
        if (const MediaInfo* info = p->mediaInfo(path); info && info->length > 0) infos << info;
    if (infos.isEmpty()) return;
    // gesperrte Zielspur bekommt nichts (wie DaVinci); fehlende Spuren entstehen ungesperrt
    const int idx0 = std::max(0, track);
    const bool videoOk = !isTrackLocked({TrackKind::Video, idx0});
    const bool audioOk = !isTrackLocked({TrackKind::Audio, idx0});
    infos.erase(std::remove_if(infos.begin(), infos.end(),
                               [&](const MediaInfo* i) { return !(i->hasVideo && videoOk) && !(i->hasAudio && audioOk); }),
                infos.end());
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
            const bool video = info->hasVideo && videoOk;
            // Pro Ton-Stream ein Clip auf der nächsten Spur (wie DaVinci); gesperrte Spuren bekommen nichts
            QVector<int> streams;
            for (int k = 0; k < info->audioStreamCount(); ++k)
                if (k == 0 ? audioOk : !isTrackLocked({TrackKind::Audio, idx + k})) streams << k;
            c.linkId = int(video) + streams.size() > 1 ? p->newLinkId() : 0;
            if (video) {
                TimelineOps::ensureTracks(tl, TrackKind::Video, idx + 1);
                Clip v = c;
                v.id = newId();
                TimelineOps::placeClip(tl.video[idx], v, newId);
            }
            for (int k : streams) {
                TimelineOps::ensureTracks(tl, TrackKind::Audio, idx + k + 1);
                Clip a = c;
                a.id = newId();
                a.audioStream = k;
                TimelineOps::placeClip(tl.audio[idx + k], a, newId);
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
        while (isTrackLocked({TrackKind::Video, track})) ++track; // gesperrte Spur überspringen
    } else if (isTrackLocked({TrackKind::Video, track})) {
        return;
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
    if (ref.kind == TrackKind::Audio) emit mixerOnlyEdit();
    m_project->edit(T("Spur stumm"), [&](Timeline& tl) { tl.track(ref).muted = !tl.track(ref).muted; });
}

void Editor::toggleTrackHidden(TrackRef ref)
{
    m_project->edit(T("Spur ausblenden"), [&](Timeline& tl) { tl.track(ref).hidden = !tl.track(ref).hidden; });
}

void Editor::toggleTrackLock(TrackRef ref)
{
    const Timeline& cur = m_project->timeline();
    if (ref.index < 0 || ref.index >= cur.tracks(ref.kind).size()) return;
    const bool lock = !cur.track(ref).locked;
    // Auswahl auf der Spur aufheben (Clips, Übergang, Keyframe-Rauten), bevor sie gesperrt wird
    if (lock) {
        QSet<int> onTrack;
        for (const Clip& c : cur.track(ref).clips) onTrack.insert(c.id);
        const TransitionKey t = m_selection->transition();
        if (onTrack.contains(t.leftId) || onTrack.contains(t.rightId) || onTrack.contains(m_selection->keyClip())) {
            m_selection->clear();
        } else if (m_selection->ids().intersects(onTrack)) {
            m_selection->set(m_selection->ids() - onTrack);
        }
    }
    m_project->edit(lock ? T("Spur sperren") : T("Spur entsperren"), [&](Timeline& tl) { tl.track(ref).locked = lock; });
}

void Editor::renameTrack(TrackRef ref, const QString& name)
{
    const Timeline& cur = m_project->timeline();
    if (ref.index < 0 || ref.index >= cur.tracks(ref.kind).size()) return;
    // Standardname eingegeben (oder leer) = wieder automatisch benennen
    QString n = name.simplified();
    if (n == trackDisplayName(Track{}, ref)) n.clear();
    if (n == cur.track(ref).name) return;
    m_project->edit(T("Spur umbenennen"), [&](Timeline& tl) { tl.track(ref).name = n; });
}

void Editor::setTrackColor(TrackRef ref, const QString& colorId)
{
    const Timeline& cur = m_project->timeline();
    if (ref.index < 0 || ref.index >= cur.tracks(ref.kind).size() || cur.track(ref).color == colorId) return;
    if (!colorId.isEmpty() && !trackColorInfo(colorId)) return;
    m_project->edit(T("Spurfarbe ändern"), [&](Timeline& tl) { tl.track(ref).color = colorId; });
}

void Editor::moveClips(const QVector<int>& idsIn, int deltaFrames, TrackKind kind, int trackDelta)
{
    const QVector<int> ids = editable(idsIn);
    if (ids.isEmpty() || (deltaFrames == 0 && trackDelta == 0)) return;
    // nie auf eine gesperrte Spur verschieben
    const Timeline& cur = m_project->timeline();
    const int dt = TimelineOps::clampTrackDelta(cur, ids, kind, trackDelta);
    for (TrackRef ref : TimelineOps::tracksOf(cur, ids))
        if (isTrackLocked({ref.kind, ref.index + dt})) return;
    Project* p = m_project;
    p->edit(T("Clips verschieben"), [&](Timeline& tl) {
        TimelineOps::detachTransitions(tl, ids);
        TimelineOps::moveClips(tl, ids, deltaFrames, kind, trackDelta,
                               [p] { return p->newClipId(); });
    });
}

TimelineOps::SourceLength Editor::sourceLength() const
{
    return [p = m_project](const Clip& c) {
        if (c.isCompound()) return p->sequenceLength(c.sequenceId); // Inhalt der Sequenz
        const MediaInfo* m = p->mediaInfo(c.mediaPath);
        return m && !m->isImage ? c.retimedLength(m->length) : 0; // Standbilder beliebig lang ziehbar
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

TimelineOps::TrimEdit Editor::trimEdit(TimelineOps::TrimKind kind, int clipId, TimelineOps::Edge edge) const
{
    using namespace TimelineOps;
    const Timeline& tl = m_project->timeline();
    TrimEdit e;
    e.kind = kind;
    e.edge = edge;
    const Clip* c = findClip(tl, clipId);
    if (!c) return e;
    if (kind != TrimKind::Roll) {
        e.ids = withLinked({clipId});
        return e;
    }
    // Roll: auf jeder Spur der Partner den Clip, der am Schnitt endet, und den, der dort beginnt
    const int cut = edge == Edge::Start ? c->start : c->end();
    TrackRef ref;
    findClip(tl, clipId, &ref);
    QVector<int> seeds = withLinked({clipId});
    for (const Clip& o : tl.track(ref).clips) // Nachbar auf der anderen Seite des Schnitts
        if (o.id != clipId && (edge == Edge::Start ? o.end() == cut : o.start == cut)) seeds += withLinked({o.id});
    QVector<TrackRef> done;
    for (int id : seeds) {
        if (!findClip(tl, id, &ref) || done.contains(ref)) continue;
        done << ref;
        for (const Clip& o : tl.track(ref).clips) {
            if (o.end() == cut) e.ids << o.id;
            if (o.start == cut) e.rightIds << o.id;
        }
    }
    return e;
}

int Editor::clampTrimEdit(const TimelineOps::TrimEdit& e, int delta) const
{
    return TimelineOps::clampTrimEdit(m_project->timeline(), e, delta, sourceLength());
}

Timeline Editor::previewTrimEdit(const TimelineOps::TrimEdit& e, int delta) const
{
    Timeline tl = m_project->timeline();
    TimelineOps::applyTrimEdit(tl, e, delta, sourceLength());
    return tl;
}

void Editor::applyTrimEdit(const TimelineOps::TrimEdit& e, int delta)
{
    using TimelineOps::TrimKind;
    if (e.isNull() || clampTrimEdit(e, delta) == 0) return;
    const QString text = e.kind == TrimKind::Ripple ? T("Ripple-Trimmen")
                         : e.kind == TrimKind::Roll ? T("Schnitt verschieben (Roll)")
                         : e.kind == TrimKind::Slip ? T("Inhalt verschieben (Slip)")
                                                    : T("Clip verschieben (Slide)");
    m_project->edit(text, [&](Timeline& tl) { TimelineOps::applyTrimEdit(tl, e, delta, sourceLength()); });
}

void Editor::slipSelection(int frames)
{
    if (m_selection->isEmpty()) return;
    TimelineOps::TrimEdit e;
    e.kind = TimelineOps::TrimKind::Slip;
    e.ids = withLinked(m_selection->ids().values().toVector());
    applyTrimEdit(e, frames);
}

void Editor::setClipVolume(int clipId, double db)
{
    db = std::clamp(db, kMinVolumeDb, kMaxVolumeDb);
    const Clip* c = TimelineOps::findClip(m_project->timeline(), clipId);
    if (!c || c->volumeDb == db || TimelineOps::isLocked(m_project->timeline(), clipId)) return;
    m_project->edit(T("Lautstärke"), [&](Timeline& tl) {
        if (Clip* clip = TimelineOps::findClip(tl, clipId)) clip->volumeDb = db;
    });
}

void Editor::normalizeAudio(const QHash<int, double>& peakDb, double targetDb, bool relative,
                            std::optional<double> relativeRef)
{
    const Timeline& cur = m_project->timeline();
    QHash<int, double> gain; // neue Clip-Lautstärke (dB)
    double loudest = -1e9;
    for (auto it = peakDb.begin(); it != peakDb.end(); ++it) {
        TrackRef ref;
        const Clip* c = TimelineOps::findClip(cur, it.key(), &ref);
        if (!c || ref.kind != TrackKind::Audio || c->isTitle() || TimelineOps::isLocked(cur, c->id)) continue;
        if (it.value() <= -100.0) continue; // Stille: nichts anzuheben
        gain[c->id] = targetDb - it.value();
        loudest = std::max(loudest, it.value());
    }
    if (gain.isEmpty()) return;
    if (relative && relativeRef && *relativeRef <= -100.0) return; // gemeinsam gemessen: Stille
    if (relative)
        for (double& g : gain) g = targetDb - (relativeRef ? *relativeRef : loudest);
    for (double& g : gain) g = std::clamp(g, kMinVolumeDb, kMaxVolumeDb);
    m_project->edit(T("Audiopegel normalisieren"), [&](Timeline& tl) {
        for (auto it = gain.begin(); it != gain.end(); ++it) {
            Clip* c = TimelineOps::findClip(tl, it.key());
            if (!c) continue;
            if (Keys::animated(*c, AnimParam::Volume)) {
                KeyTrack& keys = c->keys[AnimParam::Volume];
                double top = kMinVolumeDb;
                for (const Keyframe& k : keys) top = std::max(top, k.value);
                const double delta = it.value() - top;
                for (Keyframe& k : keys) k.value = std::clamp(k.value + delta, kMinVolumeDb, kMaxVolumeDb);
            } else {
                c->volumeDb = it.value();
            }
        }
    });
}

QVector<int> Editor::selectedAudioClips() const
{
    const Timeline& tl = m_project->timeline();
    QVector<int> out;
    for (int id : withLinked(m_selection->ids().values().toVector())) {
        TrackRef ref;
        const Clip* c = TimelineOps::findClip(tl, id, &ref);
        if (c && ref.kind == TrackKind::Audio && !c->mediaPath.isEmpty() && !TimelineOps::isLocked(tl, id)) out << id;
    }
    std::sort(out.begin(), out.end());
    return out;
}

void Editor::bladeAt(int clipId, int frame)
{
    Project* p = m_project;
    const QVector<int> ids = withLinked({clipId});
    if (ids.isEmpty()) return;
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
    return editable(ids);
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
    if (!c || TimelineOps::isLocked(m_project->timeline(), clipId)) return;
    const bool in = edge == TimelineOps::Edge::Start;
    // Der andere Fade kann nach dem Kürzen länger als der Clip sein -> Obergrenze nie negativ
    frames = std::clamp(frames, 0, std::max(0, c->length() - (in ? c->fadeOut : c->fadeIn)));
    if (frames == (in ? c->fadeIn : c->fadeOut)) return;
    m_project->edit(in ? T("Einblenden") : T("Ausblenden"), [&](Timeline& tl) {
        if (Clip* x = TimelineOps::findClip(tl, clipId)) (in ? x->fadeIn : x->fadeOut) = frames;
    }, mergeKey);
}

void Editor::modifyClips(const QVector<int>& idsIn, const QString& text, const std::function<void(Clip&)>& fn,
                         const QString& mergeKey)
{
    const QVector<int> ids = editable(idsIn);
    if (ids.isEmpty()) return;
    m_project->edit(text, [&](Timeline& tl) {
        for (int id : ids)
            if (Clip* c = TimelineOps::findClip(tl, id)) fn(*c);
    }, mergeKey);
}

void Editor::addEffect(const QVector<int>& ids, const QString& effectId)
{
    const EffectDescriptor* d = EffectRegistry::find(effectId);
    if (!d) return;
    const Timeline& tl = m_project->timeline();
    QVector<int> targets;
    for (int id : editable(ids)) {
        TrackRef ref;
        const Clip* c = TimelineOps::findClip(tl, id, &ref);
        if (c && ref.kind == TrackKind::Video && !EffectRegistry::has(*c, effectId)) targets << id;
    }
    modifyClips(targets, T("%1 hinzufügen").arg(d->name), [&](Clip& c) { EffectRegistry::add(c, effectId); });
}

void Editor::removeEffect(const QVector<int>& ids, const QString& effectId)
{
    const EffectDescriptor* d = EffectRegistry::find(effectId);
    if (!d) return;
    QVector<int> targets;
    for (int id : editable(ids))
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id); c && EffectRegistry::has(*c, effectId))
            targets << id;
    modifyClips(targets, T("%1 entfernen").arg(d->name), [&](Clip& c) { EffectRegistry::remove(c, effectId); });
}

QVector<int> Editor::effectTargets(int frame) const
{
    const Timeline& tl = m_project->timeline();
    QVector<int> ids;
    for (int id : m_selection->ids()) {
        TrackRef ref;
        if (TimelineOps::findClip(tl, id, &ref) && ref.kind == TrackKind::Video) ids << id;
    }
    if (!ids.isEmpty()) return ids;
    // Ohne Auswahl: sichtbarer (oberster) Videoclip unter dem Playhead
    for (int i = tl.video.size() - 1; i >= 0; --i)
        if (!tl.video[i].locked)
            for (const Clip& c : tl.video[i].clips)
                if (frame >= c.start && frame < c.end()) return {c.id};
    return {};
}

namespace {
constexpr const char* kGrade = "grade"; // Effekt der Color-Seite (EffectRegistry)

QVector<AnimParam> gradeParams()
{
    QVector<AnimParam> list;
    if (const EffectDescriptor* d = EffectRegistry::find(kGrade))
        for (const EffectParam& p : d->params)
            if (p.anim != AnimParam::Count) list << p.anim;
    return list;
}

// Standardwert eines Grade-Parameters
double gradeDefault(AnimParam a)
{
    const EffectParam* p = nullptr;
    return EffectRegistry::paramFor(a, nullptr, &p) ? p->defaultValue.toDouble() : 0.0;
}

// Nur Videoclips (Color-Seite wirkt aufs Bild)
QVector<int> videoOnly(const Timeline& tl, const QVector<int>& ids)
{
    QVector<int> out;
    for (int id : ids) {
        TrackRef ref;
        if (TimelineOps::findClip(tl, id, &ref) && ref.kind == TrackKind::Video) out << id;
    }
    return out;
}

int localFrame(const Clip& c, int frame) { return std::clamp(frame - c.start, 0, std::max(0, c.length() - 1)); }
} // namespace

void Editor::setGradeValues(const QVector<int>& ids, const QVector<QPair<AnimParam, double>>& values, int frame,
                            const QString& text, const QString& mergeKey)
{
    modifyClips(videoOnly(m_project->timeline(), ids), text, [&](Clip& c) {
        EffectRegistry::add(c, kGrade); // schon vorhanden: nichts
        for (const auto& [p, v] : values) Keys::setValue(c, p, localFrame(c, frame), v);
    }, mergeKey);
}

void Editor::resetGrade(const QVector<int>& ids, const QVector<AnimParam>& params, int frame, const QString& text)
{
    QVector<int> targets;
    for (int id : videoOnly(m_project->timeline(), ids))
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id); c && EffectRegistry::has(*c, kGrade))
            targets << id;
    modifyClips(targets, text, [&](Clip& c) {
        if (params.isEmpty()) {
            EffectRegistry::remove(c, kGrade);
            return;
        }
        for (AnimParam p : params) Keys::setValue(c, p, localFrame(c, frame), gradeDefault(p));
    });
}

void Editor::setGradeLut(const QVector<int>& ids, const QString& path)
{
    modifyClips(videoOnly(m_project->timeline(), ids), path.isEmpty() ? T("LUT entfernen") : T("LUT laden"),
                [&](Clip& c) {
                    EffectRegistry::add(c, kGrade);
                    EffectRegistry::instance(c, kGrade)->params["lut"] = path;
                });
}

void Editor::setGradeEnabled(const QVector<int>& ids, bool on)
{
    modifyClips(videoOnly(m_project->timeline(), ids), on ? T("Farbkorrektur an") : T("Farbkorrektur aus"),
                [&](Clip& c) {
                    if (EffectInstance* e = EffectRegistry::instance(c, kGrade)) e->enabled = on;
                });
}

void Editor::setGradeKeyframe(const QVector<int>& ids, int frame, bool on)
{
    const QVector<AnimParam> params = gradeParams();
    modifyClips(videoOnly(m_project->timeline(), ids), on ? T("Keyframe setzen") : T("Keyframe entfernen"),
                [&](Clip& c) {
                    EffectRegistry::add(c, kGrade);
                    const int t = localFrame(c, frame);
                    for (AnimParam p : params) {
                        if (on) Keys::setKey(c, p, t, Keys::valueAt(c, p, t));
                        else Keys::removeKey(c, p, t);
                    }
                });
}

void Editor::rippleDeleteSelection()
{
    const QVector<int> cues = selectedSubtitles();
    const QVector<int> ids = editable(clipIdsOf(m_selection->ids()));
    if (ids.isEmpty() && cues.isEmpty()) return;
    m_project->edit(T("Löschen mit Ripple"), [&](Timeline& tl) {
        removeCues(tl, cues); // Untertitel: ohne Ripple (Lücke bleibt)
        if (ids.isEmpty()) return;
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
            if (!t.locked) // gesperrte Spuren lassen sich nicht auswählen
                for (const auto& c : t.clips) ids.insert(c.id);
    m_selection->set(ids);
}

void Editor::nudgeSelection(int frames)
{
    const QVector<int> ids = editable(m_selection->ids().values().toVector());
    if (ids.isEmpty()) return;
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
    const QVector<int> ids = editable(m_selection->ids().values().toVector());
    if (ids.isEmpty()) return;
    // Wie DaVinci: ist einer aktiv, werden alle deaktiviert
    bool anyEnabled = false;
    for (int id : ids)
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id)) anyEnabled |= c->enabled;
    modifyClips(ids, anyEnabled ? T("Clip deaktivieren") : T("Clip aktivieren"),
                [anyEnabled](Clip& c) { c.enabled = !anyEnabled; });
}

namespace {
// Videoclip mit Medium (Titel und Audioclips werden nicht gecacht)
bool cacheable(const Timeline& tl, int id)
{
    TrackRef ref;
    const Clip* c = TimelineOps::findClip(tl, id, &ref);
    return c && ref.kind == TrackKind::Video && !c->isTitle() && !c->mediaPath.isEmpty();
}
} // namespace

int Editor::selectionRenderCacheState() const
{
    int on = 0, n = 0;
    for (int id : m_selection->ids())
        if (cacheable(m_project->timeline(), id)) {
            ++n;
            on += TimelineOps::findClip(m_project->timeline(), id)->renderCache ? 1 : 0;
        }
    if (n == 0) return -1;
    return on == n ? 1 : on == 0 ? 0 : 2;
}

void Editor::toggleSelectionRenderCache()
{
    QVector<int> ids;
    for (int id : editable(m_selection->ids().values().toVector()))
        if (cacheable(m_project->timeline(), id)) ids << id;
    if (ids.isEmpty()) return;
    const bool on = selectionRenderCacheState() != 1;
    modifyClips(ids, on ? T("Render-Cache an") : T("Render-Cache aus"), [on](Clip& c) { c.renderCache = on; });
}

void Editor::toggleLinkSelection()
{
    const QVector<int> ids = editable(m_selection->ids().values().toVector());
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

void Editor::setTargetTracks(int video, int audio)
{
    video = std::max(0, video);
    audio = std::max(0, audio);
    if (video == m_targetVideo && audio == m_targetAudio) return;
    m_targetVideo = video;
    m_targetAudio = audio;
    emit targetTracksChanged();
}

void Editor::setSourceMarkIn(const QString& path, int frame)
{
    const MediaInfo* m = m_project->mediaInfo(path);
    if (!m) return;
    const int out = frame >= 0 && m->markOut >= 0 && m->markOut < frame ? -1 : m->markOut;
    m_project->setMediaMarks(path, frame, out);
}

void Editor::setSourceMarkOut(const QString& path, int frame)
{
    const MediaInfo* m = m_project->mediaInfo(path);
    if (!m) return;
    const int in = frame >= 0 && m->markIn > frame ? -1 : m->markIn;
    m_project->setMediaMarks(path, in, frame);
}

void Editor::clearSourceMarks(const QString& path)
{
    m_project->setMediaMarks(path, -1, -1);
}

void Editor::placeSource(Timeline& tl, const MediaInfo& m, int sIn, int len, int start, int vTrack, int aTrack,
                         double speed)
{
    Project* p = m_project;
    auto newId = [p] { return p->newClipId(); };
    Clip c;
    c.mediaPath = m.path;
    c.start = start;
    c.in = sIn;
    c.out = sIn + len - 1;
    c.speed = speed;
    // Ziele: Bild, dann pro Ton-Stream eine Spur ab aTrack (weitere Streams auf gesperrten Spuren entfallen)
    struct Target { TrackKind kind; int idx; int stream; };
    QVector<Target> targets;
    if (m.hasVideo) targets << Target{TrackKind::Video, vTrack, 0};
    if (m.hasAudio)
        for (int k = 0; k < m.audioStreamCount(); ++k)
            if (k == 0 || !isTrackLocked({TrackKind::Audio, aTrack + k})) targets << Target{TrackKind::Audio, aTrack + k, k};
    c.linkId = targets.size() > 1 ? p->newLinkId() : 0;
    for (const Target& tg : targets) {
        TimelineOps::ensureTracks(tl, tg.kind, tg.idx + 1);
        Track& t = tl.tracks(tg.kind)[tg.idx];
        Clip x = c;
        x.id = newId();
        x.audioStream = tg.stream;
        TimelineOps::clearDissolveAt(t, x.start);
        TimelineOps::clearDissolveAt(t, x.end());
        TimelineOps::placeClip(t, x, newId);
    }
}

int Editor::sourceEdit(SourceEditMode mode, const QString& path, int srcPos, int playhead)
{
    using M = SourceEditMode;
    using namespace TimelineOps;
    Project* p = m_project;
    const MediaInfo* info = p->mediaInfo(path);
    if (!info || info->length <= 0 || (!info->hasVideo && !info->hasAudio)) return -1;
    const MediaInfo m = *info;
    const Timeline& cur = p->timeline();

    // Quellbereich: Quell-In/Out, fehlt einer -> Clipanfang bzw. -ende (wie DaVinci)
    const int last = m.length - 1;
    int sIn = std::clamp(m.markIn >= 0 ? m.markIn : 0, 0, last);
    const int sOut = std::clamp(m.markOut >= 0 ? m.markOut : last, 0, last);
    if (sOut < sIn) return -1;
    int len = sOut - sIn + 1;
    int start = std::max(0, playhead);
    int vTrack = m_targetVideo, aTrack = m_targetAudio;
    bool usedTimelineMarks = false;
    QVector<TrackRef> rippleTracks; // Insert / Ripple Overwrite
    int rippleFrom = 0, rippleDelta = 0;
    int replaceEnd = 0;             // Ripple Overwrite: Ende des ersetzten Clips
    double speed = 1.0;             // Fit to Fill

    // Clip unter dem Playhead auf der Zielspur (Video, bei reinem Ton Audio) samt Spuren seiner Partner
    auto clipUnderPlayhead = [&]() -> const Clip* {
        const TrackRef ref{m.hasVideo ? TrackKind::Video : TrackKind::Audio, m.hasVideo ? m_targetVideo : m_targetAudio};
        if (ref.index >= cur.tracks(ref.kind).size()) return nullptr;
        for (const Clip& c : cur.track(ref).clips)
            if (c.start <= playhead && playhead < c.end()) return &c;
        return nullptr;
    };
    auto partnerTracks = [&](const Clip& c) {
        QVector<TrackRef> refs;
        int firstAudio = INT_MAX; // mehrere Ton-Streams: Stream 0 kommt auf die oberste Audiospur der Gruppe
        for (int id : linkedGroup(cur, c.id)) {
            TrackRef r;
            if (!findClip(cur, id, &r) || refs.contains(r)) continue;
            refs << r;
            if (r.kind == TrackKind::Video) vTrack = r.index;
            else firstAudio = std::min(firstAudio, r.index);
        }
        if (firstAudio != INT_MAX) aTrack = firstAudio;
        return refs;
    };

    switch (mode) {
    case M::Insert:
    case M::Overwrite:
    case M::PlaceOnTop: {
        // Timeline-In/Out: In = Ziel, In+Out begrenzt die Länge, nur Out = rückwärts ab Out
        const int tIn = cur.markIn, tOut = cur.markOut;
        if (tIn >= 0 && tOut >= tIn) {
            start = tIn;
            len = std::min(len, tOut - tIn + 1);
        } else if (tIn >= 0) {
            start = tIn;
        } else if (tOut >= 0) {
            start = tOut + 1 - len;
            if (start < 0) { // vorne kürzen, das Ende bleibt am Out
                sIn -= start;
                len += start;
                start = 0;
            }
        }
        usedTimelineMarks = tIn >= 0 || tOut >= 0;
        if (mode == M::PlaceOnTop) {
            // Erste Spur über allen Clips im Bereich (fehlende Spur wird angelegt)
            auto freeTrack = [&](TrackKind kind) {
                int idx = 0;
                const auto& tracks = cur.tracks(kind);
                for (int i = 0; i < tracks.size(); ++i)
                    for (const Clip& c : tracks[i].clips)
                        if (c.start < start + len && c.end() > start) idx = i + 1;
                while (idx < tracks.size() && tracks[idx].locked) ++idx; // gesperrte Spur überspringen
                return idx;
            };
            vTrack = freeTrack(TrackKind::Video);
            aTrack = freeTrack(TrackKind::Audio);
        }
        if (mode == M::Insert) {
            // Wie DaVinci: alle nicht gesperrten Spuren rücken mit (bleiben synchron), die Zielspuren sowieso
            if (m.hasVideo) rippleTracks << TrackRef{TrackKind::Video, vTrack};
            for (int k = 0; k < m.audioStreamCount(); ++k) rippleTracks << TrackRef{TrackKind::Audio, aTrack + k};
            for (TrackKind kind : {TrackKind::Video, TrackKind::Audio})
                for (int i = 0; i < cur.tracks(kind).size(); ++i)
                    if (!cur.tracks(kind)[i].locked && !rippleTracks.contains(TrackRef{kind, i})) rippleTracks << TrackRef{kind, i};
        }
        break;
    }
    case M::AppendAtEnd:
        start = endFrame(cur);
        break;
    case M::FitToFill: {
        // 4-Punkt-Schnitt wie DaVinci: braucht Timeline-In und -Out; Geschwindigkeit = Quelllänge / Ziellänge
        const int tIn = cur.markIn, tOut = cur.markOut;
        if (tIn < 0 || tOut < tIn) return -1;
        const int target = tOut - tIn + 1;
        speed = double(len) / target;
        if (std::abs(speed - 1.0) < 1e-9) speed = 1.0;
        // in/out zählen im umgerechneten Material (Clip::speed)
        const int avail = std::max(1, int(m.length / speed));
        if (avail < target) return -1;
        sIn = std::clamp(int(std::lround(sIn / speed)), 0, avail - target);
        len = target;
        start = tIn;
        usedTimelineMarks = true;
        break;
    }
    case M::Replace: {
        // Länge und Lage bleiben; Quell-In (sonst Quell-Playhead) deckt sich mit dem Timeline-Playhead
        const Clip* c = clipUnderPlayhead();
        if (!c) return -1;
        partnerTracks(*c);
        const int anchor = m.markIn >= 0 ? m.markIn : srcPos;
        sIn = anchor - (playhead - c->start);
        len = c->length();
        if (sIn < 0 || sIn + len - 1 > last) return -1; // zu wenig Material
        start = c->start;
        break;
    }
    case M::RippleOverwrite: {
        const Clip* c = clipUnderPlayhead();
        if (!c) return -1;
        rippleTracks = partnerTracks(*c);
        start = c->start;
        replaceEnd = c->end();
        rippleFrom = c->end();
        rippleDelta = len - c->length();
        break;
    }
    }
    if (len <= 0) return -1;
    // Gesperrte Zielspur: dieser Teil entfällt (wie beim Ablegen), gesperrte Spuren rücken nie
    MediaInfo m2 = m;
    if (m2.hasVideo && isTrackLocked(TrackRef{TrackKind::Video, vTrack})) m2.hasVideo = false;
    if (m2.hasAudio && isTrackLocked(TrackRef{TrackKind::Audio, aTrack})) m2.hasAudio = false;
    if (!m2.hasVideo && !m2.hasAudio) return -1;
    rippleTracks.erase(std::remove_if(rippleTracks.begin(), rippleTracks.end(), [&](const TrackRef& r) { return isTrackLocked(r); }),
                       rippleTracks.end());
    if (m2.hasVideo && !rippleTracks.contains(TrackRef{TrackKind::Video, vTrack}) && mode == M::RippleOverwrite)
        rippleTracks << TrackRef{TrackKind::Video, vTrack};
    if (m2.hasAudio && mode == M::RippleOverwrite)
        for (int k = 0; k < m2.audioStreamCount(); ++k)
            if (const TrackRef r{TrackKind::Audio, aTrack + k}; !rippleTracks.contains(r) && !isTrackLocked(r)) rippleTracks << r;

    QString text;
    switch (mode) {
    case M::Insert: text = T("Clip einfügen: %1"); break;
    case M::Overwrite: text = T("Clip überschreiben: %1"); break;
    case M::Replace: text = T("Clip ersetzen: %1"); break;
    case M::PlaceOnTop: text = T("Oben platzieren: %1"); break;
    case M::RippleOverwrite: text = T("Ripple-Überschreiben: %1"); break;
    case M::AppendAtEnd: text = T("Am Ende anhängen: %1"); break;
    case M::FitToFill: text = T("Einpassen: %1"); break;
    }
    p->edit(text.arg(m.name), [&](Timeline& tl) {
        auto newId = [p] { return p->newClipId(); };
        for (const TrackRef& r : rippleTracks) ensureTracks(tl, r.kind, r.index + 1);
        if (mode == M::Insert) insertGap(tl, rippleTracks, start, len, newId, [p] { return p->newLinkId(); });
        if (mode == M::RippleOverwrite) {
            for (const TrackRef& r : rippleTracks) {
                clearRange(tl.track(r), start, replaceEnd, newId);
                shiftFrom(tl.track(r), rippleFrom, rippleDelta);
            }
            // übrige nicht gesperrte Spuren bleiben synchron (bleiben stehen, falls sie überschreiben würden)
            TimelineOps::rippleTracks(tl, {{rippleFrom, rippleDelta}}, rippleTracks);
        }
        placeSource(tl, m2, sIn, len, start, vTrack, aTrack, speed);
        if (usedTimelineMarks) tl.markIn = tl.markOut = -1; // wie DaVinci: benutzte Marken sind verbraucht
    });
    return mode == M::Replace ? playhead : start + len;
}

void Editor::placeSourceRange(const QString& path, int in, int out, int frame, int track)
{
    const MediaInfo* info = m_project->mediaInfo(path);
    if (!info || info->length <= 0) return;
    const MediaInfo m = *info;
    in = std::clamp(in, 0, m.length - 1);
    out = std::clamp(out, in, m.length - 1);
    MediaInfo m2 = m; // gesperrte Spur: dieser Teil entfällt
    track = std::max(0, track);
    if (m2.hasVideo && isTrackLocked(TrackRef{TrackKind::Video, track})) m2.hasVideo = false;
    if (m2.hasAudio && isTrackLocked(TrackRef{TrackKind::Audio, track})) m2.hasAudio = false;
    if (!m2.hasVideo && !m2.hasAudio) return;
    m_project->edit(T("Clip überschreiben: %1").arg(m.name), [&](Timeline& tl) {
        placeSource(tl, m2, in, out - in + 1, std::max(0, frame), track, track);
    });
}

void Editor::copySelection()
{
    // Überblendung zu einem nicht mitkopierten Clip gehört nicht dazu (sonst blendet die Kopie über Schwarz aus)
    Timeline tl = m_project->timeline();
    TimelineOps::detachTransitions(tl, clipIdsOf(m_selection->ids()));
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
    // Nur löschen, was auch in die Zwischenablage kommt (Clips): ausgewählte Untertitel/Keyframes/Übergänge gingen
    // sonst verloren, und Einfügen brachte die alte Zwischenablage
    const QVector<int> ids = editable(clipIdsOf(m_selection->ids()));
    if (ids.isEmpty()) return;
    copySelection();
    m_project->edit(T("Ausschneiden"), [&](Timeline& tl) {
        TimelineOps::detachTransitions(tl, ids);
        for (int id : ids) TimelineOps::removeClip(tl, id);
    });
    m_selection->clear();
}

void Editor::paste(int frame)
{
    if (m_clipboard.isEmpty()) return;
    // nicht auf gesperrte Spuren einfügen
    QVector<ClipboardItem> items;
    for (const auto& it : m_clipboard)
        if (!isTrackLocked(it.ref)
            && (!it.clip.isCompound() || m_project->canNest(it.clip.sequenceId, m_project->currentSequence())))
            items << it; // Compound Clip nie in sich selbst (Schleife)
    if (items.isEmpty()) return;
    Project* p = m_project;
    QSet<int> pasted;
    p->edit(T("Einfügen"), [&](Timeline& tl) {
        QHash<int, int> linkMap; // Kopie bekommt eigene Verknüpfung
        for (const auto& it : items) {
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

void Editor::setKeyframes(const QVector<int>& idsIn, const QVector<AnimParam>& params, int frame, bool on)
{
    const QVector<int> ids = editable(idsIn);
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

void Editor::setKeyframeEase(const QVector<int>& idsIn, const QVector<AnimParam>& params, int frame, KeyEase ease)
{
    const QVector<int> ids = editable(idsIn);
    if (ids.isEmpty()) return;
    m_project->edit(T("Keyframe-Verlauf"), [&](Timeline& tl) {
        for (int id : ids)
            if (Clip* c = TimelineOps::findClip(tl, id))
                Keys::setEase(*c, {std::clamp(frame - c->start, 0, c->length() - 1)}, ease, params);
    });
}

void Editor::moveKeyframes(int clipId, const QVector<int>& times, int delta)
{
    if (times.isEmpty() || delta == 0 || TimelineOps::isLocked(m_project->timeline(), clipId)) return;
    m_project->edit(T("Keyframes verschieben"), [&](Timeline& tl) {
        if (Clip* c = TimelineOps::findClip(tl, clipId)) Keys::move(*c, times, delta);
    });
    QSet<int> moved;
    for (int t : times) moved.insert(t + delta);
    m_selection->setKeyframes(clipId, moved);
}

void Editor::removeKeyframes(int clipId, const QVector<int>& times, const QVector<AnimParam>& params)
{
    if (times.isEmpty() || TimelineOps::isLocked(m_project->timeline(), clipId)) return;
    m_project->edit(T("Keyframes löschen"), [&](Timeline& tl) {
        if (Clip* c = TimelineOps::findClip(tl, clipId)) Keys::removeAt(*c, times, params);
    });
    m_selection->setKeyframes(0, {});
}

void Editor::deleteSelection()
{
    if (m_selection->keyClip()) { // ausgewählte Keyframe-Rauten gehen vor (wie DaVinci)
        const int param = m_selection->keyParam();
        removeKeyframes(m_selection->keyClip(), m_selection->keyTimes().values().toVector(),
                        param < 0 ? QVector<AnimParam>{} : QVector<AnimParam>{AnimParam(param)});
        return;
    }
    if (const TransitionKey t = m_selection->transition(); !t.isNull()) {
        removeTransition(t.leftId, t.rightId);
        m_selection->clear();
        return;
    }
    const QVector<int> cues = selectedSubtitles();
    const QVector<int> ids = editable(clipIdsOf(m_selection->ids()));
    if (ids.isEmpty() && cues.isEmpty()) return;
    // Löschen ohne Ripple: es bleibt eine Lücke, nichts rutscht nach
    m_project->edit(T("Löschen"), [&](Timeline& tl) {
        removeCues(tl, cues);
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
                    || x.transOutStyle != y.transOutStyle || x.transInAlone != y.transInAlone
                    || x.transOutAlone != y.transOutAlone)
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
    auto isLocked = [&](int clipId) { return TimelineOps::isLocked(cur, clipId); };
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
                        if (!t.locked && (best < 0 || std::abs(f - frame) < std::abs(best - frame))) best = f;
        if (best < 0) return;
        for (TrackKind k : kinds)
            for (const auto& t : cur.tracks(k))
                for (const auto& c : t.clips) {
                    if (isLocked(c.id)) continue;
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
                c->transOutAlone = clips[idx + 1].transInAlone = false; // am Schnitt gewollt: Überblendung
                touched.insert(clips[idx + 1].id);
            }
        } else {
            set(c->transIn, c->transInStyle, length);
            if (idx > 0 && clips[idx - 1].end() == c->start) {
                set(clips[idx - 1].transOut, clips[idx - 1].transOutStyle, length);
                c->transInAlone = clips[idx - 1].transOutAlone = false;
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
    if ((l && tl.track(lRef).locked) || (r && tl.track(rRef).locked)) return std::nullopt; // gesperrte Spur
    if (l && r && (!(lRef == rRef) || l->end() != r->start)) return std::nullopt; // kein gemeinsamer Schnitt
    const int length = std::max(1, m_project->fps()); // Standard 1 s wie DaVinci
    if (l) l->transOut = length, l->transOutStyle = style;
    if (r) r->transIn = length, r->transInStyle = style;
    if (l && r) l->transOutAlone = r->transInAlone = false; // auf den Schnitt gezogen: Überblendung
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
    if (TimelineOps::isLocked(m_project->timeline(), leftId ? leftId : rightId)) return;
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
    if ((!l && !r) || TimelineOps::isLocked(tl, leftId ? leftId : rightId)) return;
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
    if ((!l && !r) || TimelineOps::isLocked(tl, leftId ? leftId : rightId)) return;
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

void Editor::setClipSpeed(const QVector<int>& ids, const Retime& r, bool ripple)
{
    const QVector<int> all = withLinked(ids);
    if (all.isEmpty() || r.speed <= 0) return;
    m_project->edit(T("Geschwindigkeit ändern"), [&](Timeline& tl) {
        QSet<int> touched;
        const QVector<TrackRef> edited = TimelineOps::tracksOf(tl, all);
        QVector<QPair<int, int>> shifts; // Ripple für die übrigen Spuren (ab altem Ende, Längenänderung)
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (Track& t : tl.tracks(k)) {
                // von hinten, damit Ripple-Verschiebungen sich nicht gegenseitig verfälschen
                for (int i = t.clips.size() - 1; i >= 0; --i) {
                    Clip& c = t.clips[i];
                    if (!all.contains(c.id) || c.isTitle()) continue;
                    const MediaInfo* m = m_project->mediaInfo(c.mediaPath);
                    if (!m || m->isImage || m->length <= 0) continue;
                    const int L = m->length;
                    // Konstantes Tempo ersetzt eine Speed Ramp (wie DaVinci „Change Clip Speed“)
                    Clip n = c;
                    n.speed = r.speed;
                    n.reverse = r.reverse;
                    n.freeze = r.freeze;
                    n.ramp.clear();
                    const RetimeMap oldMap(c, L), newMap(n, L);
                    // Ausschnitt in Datei-Frames (a = Anfang des Clips, b = Ende, bei rückwärts a > b)
                    const double a = oldMap.fileFrameAt(c.in);
                    const double b = oldMap.fileFrameAt(c.out);
                    const bool flip = r.reverse != c.reverse;
                    // Neuer Anfang: gleiches Datei-Frame; bei Richtungswechsel dasselbe Stück umgekehrt
                    const double startFile = flip ? b : a;
                    const int avail = std::max(1, int(L / r.speed));
                    const int newIn = std::clamp(int(std::lround(newMap.materialAtFile(startFile))), 0, avail - 1);
                    const int oldLen = c.length();
                    int newLen = oldLen;
                    if (ripple && !r.freeze && !c.freeze) {
                        // Quell-Spanne des Clips (bei Speed Ramp nicht einfach Länge · Tempo)
                        const double span = c.hasRamp() ? oldMap.sourceAt(c.out + 1) - oldMap.sourceAt(c.in)
                                                        : oldLen * c.speed;
                        newLen = std::max(1, int(std::lround(span / r.speed)));
                    }
                    if (!r.freeze) newLen = std::min(newLen, avail - newIn);
                    // Keyframes über die Datei-Frames umrechnen
                    for (auto it = c.keys.begin(); it != c.keys.end(); ++it) {
                        KeyTrack mapped = it.value(), moved;
                        for (Keyframe& kf : mapped)
                            kf.frame = int(std::lround(newMap.materialAtFile(oldMap.fileFrameAt(kf.frame))));
                        Keys::rescaleHandles(it.value(), mapped);
                        for (const Keyframe& kf : mapped)
                            if (std::none_of(moved.begin(), moved.end(), [&](const Keyframe& o) { return o.frame == kf.frame; }))
                                moved << kf;
                        std::sort(moved.begin(), moved.end(),
                                  [](const Keyframe& x, const Keyframe& y) { return x.frame < y.frame; });
                        it.value() = moved;
                    }
                    const int oldEnd = c.end();
                    c.in = newIn;
                    c.out = newIn + newLen - 1;
                    c.speed = r.speed;
                    c.reverse = r.reverse;
                    c.freeze = r.freeze;
                    c.freezeFrame = r.freeze ? newIn : -1; // Bild am Clipanfang (bleibt beim Teilen/Trimmen)
                    c.keepPitch = r.keepPitch;
                    c.ramp.clear();
                    c.fadeIn = std::min(c.fadeIn, newLen);
                    c.fadeOut = std::min(c.fadeOut, newLen);
                    touched.insert(c.id);
                    if (const int delta = newLen - oldLen; ripple && delta != 0) {
                        for (int j = i + 1; j < t.clips.size(); ++j)
                            if (t.clips[j].start >= oldEnd) t.clips[j].start += delta;
                        // verknüpfte Partner liefern denselben Versatz -> nur einmal
                        if (!shifts.contains(qMakePair(oldEnd, delta))) shifts << qMakePair(oldEnd, delta);
                    }
                }
            }
        TimelineOps::rippleTracks(tl, shifts, edited);
        fitTransitions(tl, touched, sourceLength());
    });
}

// ---- Speed Ramp (Retime Controls) ----

bool Editor::canRetime(int clipId) const
{
    const Clip* c = TimelineOps::findClip(m_project->timeline(), clipId);
    if (!c || c->isTitle() || c->isCompound() || c->freeze) return false;
    const MediaInfo* m = m_project->mediaInfo(c->mediaPath);
    return m && !m->isImage && m->length > 0;
}

void Editor::editRamp(const QString& text, int clipId, const std::function<bool(Clip&, const RetimeMap&, int)>& change)
{
    if (!canRetime(clipId) || !editable({clipId}).contains(clipId)) return;
    const Timeline& cur = m_project->timeline();
    const Clip* first = TimelineOps::findClip(cur, clipId);
    const MediaInfo* m = m_project->mediaInfo(first->mediaPath);
    const int L = m->length;
    // Partner derselben Datei machen mit (Video + Ton), fremde Partner (anderer Ton) nicht
    QVector<int> ids;
    for (int id : withLinked({clipId}))
        if (const Clip* c = TimelineOps::findClip(cur, id); c && c->mediaPath == first->mediaPath && !c->freeze)
            ids << id;
    // Erst am Hauptclip prüfen, ob sich etwas ändert (kein leerer Undo-Schritt)
    {
        Clip probe = *first;
        if (!change(probe, RetimeMap(*first, L), L)) return;
    }
    Timeline tl = cur;
    QSet<int> touched;
    QVector<QPair<int, int>> shifts;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (Track& t : tl.tracks(k))
            for (int i = t.clips.size() - 1; i >= 0; --i) {
                Clip& c = t.clips[i];
                if (!ids.contains(c.id)) continue;
                const RetimeMap oldMap(c, L);
                // Quellstellen merken: Anfang, Ende (exklusiv), Keyframes
                const double sIn = oldMap.sourceAt(c.in), sEnd = oldMap.sourceAt(c.out + 1);
                QMap<AnimParam, QVector<double>> keySrc;
                for (auto it = c.keys.cbegin(); it != c.keys.cend(); ++it)
                    for (const Keyframe& kf : it.value()) keySrc[it.key()] << oldMap.sourceAt(kf.frame);
                Clip n = c;
                if (!change(n, oldMap, L)) continue;
                ::Retime::normalize(n.ramp, L);
                const RetimeMap newMap(n, L);
                const int avail = std::max(1, newMap.length());
                n.in = std::clamp(int(std::lround(newMap.materialAt(sIn))), 0, avail - 1);
                const int len = std::clamp(int(std::lround(newMap.materialAt(sEnd))) - n.in, 1, avail - n.in);
                n.out = n.in + len - 1;
                for (auto it = n.keys.begin(); it != n.keys.end(); ++it) {
                    KeyTrack mapped = it.value(), moved;
                    const QVector<double>& src = keySrc[it.key()];
                    mapped.resize(std::min(mapped.size(), src.size()));
                    for (int j = 0; j < mapped.size(); ++j) mapped[j].frame = int(std::lround(newMap.materialAt(src[j])));
                    Keys::rescaleHandles(it.value(), mapped);
                    for (const Keyframe& kf : mapped)
                        if (moved.isEmpty() || moved.last().frame != kf.frame) moved << kf;
                    it.value() = moved;
                }
                n.fadeIn = std::min(n.fadeIn, len);
                n.fadeOut = std::min(n.fadeOut, len);
                const int oldEnd = c.end();
                const int delta = n.length() - c.length();
                c = n;
                touched.insert(c.id);
                if (delta != 0) {
                    for (int j = i + 1; j < t.clips.size(); ++j)
                        if (t.clips[j].start >= oldEnd) t.clips[j].start += delta;
                    if (!shifts.contains(qMakePair(oldEnd, delta))) shifts << qMakePair(oldEnd, delta);
                }
            }
    if (touched.isEmpty()) return;
    TimelineOps::rippleTracks(tl, shifts, TimelineOps::tracksOf(tl, ids));
    fitTransitions(tl, touched, sourceLength());
    m_project->edit(text, [&](Timeline& t) { t = tl; });
}

void Editor::addSpeedPoint(int clipId, int frame)
{
    const Clip* c = TimelineOps::findClip(m_project->timeline(), clipId);
    if (!c || frame <= c->start || frame >= c->end()) return;
    const int m = c->in + frame - c->start;
    editRamp(T("Speed-Punkt hinzufügen"), clipId, [m](Clip& x, const RetimeMap& map, int) {
        const double s = map.sourceAt(m);
        for (const SpeedPoint& p : x.ramp)
            if (std::abs(p.source - s) < 1) return false;
        SpeedPoint p;
        p.source = s;
        // neuer Abschnitt behält das Tempo der Stelle (harter Wert des Abschnitts, ohne Übergang)
        int seg = 0;
        while (seg < x.ramp.size() && x.ramp[seg].source <= s) ++seg;
        p.speed = ::Retime::segmentSpeed(x, seg);
        x.ramp << p;
        std::sort(x.ramp.begin(), x.ramp.end(), [](const SpeedPoint& a, const SpeedPoint& b) { return a.source < b.source; });
        return true;
    });
}

void Editor::removeSpeedPoint(int clipId, int index)
{
    editRamp(T("Speed-Punkt entfernen"), clipId, [index](Clip& x, const RetimeMap&, int) {
        if (index < 0 || index >= x.ramp.size()) return false;
        x.ramp.removeAt(index);
        return true;
    });
}

void Editor::moveSpeedPoint(int clipId, int index, int delta)
{
    if (delta == 0) return;
    editRamp(T("Speed-Punkt verschieben"), clipId, [index, delta](Clip& x, const RetimeMap&, int L) {
        if (index < 0 || index >= x.ramp.size()) return false;
        // Tempo davor läuft weiter bzw. endet früher -> neue Quellstelle aus dem Tempo des Abschnitts davor
        const double before = ::Retime::segmentSpeed(x, index);
        const double lo = (index > 0 ? x.ramp[index - 1].source : 0) + 1;
        const double hi = (index + 1 < x.ramp.size() ? x.ramp[index + 1].source : L) - 1;
        const double s = std::clamp(x.ramp[index].source + delta * before, lo, hi);
        if (std::abs(s - x.ramp[index].source) < 1e-6) return false;
        x.ramp[index].source = s;
        return true;
    });
}

void Editor::setSegmentSpeed(int clipId, int segment, double speed)
{
    if (speed <= 0) return;
    editRamp(T("Geschwindigkeit ändern"), clipId, [segment, speed](Clip& x, const RetimeMap&, int) {
        if (segment < 0 || segment > x.ramp.size()) return false;
        const double before = ::Retime::segmentSpeed(x, segment);
        ::Retime::setSegmentSpeed(x, segment, speed);
        return ::Retime::segmentSpeed(x, segment) != before;
    });
}

void Editor::setSpeedPointSmooth(int clipId, int index, int frames)
{
    editRamp(T("Übergang des Speed-Punkts"), clipId, [index, frames](Clip& x, const RetimeMap&, int) {
        if (index < 0 || index >= x.ramp.size() || x.ramp[index].smooth == std::max(0, frames)) return false;
        x.ramp[index].smooth = std::max(0, frames);
        return true;
    });
}
