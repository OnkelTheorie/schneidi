// Editor: transitions.
#include "core/Editor.h"
#include "core/EditorDetail.h"

#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/Presets.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Retime.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/Subtitles.h"
#include "core/TimelineOps.h"

#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <algorithm>

using EditorDetail::fitTransitions;

void EditorDetail::fitTransitions(Timeline& tl, const QSet<int>& clipIds, const TimelineOps::SourceLength& len)
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

namespace {

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
