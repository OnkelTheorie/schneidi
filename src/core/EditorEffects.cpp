// Editor: effects and color grade.
#include "core/Editor.h"

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

void Editor::addEffect(const QVector<int>& ids, const QString& effectId)
{
    if (effectId.startsWith(EffectFolders::LutPrefix)) { // LUT aus der Effects Library -> Farbkorrektur des Clips
        setGradeLut(ids, effectId.mid(int(qstrlen(EffectFolders::LutPrefix))));
        return;
    }
    const Timeline& tl = m_project->timeline();
    const bool preset = effectId.startsWith(Presets::Prefix);
    QVector<int> targets;
    for (int id : editable(ids)) {
        TrackRef ref;
        const Clip* c = TimelineOps::findClip(tl, id, &ref);
        if (c && ref.kind == TrackKind::Video && (preset || !EffectRegistry::has(*c, effectId))) targets << id;
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
