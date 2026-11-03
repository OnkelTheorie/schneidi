// SPDX-License-Identifier: AGPL-3.0-only
// Editor: effects and color grade.
#include "core/Editor.h"

#include "core/Accents.h"
#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/GradeWindows.h"
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

void Editor::addEffect(const QVector<int>& ids, const QString& effectId)
{
    if (effectId.startsWith(EffectFolders::LutPrefix)) { // LUT from the Effects Library -> the clip's color grade
        setGradeLut(ids, effectId.mid(int(qstrlen(EffectFolders::LutPrefix))));
        return;
    }
    const Timeline& tl = m_project->timeline();
    const bool preset = effectId.startsWith(Presets::Prefix);
    const TrackKind kind = effectKind(effectId);
    QVector<int> targets;
    for (int id : editable(ids)) {
        TrackRef ref;
        const Clip* c = TimelineOps::findClip(tl, id, &ref);
        if (c && ref.kind == kind && (preset || !EffectRegistry::has(*c, effectId))) targets << id;
    }
    if (preset) { // Shotcut/Kdenlive preset -> its effects, one undo step
        const Presets::Preset p = Presets::load(effectId.mid(int(qstrlen(Presets::Prefix))));
        const Presets::Mapped m = Presets::map(p, m_project->frameRate());
        if (!p.error.isEmpty() || m.empty()) return;
        modifyClips(targets, T("Preset „%1“ anwenden").arg(p.name), [&](Clip& c) { Presets::apply(c, m); });
        return;
    }
    const EffectDescriptor* d = EffectRegistry::find(effectId);
    if (!d) return;
    modifyClips(targets, T("%1 hinzufügen").arg(d->name), [&](Clip& c) { EffectRegistry::add(c, effectId); });
}

QVector<int> Editor::cleanVoice(const QVector<int>& ids, const QVariantMap& values)
{
    const QString id = QStringLiteral("voiceclean");
    const EffectDescriptor* d = EffectRegistry::find(id);
    const Timeline& tl = m_project->timeline();
    QVector<int> targets;
    for (int x : editable(ids))
        if (TrackRef ref; TimelineOps::findClip(tl, x, &ref) && ref.kind == TrackKind::Audio && !targets.contains(x))
            targets << x;
    if (!d || targets.isEmpty()) return {};
    modifyClips(targets, d->name, [&](Clip& c) {
        if (!EffectRegistry::has(c, id)) EffectRegistry::add(c, id);
        EffectInstance* e = EffectRegistry::instance(c, id);
        if (!e) return;
        for (auto it = values.begin(); it != values.end(); ++it) e->params[it.key()] = it.value();
        e->enabled = true;
    });
    return targets;
}

QVector<int> Editor::audioTrackClips(int index) const
{
    const Timeline& tl = m_project->timeline();
    QVector<int> out;
    if (index < 0 || index >= tl.audio.size() || tl.audio[index].locked) return out;
    for (const Clip& c : tl.audio[index].clips) out << c.id;
    return out;
}

void Editor::removeEffect(const QVector<int>& ids, const QString& effectId)
{
    // unknown effects (plugin missing on this computer) can be removed too
    const EffectDescriptor* d = EffectRegistry::find(effectId);
    QVector<int> targets;
    for (int id : editable(ids))
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id); c && EffectRegistry::has(*c, effectId))
            targets << id;
    modifyClips(targets, T("%1 entfernen").arg(d ? d->name : effectId),
                [&](Clip& c) { EffectRegistry::remove(c, effectId); });
}

void Editor::removeAllEffects(const QVector<int>& ids)
{
    const auto removable = [](const EffectInstance& e) { return e.effectId != "chromakey"; };
    QVector<int> targets;
    for (int id : editable(ids))
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id);
            c && std::any_of(c->effects.begin(), c->effects.end(), removable))
            targets << id;
    modifyClips(targets, T("Alle Effekte entfernen"), [&](Clip& c) {
        QStringList ids;
        for (const EffectInstance& e : c.effects)
            if (removable(e)) ids << e.effectId;
        for (const QString& id : ids) EffectRegistry::remove(c, id); // with the keyframes of its parameters
    });
}

