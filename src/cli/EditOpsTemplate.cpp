// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli: edit ops on template placeholders (core/TemplateFill.h) - `slot` marks a clip as a placeholder with
// its rules (or unmarks it), `fill` fills one slot with a file or a text by those rules (Editor::fillSlot).
#include "cli/CommandsDetail.h"

#include "core/TimelineOps.h"

#include <QJsonArray>

namespace Cli::detail {

namespace {

template <typename E, size_t N>
E enumOf(const char* const (&ids)[N], const QJsonObject& op, const char* key, E fallback)
{
    if (!op.contains(key)) return fallback;
    E v = fallback;
    if (!slotEnumFromId(ids, op.value(key).toString().trimmed().toLower(), &v)) {
        QStringList names;
        for (const char* id : ids) names << id;
        fail("BAD_ARGUMENT", QString("%1: %2").arg(key, names.join(", ")));
    }
    return v;
}

bool flagOf(const QJsonObject& op, const char* key, bool fallback)
{
    if (!op.contains(key)) return fallback;
    if (!op.value(key).isBool()) fail("BAD_ARGUMENT", QString("%1: true or false").arg(key));
    return op.value(key).toBool();
}

void opSlot(Session& s, const QJsonObject& op)
{
    const Clip& c = clipOf(s, op);
    const int id = c.id;
    if (op.value("remove").toBool()) {
        if (!c.slot.isSet()) {
            s.notes << QString("clip %1 is no placeholder: nothing to remove").arg(id);
            return;
        }
        if (const QString err = s.editor.setClipSlot(id, {}); !err.isEmpty()) fail("BAD_ARGUMENT", err);
        s.notes << QString("clip %1 is no placeholder any more").arg(id);
        return;
    }
    const ProjectFormat f = s.project.format();
    ClipSlot slot = c.slot;
    if (op.contains("id")) slot.id = op.value("id").toString().trimmed();
    if (slot.id.isEmpty()) fail("BAD_ARGUMENT", "slot needs an id (the key `templates` fill takes, e.g. \"clip1\", \"hook\")");
    // A new mark of a title is a text slot, of an audio clip music, unless kind says otherwise
    TrackRef ref;
    TimelineOps::findClip(s.project.timeline(), id, &ref);
    SlotKind kind = c.slot.isSet() ? c.slot.kind : c.isTitle() ? SlotKind::Text
                                               : ref.kind == TrackKind::Audio ? SlotKind::Music : SlotKind::Clip;
    slot.kind = enumOf(kSlotKindIds, op, "kind", kind);
    if (op.contains("label")) slot.label = op.value("label").toString().trimmed();
    if (op.contains("hint")) slot.hint = op.value("hint").toString().trimmed();
    slot.length = enumOf(kSlotLengthIds, op, "length", slot.length);
    if (op.contains("min")) slot.minLen = std::max(0, timeOf(s, op, "min"));
    if (op.contains("max")) slot.maxLen = std::max(0, timeOf(s, op, "max"));
    slot.fit = enumOf(kSlotFitIds, op, "fit", slot.fit);
    slot.reframe = flagOf(op, "reframe", slot.reframe);
    if (op.contains("max_chars")) slot.maxChars = std::max(0, op.value("max_chars").toInt());
    slot.shrink = flagOf(op, "shrink", slot.shrink);
    slot.fitMusic = flagOf(op, "fit_music", slot.fitMusic);
    slot.snapCuts = flagOf(op, "snap_cuts", slot.snapCuts);
    if (const QString err = s.editor.setClipSlot(id, slot); !err.isEmpty()) fail("BAD_ARGUMENT", err);
    const Clip* now = TimelineOps::findClip(s.project.timeline(), id);
    const QJsonObject j = slotJson(now ? now->slot : slot, f);
    QStringList rules;
    for (auto it = j.begin(); it != j.end(); ++it)
        if (it.key() != "id" && it.key() != "kind")
            rules << QString("%1 %2").arg(it.key(), it.value().toVariant().toString());
    s.notes << QString("clip %1 is slot '%2' (%3%4)")
                   .arg(id)
                   .arg(slot.id, kSlotKindIds[int(slot.kind)], rules.isEmpty() ? QString() : ": " + rules.join(", "));
}

void opFill(Session& s, const QJsonObject& op)
{
    const QString id = op.value("slot").toString().trimmed();
    if (id.isEmpty()) fail("BAD_ARGUMENT", "fill needs slot (its id, see `templates` info)");
    if (!op.contains("value")) fail("BAD_ARGUMENT", "fill needs value: a file, {file, from?, to?} or a text");
    const ProjectData d = s.project.data();
    const TemplateFill::Values values = fillValues(QJsonObject{{id, op.value("value")}}, d, d.format);
    const QString reframe = op.value("reframe").toString("cached").trimmed().toLower();
    if (reframe != "cached" && reframe != "auto") fail("BAD_ARGUMENT", "reframe: \"cached\" or \"auto\"");
    QStringList notes;
    const TemplateFill::Context ctx = fillContext(d.format, op.value("_beats").toObject(), reframe == "auto", &notes);
    const TemplateFill::Report r = s.editor.fillSlot(id, values.value(id), ctx);
    if (!r.ok()) fail("FILL_FAILED", fillProblemsText(r.errors));
    for (const TemplateFill::SlotReport& x : r.results) {
        s.notes << QString("slot '%1': %2").arg(x.slot, x.text);
        for (const QString& w : x.warnings) s.notes << QString("slot '%1': %2").arg(x.slot, w);
    }
    for (const QString& n : notes) s.notes << n;
}

} // namespace

void addTemplateOps(QVector<OpDef>& ops)
{
    ops << OpDef{"slot",
                 "{op:'slot', clip, id, kind?, label?, hint?, length?, min?, max?, fit?, reframe?, max_chars?, shrink?, "
                 "fit_music?, snap_cuts?} / {op:'slot', clip, remove:true}",
                 "mark a clip as template placeholder (slot) with its rules, or unmark it",
                 "kind: clip (video/picture), text (a title's text), music (audio clip), logo; default from the clip. "
                 "length fixed (default) or flexible (takes the value's length within min..max, later clips move). "
                 "fit for a too short value: trim (default), speed, freeze, loop. reframe (clip): fit another shape "
                 "to the frame (default true). max_chars, shrink (text: smaller font when longer, default true). "
                 "fit_music (music: cut the song on its phrases to the slot, default true), snap_cuts. Only the given "
                 "rules change on a clip that is a slot already. `info` shows them as slot:{…}.",
                 opSlot};
    ops << OpDef{"fill", "{op:'fill', slot, value, reframe?}",
                 "fill a template placeholder: value = a file, {file, from?, to?} or a text, by the slot's rules",
                 "All instances of the slot (every timeline) get the value; trimmed, slowed, frozen, looped, reframed, "
                 "text shrunk as the slot says, the notes tell what was done. reframe: cached (default: auto where "
                 "the file was analysed, else center) or auto (measure now). Several slots at once: `templates` "
                 "with project + values.",
                 opFill, {"_beats"}};
}

} // namespace Cli::detail
