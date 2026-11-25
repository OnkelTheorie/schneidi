// SPDX-License-Identifier: AGPL-3.0-only
// Editor: effects and color grade.
#include "core/Editor.h"

#include "core/CompGraph.h"

#include "core/Accents.h"
#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/Equalizer.h"
#include "core/GradeQualifier.h"
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

QVector<int> Editor::cleanVoice(const QVector<int>& ids, const QVariantMap& values, bool voiceEq)
{
    const QString id = QStringLiteral("voiceclean");
    const EffectDescriptor* d = EffectRegistry::find(id);
    const Timeline& tl = m_project->timeline();
    QVector<int> targets;
    QVector<int> tracks; // audio tracks of the targets (voice EQ)
    for (int x : editable(ids))
        if (TrackRef ref; TimelineOps::findClip(tl, x, &ref) && ref.kind == TrackKind::Audio && !targets.contains(x)) {
            targets << x;
            if (!tracks.contains(ref.index)) tracks << ref.index;
        }
    if (!d || targets.isEmpty()) return {};
    const TrackEq eq = Equalizer::voicePreset();
    m_project->edit(d->name, [&](Timeline& t) {
        for (int x : targets) {
            Clip* c = TimelineOps::findClip(t, x);
            if (!c) continue;
            if (!EffectRegistry::has(*c, id)) EffectRegistry::add(*c, id);
            EffectInstance* e = EffectRegistry::instance(*c, id);
            if (!e) continue;
            for (auto it = values.begin(); it != values.end(); ++it) e->params[it.key()] = it.value();
            e->enabled = true;
        }
        if (voiceEq)
            for (int track : tracks)
                if (track < t.audio.size()) t.audio[track].eq = eq;
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

// A grade value of the clip's own (its windows, its qualifier) rather than of the correction of the whole picture
bool ownGradeKey(const QString& key) { return GradeWindows::windowOf(key) > 0 || GradeQualifier::isKey(key); }
bool ownGradeParam(AnimParam p)
{
    QString effectId, key;
    return Keys::effectParamOf(p, &effectId, &key) && effectId == QLatin1String(kGrade) && ownGradeKey(key);
}
bool correctionParam(AnimParam p)
{
    QString effectId, key;
    if (Keys::effectParamOf(p, &effectId, &key)) return effectId == QLatin1String(kGrade) && !ownGradeKey(key);
    return int(p) >= int(AnimParam::GradeLiftY) && int(p) <= int(AnimParam::GradeExposure);
}

// A kill feed copy takes the correction of its clip (values, keyframes, LUT, curves, on/off) and keeps its own
// windows and qualifier (a name blur made on the copy). The clip's grade off or gone: the copy's correction is
// neutral, its own windows stay on.
void syncCopyGrade(Clip& copy, const Clip& lead)
{
    const EffectInstance* from = EffectRegistry::instance(lead, kGrade);
    int at = -1;
    for (int i = 0; i < copy.effects.size() && at < 0; ++i)
        if (copy.effects[i].effectId == QLatin1String(kGrade)) at = i;
    QVariantMap own;
    if (at >= 0)
        for (auto it = copy.effects[at].params.cbegin(); it != copy.effects[at].params.cend(); ++it)
            if (ownGradeKey(it.key())) own.insert(it.key(), it.value());
    const bool ownKeys = std::any_of(copy.keys.keyBegin(), copy.keys.keyEnd(), ownGradeParam);
    const bool hasOwn = !own.isEmpty() || ownKeys;
    const bool on = from && from->enabled;
    copy.keys.removeIf([](const auto& it) { return correctionParam(it.key()); });
    if (!from && !hasOwn) {
        if (at >= 0) copy.effects.removeAt(at);
        return;
    }
    EffectInstance g = from ? *from : EffectInstance{};
    g.effectId = kGrade;
    for (auto it = g.params.begin(); it != g.params.end();)
        it = ownGradeKey(it.key()) || (!on && hasOwn) ? g.params.erase(it) : std::next(it);
    for (auto it = own.cbegin(); it != own.cend(); ++it) g.params.insert(it.key(), it.value());
    g.enabled = hasOwn || on;
    if (on || !hasOwn)
        for (auto it = lead.keys.cbegin(); it != lead.keys.cend(); ++it)
            if (correctionParam(it.key())) {
                KeyTrack k = it.value();
                for (Keyframe& f : k) f.frame += copy.in - lead.in;
                copy.keys.insert(it.key(), k);
            }
    if (at >= 0) copy.effects[at] = g;
    else copy.effects << g;
}
} // namespace

void Editor::modifyGrade(const QVector<int>& ids, const QString& text, const std::function<void(Clip&)>& fn,
                         const QString& mergeKey)
{
    const QVector<int> leads = editable(videoOnly(m_project->timeline(), ids));
    if (leads.isEmpty()) return;
    QHash<int, int> leadOf; // copy -> its clip
    for (int id : leads)
        for (int copy : followingCopies({id}, true))
            if (!leads.contains(copy)) leadOf.insert(copy, id);
    m_project->edit(text, [&](Timeline& tl) {
        for (int id : leads)
            if (Clip* c = TimelineOps::findClip(tl, id)) fn(*c);
        for (auto it = leadOf.cbegin(); it != leadOf.cend(); ++it) {
            Clip* copy = TimelineOps::findClip(tl, it.key());
            const Clip* lead = TimelineOps::findClip(tl, it.value());
            if (copy && lead) syncCopyGrade(*copy, *lead);
        }
    }, mergeKey);
}

void Editor::setGradeValues(const QVector<int>& ids, const QVector<QPair<AnimParam, double>>& values, int frame,
                            const QString& text, const QString& mergeKey)
{
    modifyGrade(ids, text, [&](Clip& c) {
        EffectRegistry::add(c, kGrade); // already present: nothing
        for (const auto& [p, v] : values) {
            // Values of a window only where the clip has that window
            QString effectId, key;
            if (Keys::effectParamOf(p, &effectId, &key)) {
                if (const int n = GradeWindows::windowOf(key); n > 0 && !GradeWindows::exists(c, n)) continue;
                if (GradeQualifier::isKey(key) && !GradeQualifier::exists(c)) continue; // likewise the qualifier's
            }
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
    modifyGrade(targets, text, [&](Clip& c) {
        if (params.isEmpty()) {
            EffectRegistry::remove(c, kGrade);
            return;
        }
        for (AnimParam p : params) {
            QString effectId, key;
            if (Keys::effectParamOf(p, &effectId, &key)) { // a window's value only where the clip has that window
                if (const int n = GradeWindows::windowOf(key); n > 0 && !GradeWindows::exists(c, n)) continue;
                if (GradeQualifier::isKey(key) && !GradeQualifier::exists(c)) continue;
            }
            Keys::setValue(c, p, localFrame(c, frame), gradeDefault(p));
        }
    });
}

void Editor::setGradeLut(const QVector<int>& ids, const QString& path)
{
    modifyGrade(ids, path.isEmpty() ? T("LUT entfernen") : T("LUT laden"),
                [&](Clip& c) {
                    EffectRegistry::add(c, kGrade);
                    EffectRegistry::instance(c, kGrade)->params["lut"] = path;
                });
}

void Editor::setGradeEnabled(const QVector<int>& ids, bool on)
{
    modifyGrade(ids, on ? T("Farbkorrektur an") : T("Farbkorrektur aus"),
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
    } else if (window == GradeQualifier::kLayer) {
        params.clear();
        for (const QString& k : GradeWindows::correctionKeys() + GradeQualifier::rangeKeys())
            params << GradeQualifier::param(k);
    }
    modifyGrade(ids, on ? T("Keyframe setzen") : T("Keyframe entfernen"),
                [&](Clip& c) {
                    EffectRegistry::add(c, kGrade);
                    const int t = localFrame(c, frame);
                    if (window == GradeQualifier::kLayer ? !GradeQualifier::exists(c)
                                                         : window > 0 && !GradeWindows::exists(c, window))
                        return;
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
    modifyClips(targets, T("Fenster löschen"), [&](Clip& c) {
        GradeWindows::remove(c, n);
        // A qualifier limited to it: the whole picture again (a later window with the same number must not limit it)
        if (EffectInstance* e = EffectRegistry::instance(c, kGrade);
            e && int(e->params.value(GradeQualifier::key("window")).toDouble()) == n)
            e->params.remove(GradeQualifier::key("window"));
    });
}

void Editor::setGradeCurve(const QVector<int>& ids, GradeCurves::Kind kind, const QVector<QPointF>& points,
                           const QString& mergeKey)
{
    const QString text = GradeCurves::format(points);
    const QString key = GradeCurves::key(kind);
    modifyGrade(ids, T("Kurve"), [&](Clip& c) {
        EffectRegistry::add(c, kGrade);
        EffectInstance* e = EffectRegistry::instance(c, kGrade);
        if (GradeCurves::isIdentity(GradeCurves::parse(text), kind)) e->params.remove(key); // straight: as never set
        else e->params[key] = text;
    }, mergeKey);
}

void Editor::addGradeQualifier(const QVector<int>& ids)
{
    modifyClips(videoOnly(m_project->timeline(), ids), T("Qualifier hinzufügen"), [&](Clip& c) {
        EffectRegistry::add(c, kGrade);
        GradeQualifier::add(c);
    });
}

void Editor::removeGradeQualifier(const QVector<int>& ids)
{
    QVector<int> targets;
    for (int id : videoOnly(m_project->timeline(), ids))
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id); c && GradeQualifier::exists(*c))
            targets << id;
    modifyClips(targets, T("Qualifier löschen"), [&](Clip& c) { GradeQualifier::remove(c); });
}

void Editor::setGradeQualifierRange(const QVector<int>& ids, const GradeQualifier::Range& range, int frame,
                                    const QString& mergeKey)
{
    QVector<int> targets;
    for (int id : videoOnly(m_project->timeline(), ids))
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id); c && GradeQualifier::exists(*c))
            targets << id;
    const double values[] = {range.hue,    range.hueWidth, range.hueSoft, range.satLow, range.satHigh,
                             range.satSoft, range.lumLow,  range.lumHigh, range.lumSoft};
    modifyClips(targets, T("Qualifier"), [&](Clip& c) {
        const QStringList& keys = GradeQualifier::rangeKeys();
        for (int i = 0; i < keys.size(); ++i) Keys::setValue(c, GradeQualifier::param(keys[i]), localFrame(c, frame), values[i]);
        EffectRegistry::instance(c, kGrade)->params[GradeQualifier::key("invert")] = range.invert;
    }, mergeKey);
}

void Editor::setGradeQualifierInvert(const QVector<int>& ids, bool invert)
{
    QVector<int> targets;
    for (int id : videoOnly(m_project->timeline(), ids))
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id); c && GradeQualifier::exists(*c))
            targets << id;
    modifyClips(targets, invert ? T("Qualifier invertieren") : T("Qualifier nicht invertieren"), [&](Clip& c) {
        EffectRegistry::instance(c, kGrade)->params[GradeQualifier::key("invert")] = invert;
    });
}

void Editor::setGradeQualifierWindow(const QVector<int>& ids, int n)
{
    QVector<int> targets;
    for (int id : videoOnly(m_project->timeline(), ids))
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id);
            c && GradeQualifier::exists(*c) && (n == 0 || GradeWindows::exists(*c, n)))
            targets << id;
    modifyClips(targets, n ? T("Qualifier im Fenster") : T("Qualifier im ganzen Bild"), [&](Clip& c) {
        EffectInstance* e = EffectRegistry::instance(c, kGrade);
        if (n) e->params[GradeQualifier::key("window")] = double(n);
        else e->params.remove(GradeQualifier::key("window"));
    });
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

