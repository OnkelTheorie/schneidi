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
    ClipSlot s = slot;
    s.id = s.id.trimmed();
    if (s.isSet()) {
        if (const QString why = TemplateFill::canHold(*c, ref.kind, s.kind); !why.isEmpty()) return why;
        for (const TemplateFill::SlotRef& r : TemplateFill::listSlots(m_project->data()))
            if (r.slot.id == s.id && r.slot.kind != s.kind && !r.parts.contains(clipId))
                return QString("'%1' is a slot of another kind already (clip %2)").arg(s.id).arg(r.clip);
        if (s.maxLen > 0 && s.minLen > s.maxLen) std::swap(s.minLen, s.maxLen);
        s.minLen = std::max(0, s.minLen);
        s.maxLen = std::max(0, s.maxLen);
        s.maxChars = std::max(0, s.maxChars);
    } else {
        s = {};
    }
    if (c->slot == s) return {};
    m_project->edit(s.isSet() ? T("Als Platzhalter markieren") : T("Platzhalter entfernen"), [&](Timeline& tl) {
        if (Clip* x = TimelineOps::findClip(tl, clipId)) x->slot = s;
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
