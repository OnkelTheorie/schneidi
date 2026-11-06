// SPDX-License-Identifier: AGPL-3.0-only
// Editor: template placeholders (core/TemplateFill.h)
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"

QString Editor::setClipSlot(int clipId, const ClipSlot& slot)
{
    TrackRef ref;
    const Clip* c = TimelineOps::findClip(m_project->timeline(), clipId, &ref);
    if (!c) return QString("no clip %1 in the open timeline").arg(clipId);
    if (isTrackLocked(ref)) return QString("the track %1 is locked").arg(trackShortName(ref));
    // The whole instance: a slot can be several clips (frozen tail, loop repeats, a razor cut), all of them change
    const ProjectData d = m_project->data();
    const QVector<TemplateFill::SlotRef> all = TemplateFill::listSlots(d);
    QVector<int> parts{clipId};
    for (const TemplateFill::SlotRef& r : all)
        if (r.sequence == d.currentSequence && r.parts.contains(clipId)) parts = r.parts;
    ClipSlot s = slot;
    s.id = s.id.trimmed();
    if (s.isSet()) {
        for (int id : parts)
            if (const Clip* p = TimelineOps::findClip(m_project->timeline(), id))
                if (const QString why = TemplateFill::canHold(*p, ref.kind, s.kind); !why.isEmpty()) return why;
        for (const TemplateFill::SlotRef& r : all)
            if (r.slot.id == s.id && r.slot.kind != s.kind && !r.parts.contains(clipId))
                return QString("'%1' is a slot of another kind already (clip %2)").arg(s.id).arg(r.clip);
        if (s.maxLen > 0 && s.minLen > s.maxLen) std::swap(s.minLen, s.maxLen);
        s.minLen = std::max(0, s.minLen);
        s.maxLen = std::max(0, s.maxLen);
        s.maxChars = std::max(0, s.maxChars);
        // The fill keeps this one (a Fixed slot's own length), the user does not set it
        s.nominalLen = s.length == SlotLength::Fixed ? c->slot.nominalLen : 0;
    } else {
        s = {};
    }
    const auto same = [&](int id) {
        const Clip* p = TimelineOps::findClip(m_project->timeline(), id);
        return !p || p->slot == s;
    };
    if (std::all_of(parts.begin(), parts.end(), same)) return {};
    m_project->edit(s.isSet() ? T("Als Platzhalter markieren") : T("Platzhalter entfernen"), [&](Timeline& tl) {
        for (int id : parts)
            if (Clip* x = TimelineOps::findClip(tl, id)) x->slot = s;
    });
    return {};
}

TemplateFill::Report Editor::fillSlot(const QString& slotId, const TemplateFill::Value& value,
                                      const TemplateFill::Context& ctx)
{
    return fillSlots({{slotId, value}}, ctx);
}

TemplateFill::Report Editor::fillSlots(const TemplateFill::Values& values, const TemplateFill::Context& ctx)
{
    ProjectData d = m_project->data();
    const int mediaBefore = int(d.media.size());
    TemplateFill::Report report = TemplateFill::apply(d, values, ctx);
    if (std::none_of(report.results.begin(), report.results.end(), [](const TemplateFill::SlotReport& s) { return s.ok; }))
        return report;
    // New files into the Media Pool (not in the undo history, like a drag into the timeline), the ids the fill gave
    // out reserved, then all timelines as one step
    for (int i = mediaBefore; i < d.media.size(); ++i) m_project->addMedia(d.media[i]);
    while (m_project->newClipId() < d.lastClipId) {
    }
    while (m_project->newLinkId() < d.lastLinkId) {
    }
    m_project->editSequences(T("Platzhalter füllen"), [&](QVector<Sequence>& seqs, int&) { seqs = d.sequences; });
    return report;
}