namespace {

// `fx` with the effect `id` at place `to` among the movable effects (clamped); the fixed stages keep their slots
QVector<EffectInstance> withEffectAt(const QVector<EffectInstance>& fx, const QString& id, int to)
{
    QVector<int> at; // indices of the movable effects in fx
    QVector<EffectInstance> list;
    for (int i = 0; i < fx.size(); ++i)
        if (EffectRegistry::movable(fx[i].effectId)) {
            at << i;
            list << fx[i];
        }
    const auto it = std::find_if(list.begin(), list.end(), [&](const EffectInstance& e) { return e.effectId == id; });
    if (it == list.end()) return fx;
    list.move(int(it - list.begin()), std::clamp(to, 0, int(list.size()) - 1));
    QVector<EffectInstance> out = fx;
    for (int k = 0; k < at.size(); ++k) out[at[k]] = list[k];
    return out;
}

// moveEffect/moveEffectBy: place(from) = new place of the effect in a clip where it is at `from` (movable effects)
bool moveEffectWith(Editor& ed, const QVector<int>& ids, const QString& effectId, const std::function<int(int)>& place)
{
    if (!EffectRegistry::movable(effectId)) return false;
    const Timeline& tl = ed.project()->timeline();
    QVector<int> targets;
    for (int id : ed.editable(ids))
        if (const Clip* c = TimelineOps::findClip(tl, id)) {
            const int from = int(EffectRegistry::order(*c).indexOf(effectId));
            if (from >= 0 && withEffectAt(c->effects, effectId, place(from)) != c->effects) targets << id;
        }
    if (targets.isEmpty()) return false;
    const EffectDescriptor* d = EffectRegistry::find(effectId);
    ed.modifyClips(targets, T("%1 verschieben").arg(d ? d->name : effectId), [&](Clip& c) {
        c.effects = withEffectAt(c.effects, effectId, place(int(EffectRegistry::order(c).indexOf(effectId))));
    });
    return true;
}

} // namespace

bool Editor::moveEffect(const QVector<int>& ids, const QString& effectId, int to)
{
    return moveEffectWith(*this, ids, effectId, [to](int) { return to; });
}

bool Editor::moveEffectBy(const QVector<int>& ids, const QString& effectId, int steps)
{
    return moveEffectWith(*this, ids, effectId, [steps](int from) { return from + steps; });
}

TrackKind Editor::effectKind(const QString& effectId)
{
    const EffectDescriptor* d = EffectRegistry::find(effectId);
    return d && !d->video ? TrackKind::Audio : TrackKind::Video;
}

QVector<int> Editor::effectTargets(int frame, TrackKind kind) const
{
    const Timeline& tl = m_project->timeline();
    QVector<int> ids;
    for (int id : m_selection->ids()) {
        TrackRef ref;
        if (TimelineOps::findClip(tl, id, &ref) && ref.kind == kind) ids << id;
    }
    if (!ids.isEmpty()) return ids;
    // Without a selection: the visible (top) video clip under the playhead, audio: from A1 down
    const QVector<Track>& tracks = tl.tracks(kind);
    for (int k = 0; k < tracks.size(); ++k) {
        const int i = kind == TrackKind::Video ? tracks.size() - 1 - k : k;
        if (!tracks[i].locked)
            for (const Clip& c : tracks[i].clips)
                if (frame >= c.start && frame < c.end()) return {c.id};
    }
    return {};
}