namespace {

// Seen keyframes of a tracker over the analyzed range (found frames and lost ones): step keys, one on each side of a
// change, so every frame reads exactly 1 or 0. What lies before and after the range keeps its value; without lost
// frames and without earlier seen keys nothing is written (an analysis that never lost the point)
// The frames an analysis covers: its points, and after giving up the rest of the asked range (loss.to)
void coveredRange(const QVector<Comp::TrackPoint>& points, const Comp::TrackLoss& loss, int* lo, int* hi)
{
    *lo = INT_MAX;
    *hi = INT_MIN;
    for (const Comp::TrackPoint& q : points) {
        *lo = std::min(*lo, q.frame);
        *hi = std::max(*hi, q.frame);
    }
    if (loss.gaveUp) {
        for (int f : loss.frames) {
            *lo = std::min(*lo, f);
            *hi = std::max(*hi, f);
        }
        if (loss.to >= 0) {
            *lo = std::min(*lo, loss.to);
            *hi = std::max(*hi, loss.to);
        }
    }
}

// Lost frames between found ones are gaps; lost frames at the far end of a run that did not give up are not (the last
// frames blurred: followers hold the last place, nothing vanishes for the rest of the clip); after giving up every
// frame of the asked range without a point is lost
void writeSeen(Clip& c, int node, const QVector<Comp::TrackPoint>& points, const Comp::TrackLoss& loss)
{
    const AnimParam p = Keys::effectParam(Comp::kEffectId, CompGraph::paramKey(node, Comp::kSeenKey));
    const bool had = Keys::animated(c, p);
    QSet<int> found;
    for (const Comp::TrackPoint& q : points) found.insert(q.frame);
    int lo = 0, hi = -1;
    coveredRange(points, loss, &lo, &hi);
    if (lo > hi) return;
    bool anyLost = false;
    for (int f = lo; f <= hi && !anyLost; ++f) anyLost = !found.contains(f) && (loss.gaveUp || loss.frames.contains(f));
    if (!anyLost && !had) return;
    // The old curve around the range (step keys: their values as they are)
    bool keysBefore = false, keysAfter = false;
    for (int t : Keys::keyTimes(c, {p})) {
        keysBefore |= t < lo;
        keysAfter |= t > hi;
    }
    const double before = had ? Keys::valueAt(c, p, lo - 1) : 1.0, after = had ? Keys::valueAt(c, p, hi + 1) : 1.0;
    for (int t : Keys::keyTimes(c, {p}))
        if (t >= lo - 1 && t <= hi + 1) Keys::removeKey(c, p, t);
    const auto state = [&](int f) { return found.contains(f) ? 1.0 : 0.0; };
    // Into the range: the old value up to lo - 1 (a key there keeps it a step), the new one from lo
    if (keysBefore && before != state(lo)) Keys::setKey(c, p, lo - 1, before);
    if (keysBefore || state(lo) == 0 || !keysAfter) Keys::setKey(c, p, lo, state(lo));
    for (int f = lo + 1; f <= hi; ++f)
        if (state(f) != state(f - 1)) {
            Keys::setKey(c, p, f - 1, state(f - 1));
            Keys::setKey(c, p, f, state(f));
        }
    // Out of it: the old value again from hi + 1
    if (keysAfter) {
        Keys::setKey(c, p, hi, state(hi));
        if (after != state(hi)) Keys::setKey(c, p, hi + 1, after);
    }
}

} // namespace

