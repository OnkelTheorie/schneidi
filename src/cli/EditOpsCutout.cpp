// SPDX-License-Identifier: AGPL-3.0-only
// AI cutout through the CLI (0.19): op 'cutout_node' runs the person segmentation (engine/PersonMatte, extension
// "cutout") over a clip once and points the cutout node at its mattes (Editor::cutoutCompNode, one undo step).
#include "cli/CommandsDetail.h"

#include "core/CompGraph.h"
#include "core/EffectRegistry.h"
#include "core/TimelineOps.h"
#include "engine/picture/CompCutout.h"

#include <QJsonObject>

#include <algorithm>
#include <cmath>

namespace Cli::detail {

namespace PC = Picture::Comp;

namespace {

void opCutoutNode(Session& s, const QJsonObject& op)
{
    const Clip clip = clipOf(s, op, "clip"); // a copy: the timeline changes below
    if (!op.contains("node")) fail("BAD_ARGUMENT", "missing 'node' (id of a cutout node)");
    const int node = op.value("node").toInt();
    CompGraph g;
    const EffectInstance* e = EffectRegistry::instance(clip, ::Comp::kEffectId);
    if (!e || !::Comp::graphOf(*e, &g))
        fail("NOT_FOUND", QString("clip %1 has no compositing graph (add a cutout node with {op:'node', add:{type:'cutout'}})")
                              .arg(clip.id));
    const CompNode* n = g.node(node);
    if (!n) fail("NOT_FOUND", QString("no node %1 in the graph of clip %2").arg(node).arg(clip.id));
    if (n->type != QLatin1String(PC::kCutoutType))
        fail("BAD_ARGUMENT", QString("node %1 is a %2 node, not a cutout").arg(node).arg(n->type));
    // Timeline frames in, clip frames for the analysis
    const int last = clip.length() - 1;
    const int from = std::clamp(timeOf(s, op, "from", clip.start) - clip.start, 0, last);
    const int to = std::clamp(timeOf(s, op, "to", clip.start + last) - clip.start, 0, last);
    const PC::CutoutRun r = PC::cutoutNode(s.format(), clip, node, from, to);
    if (!r.error.isEmpty()) {
        if (r.error.startsWith(PersonMatte::needsExtension()))
            fail("NEEDS_EXTENSION", r.error + " - the extension 'cutout' (`extensions` {install:'cutout'})");
        fail("FAILED", r.error);
    }
    if (r.frames <= 0) fail("FAILED", QString("nothing analysed for node %1").arg(node));
    QString err;
    if (!s.editor.cutoutCompNode(clip.id, node, r.cacheId, &err)) fail("FAILED", "could not store the analysis: " + err);
    s.notes << QJsonObject{{"cutout_node", QJsonObject{{"clip", clip.id}, {"node", node}, {"from", clip.start + r.from},
                                                       {"to", clip.start + r.to}, {"frames", r.frames},
                                                       {"cache", r.cacheId},
                                                       {"ms_per_frame", std::round(r.msPerFrame * 10) / 10}}}};
}

} // namespace

void addCutoutOps(QVector<OpDef>& ops)
{
    ops << OpDef{"cutout_node", "{op:'cutout_node', clip, node, from?, to?}",
                 "AI cutout: finds the person in a clip once so a 'cutout' node can cut them out - text behind a "
                 "person without a green screen",
                 "node = id of a 'cutout' node (input src = the clip's picture). Runs a person segmentation model "
                 "over the clip (from/to = timeline times inside the clip, default: the whole clip) and stores small, "
                 "flicker-smoothed mattes in a cache; the node then refines them along the picture's edges every "
                 "frame. Without this analysis (or outside its range) the node shows nothing. Node values: threshold "
                 "(0..1, where the edge lies, default 0.5), soft and grow (project pixels), invert (keep the "
                 "background), output picture|mask. Needs the 'cutout' extension (and ONNX Runtime). Text behind a "
                 "person: nodes clip(1), text(2), merge(3) with bg 1 and fg 2, cutout(4) with src 1, merge(5) with bg "
                 "3 and fg 4, then "
                 R"({"op":"cutout_node","clip":4,"node":4}. )"
                 "The notes say frames, cache and ms_per_frame.",
                 opCutoutNode, QStringList{"clips"}};
}

} // namespace Cli::detail