QVector<int> Editor::addAccents(const QVector<int>& idsIn, const QVector<int>& frames, const Accents::Amounts& amounts,
                                const QString& text)
{
    const Timeline& tl = m_project->timeline();
    auto under = [&](const Clip& c) {
        return std::any_of(frames.begin(), frames.end(), [&](int f) { return f >= c.start && f < c.end(); });
    };
    QVector<int> ids;
    for (int id : idsIn)
        if (TrackRef ref; TimelineOps::findClip(tl, id, &ref) && ref.kind == TrackKind::Video) ids << id;
    if (idsIn.isEmpty())
        for (const Track& t : std::as_const(tl.video)) {
            if (t.hidden) continue;
            for (const Clip& c : t.clips)
                if (c.enabled && !c.isTitle() && under(c)) ids << c.id;
        }
    ids = editable(ids);
    QSet<int> landed;
    for (int id : ids) {
        const Clip* c = TimelineOps::findClip(tl, id);
        for (int f : frames)
            if (c && f >= c->start && f < c->end()) landed.insert(f);
    }
    if (landed.isEmpty()) return {};
    const ProjectFormat fmt = m_project->timelineFormat();
    modifyClips(ids, text, [&](Clip& c) {
        // All times, not only the clip's own: the ones outside space its flashes
        QVector<int> local;
        for (int f : frames) local << f - c.start;
        Accents::apply(c, local, amounts, fmt.rate.fps(), QSizeF(fmt.width, fmt.height));
    });
    QVector<int> out(landed.begin(), landed.end());
    std::sort(out.begin(), out.end());
    return out;
}

namespace {
constexpr const char* kGrade = "grade"; // effect of the Color page (EffectRegistry)

QVector<AnimParam> gradeParams()
{
    QVector<AnimParam> list;
    if (const EffectDescriptor* d = EffectRegistry::find(kGrade))
        for (const EffectParam& p : d->params)
            if (p.anim != AnimParam::Count && !p.sparse) list << p.anim; // without the windows
    return list;
}

// Default value of a grade parameter
double gradeDefault(AnimParam a)
{
    const EffectParam* p = nullptr;
    return EffectRegistry::paramFor(a, nullptr, &p) ? p->defaultValue.toDouble() : 0.0;
}

// Video clips only (Color page acts on the picture)
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
        EffectRegistry::add(c, kGrade); // already present: nothing
        for (const auto& [p, v] : values) {
            // Values of a window only where the clip has that window
            QString effectId, key;
            if (Keys::effectParamOf(p, &effectId, &key))
                if (const int n = GradeWindows::windowOf(key); n > 0 && !GradeWindows::exists(c, n)) continue;
            Keys::setValue(c, p, localFrame(c, frame), v);
        }
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
        for (AnimParam p : params) {
            QString effectId, key;
            if (Keys::effectParamOf(p, &effectId, &key)) // a window's value only where the clip has that window
                if (const int n = GradeWindows::windowOf(key); n > 0 && !GradeWindows::exists(c, n)) continue;
            Keys::setValue(c, p, localFrame(c, frame), gradeDefault(p));
        }
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

void Editor::setGradeKeyframe(const QVector<int>& ids, int frame, bool on, int window)
{
    QVector<AnimParam> params = gradeParams();
    if (window >= 1 && window <= GradeWindows::kMax) {
        params.clear();
        for (const QString& k : GradeWindows::correctionKeys() + GradeWindows::geometryKeys())
            params << GradeWindows::param(window, k);
    }
    modifyClips(videoOnly(m_project->timeline(), ids), on ? T("Keyframe setzen") : T("Keyframe entfernen"),
                [&](Clip& c) {
                    EffectRegistry::add(c, kGrade);
                    const int t = localFrame(c, frame);
                    if (window > 0 && !GradeWindows::exists(c, window)) return;
                    for (AnimParam p : params) {
                        if (on) Keys::setKey(c, p, t, Keys::valueAt(c, p, t));
                        else Keys::removeKey(c, p, t);
                    }
                });
}

void Editor::addGradeWindow(const QVector<int>& ids, int n, const QString& shape)
{
    GradeWindows::Shape s = GradeWindows::Shape::Ellipse;
    if (n < 1 || n > GradeWindows::kMax || !GradeWindows::shapeFromId(shape, &s)) return;
    modifyClips(videoOnly(m_project->timeline(), ids), T("Fenster hinzufügen"), [&](Clip& c) {
        EffectRegistry::add(c, kGrade);
        GradeWindows::add(c, n, s);
    });
}

void Editor::removeGradeWindow(const QVector<int>& ids, int n)
{
    QVector<int> targets;
    for (int id : videoOnly(m_project->timeline(), ids))
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id); c && GradeWindows::exists(*c, n))
            targets << id;
    modifyClips(targets, T("Fenster löschen"), [&](Clip& c) { GradeWindows::remove(c, n); });
}