bool Editor::trackCompNode(int clipId, int node, const QVector<Comp::TrackPoint>& points, const Comp::TrackLoss& loss)
{
    const Clip* found = TimelineOps::findClip(m_project->timeline(), clipId);
    if (!found || points.isEmpty()) return false;
    const EffectInstance* e = EffectRegistry::instance(*found, Comp::kEffectId);
    CompGraph g;
    if (!e || !Comp::graphOf(*e, &g) || !g.node(node) || g.node(node)->type != QLatin1String(Comp::kTrackerType))
        return false;
    if (editable({clipId}).isEmpty()) return false; // its track locked meanwhile: modifyClips would drop it silently
    modifyClips({clipId}, T("Knoten verfolgen"), [&](Clip& c) {
        AnimParam params[4];
        for (int i = 0; i < 4; ++i) params[i] = Keys::effectParam(Comp::kEffectId, CompGraph::paramKey(node, Comp::kTrackKeys[i]));
        // Replaced: the run's range, after giving up up to the end of the asked range (an older path there would
        // otherwise carry on after the point was lost)
        int lo = 0, hi = 0;
        coveredRange(points, loss, &lo, &hi);
        // The old path's size and turn at the new run's start frame (its scale/angle start at 1/0 there)
        const int start = points.first().frame;
        const double oldScale = Keys::keyTimes(c, {params[2]}).isEmpty() ? 1.0 : Keys::valueAt(c, params[2], start);
        const double oldAngle = Keys::keyTimes(c, {params[3]}).isEmpty() ? 0.0 : Keys::valueAt(c, params[3], start);
        for (AnimParam p : params)
            for (int t : Keys::keyTimes(c, {p}))
                if (t >= lo && t <= hi) Keys::removeKey(c, p, t);
        // The reference point followers are drawn relative to (follow rule, core/CompTrack.h): the start frame's
        // point (the first one). A new path sets it; an analysis that adds to a path kept outside its range keeps
        // it, so the part tracked before does not move, and goes on from the old path's scale and angle at its start
        // (followers do not snap back to the start frame's size)
        EffectInstance* inst = EffectRegistry::instance(c, Comp::kEffectId);
        const QString rx = CompGraph::paramKey(node, Comp::kRefKeys[0]), ry = CompGraph::paramKey(node, Comp::kRefKeys[1]);
        double scale0 = oldScale, angle0 = oldAngle;
        // One point has no size or turn: an earlier two-point or face run's must not carry over (scaled followers 15x)
        if (inst && inst->params.value(CompGraph::paramKey(node, QStringLiteral("mode"))).toString() == QLatin1String("point"))
            scale0 = 1, angle0 = 0;
        if (inst && (Keys::keyTimes(c, {params[0]}).isEmpty() || !inst->params.contains(rx) || !inst->params.contains(ry))) {
            inst->params[rx] = points.first().x;
            inst->params[ry] = points.first().y;
            scale0 = 1;
            angle0 = 0;
        }
        for (const Comp::TrackPoint& p : points) {
            const double v[4] = {p.x, p.y, p.scale * scale0, p.angle + angle0};
            for (int i = 0; i < 4; ++i) Keys::setKey(c, params[i], p.frame, v[i]);
        }
        // Four corners (mode quad): absolute places, replaced in the run's range like the rest
        if (points.first().quad) {
            for (int i = 0; i < 8; ++i) {
                const AnimParam cp = Keys::effectParam(Comp::kEffectId, CompGraph::paramKey(node, Comp::kCornerKeys[i]));
                for (int t : Keys::keyTimes(c, {cp}))
                    if (t >= lo && t <= hi) Keys::removeKey(c, cp, t);
                for (const Comp::TrackPoint& p : points)
                    Keys::setKey(c, cp, p.frame, i % 2 ? p.corners[i / 2].y() : p.corners[i / 2].x());
            }
        }
        writeSeen(c, node, points, loss);
    });
    return true;
}