void Editor::setGradeWindowPath(int clipId, int n, int from, const QVector<GradeWindows::PathPoint>& points)
{
    const Clip* found = TimelineOps::findClip(m_project->timeline(), clipId);
    if (!found || !GradeWindows::exists(*found, n) || points.isEmpty()) return;
    modifyClips({clipId}, T("Fenster verfolgen"), [&](Clip& c) {
        const AnimParam px = GradeWindows::param(n, "x"), py = GradeWindows::param(n, "y");
        int lo = from, hi = from;
        for (const GradeWindows::PathPoint& p : points) {
            lo = std::min(lo, p.frame);
            hi = std::max(hi, p.frame);
        }
        // Keep the start where it is, then replace what lies in the tracked range
        const double x0 = Keys::valueAt(c, px, from), y0 = Keys::valueAt(c, py, from);
        for (AnimParam p : {px, py})
            for (int t : Keys::keyTimes(c, {p}))
                if (t >= lo && t <= hi) Keys::removeKey(c, p, t);
        Keys::setKey(c, px, from, x0);
        Keys::setKey(c, py, from, y0);
        for (const GradeWindows::PathPoint& p : points) {
            Keys::setKey(c, px, p.frame, p.x);
            Keys::setKey(c, py, p.frame, p.y);
        }
    });
}

QHash<int, QVector<int>> Editor::blurNames(const QHash<int, QVector<NameBlur::Mask>>& masks, const QString& fill,
                                          double amount)
{
    QHash<int, QVector<int>> added;
    GradeWindows::Fill f = GradeWindows::Fill::Blur;
    if (!GradeWindows::fillFromId(fill, &f) || (f != GradeWindows::Fill::Blur && f != GradeWindows::Fill::Pixelate))
        return added;
    QVector<int> targets;
    for (int id : videoOnly(m_project->timeline(), masks.keys().toVector()))
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id);
            c && !masks.value(id).isEmpty() && GradeWindows::freeNumber(*c))
            targets << id;
    std::sort(targets.begin(), targets.end());
    modifyClips(targets, T("Namen verwischen"), [&](Clip& c) {
        EffectRegistry::add(c, kGrade);
        // A grade switched off (Color page, Inspector) would leave the new windows out of the picture: the names
        // showed in the export
        if (EffectInstance* g = EffectRegistry::instance(c, kGrade)) g->enabled = true;
        for (const NameBlur::Mask& m : masks.value(c.id)) {
            const int n = GradeWindows::freeNumber(c);
            if (!n || m.keys.isEmpty()) break;
            GradeWindows::add(c, n, GradeWindows::Shape::Rectangle);
            EffectInstance* e = EffectRegistry::instance(c, kGrade);
            e->params[GradeWindows::key(n, "fill")] = QString(GradeWindows::fillId(f));
            e->params[GradeWindows::key(n, "softness")] = 0.5; // the padding covers the text, a short soft edge
            const AnimParam px = GradeWindows::param(n, "x"), py = GradeWindows::param(n, "y");
            const AnimParam pw = GradeWindows::param(n, "width"), ph = GradeWindows::param(n, "height");
            const AnimParam pa = GradeWindows::param(n, "amount");
            for (const NameBlur::Key& k : m.keys) {
                Keys::setKey(c, px, k.frame, k.rect.center().x() * 100);
                Keys::setKey(c, py, k.frame, k.rect.center().y() * 100);
                Keys::setKey(c, pw, k.frame, k.on ? k.rect.width() * 100 : 0);
                Keys::setKey(c, ph, k.frame, k.on ? k.rect.height() * 100 : 0);
                Keys::setKey(c, pa, k.frame, k.on ? amount : 0);
            }
            added[c.id] << n;
        }
    });
    return added;
}

void Editor::setGradeWindowOption(const QVector<int>& ids, int n, const QString& key, const QVariant& value)
{
    QVector<int> targets;
    for (int id : videoOnly(m_project->timeline(), ids))
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id); c && GradeWindows::exists(*c, n))
            targets << id;
    modifyClips(targets, T("Fenster ändern"), [&](Clip& c) {
        EffectRegistry::instance(c, kGrade)->params[GradeWindows::key(n, key)] = value;
    });
}