QHash<int, QVector<int>> Editor::blurNames(const QHash<int, QVector<NameBlur::Mask>>& masks, const QString& fill,
                                          double amount, int* dropped, int* locked)
{
    if (dropped) *dropped = 0;
    if (locked) *locked = 0;
    QHash<int, QVector<int>> added;
    GradeWindows::Fill f = GradeWindows::Fill::Blur;
    if (!GradeWindows::fillFromId(fill, &f) || (f != GradeWindows::Fill::Blur && f != GradeWindows::Fill::Pixelate))
        return added;
    QVector<int> targets;
    int wanted = 0; // masks with keys on video clips: each one not placed leaves a name visible
    // A clip on a locked track (a kill feed copy above its unlocked clip) stays as it is, like every edit leaves it:
    // its names stay readable, counted apart so that the caller can say why
    const QVector<int> open = editable(masks.keys().toVector());
    for (int id : videoOnly(m_project->timeline(), masks.keys().toVector()))
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id); c && !masks.value(id).isEmpty()) {
            int n = 0;
            for (const NameBlur::Mask& m : masks.value(id)) n += !m.keys.isEmpty();
            if (!open.contains(id)) {
                if (locked) *locked += n;
                continue;
            }
            wanted += n;
            if (GradeWindows::freeNumber(*c)) targets << id;
        }
    std::sort(targets.begin(), targets.end());
    modifyClips(targets, T("Namen verwischen"), [&](Clip& c) {
        EffectRegistry::add(c, kGrade);
        // A grade switched off (Color page, Inspector) would leave the new windows out of the picture: the names
        // showed in the export
        if (EffectInstance* g = EffectRegistry::instance(c, kGrade)) g->enabled = true;
        for (const NameBlur::Mask& m : masks.value(c.id)) {
            if (m.keys.isEmpty()) continue;
            const int n = GradeWindows::freeNumber(c);
            if (!n) break;
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
    if (dropped) {
        int placed = 0;
        for (const QVector<int>& w : added) placed += w.size();
        *dropped = wanted - placed;
    }
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
