// SPDX-License-Identifier: AGPL-3.0-only
// Edit ops of the compositing graph (0.19, effect "comp" on a clip, core/CompGraph.h): the whole graph at once
// ('comp'), the compound comp of a selection ('comp_from'), single nodes and links ('node'), the tracker's analysis
// ('track_node'); plus what `effects kind:'nodes'`, `info`, `frames clip/node` and `check` show of graphs. The node
// kinds are the engine's (engine/picture/CompNodes.h); every change goes through an Editor op (one undo step), so the
// Comp page does the same.

#include "cli/CommandsDetail.h"

#include "core/CompGraph.h"
#include "core/EffectRegistry.h"
#include "core/ExportCheck.h"
#include "core/Keyframes.h"
#include "core/TimelineOps.h"
#include "engine/picture/CompNodes.h"
#include "engine/picture/CompTrack.h"

#include <QJsonArray>

#include <algorithm>

namespace Cli::detail {

namespace PC = Picture::Comp;

namespace {

const CompGraph::KindInfo kKinds = [](const QString& type, QStringList* inputs) { return PC::kindInfo(type, inputs); };

QString kindIds()
{
    QStringList ids;
    for (const PC::NodeType& k : PC::all()) ids << k.id;
    return ids.join(", ");
}

const PC::NodeType& kindOf(const QString& type, const QString& where)
{
    const PC::NodeType* k = PC::find(type);
    if (!k)
        fail("BAD_ARGUMENT", QString("%1: unknown node type '%2' (types: %3; `effects kind:'nodes'` lists them)")
                                 .arg(where, type, kindIds()));
    return *k;
}

const char* typeName(EffectParam::Type t)
{
    static const char* names[] = {"number", "color", "path", "boolean", "choice"};
    return names[t];
}

// Node values from JSON, checked against the kind's parameters (keys without the "n<id>." prefix)
QVariantMap nodeValues(Session& s, const PC::NodeType& k, const QJsonValue& v, const QString& where)
{
    QVariantMap out;
    if (v.isUndefined() || v.isNull()) return out;
    if (!v.isObject()) fail("BAD_ARGUMENT", where + ": 'params' must be an object {key: value}");
    const QJsonObject o = v.toObject();
    for (auto it = o.begin(); it != o.end(); ++it) {
        const QString key = it.key();
        const QString name = QString("%1.%2").arg(where, key);
        const EffectParam* p = k.param(key);
        // fx: the effect's own values under e_<key> (any value its effect takes)
        if (!p && k.id == QLatin1String("fx") && key.startsWith(QLatin1String("e_"))) {
            out[key] = it.value().toVariant();
            continue;
        }
        if (!p) {
            QStringList keys;
            for (const EffectParam& q : k.params)
                if (!q.sparse) keys << q.key;
            fail("BAD_ARGUMENT", QString("%1: node type %2 has no parameter '%3' (params: %4)")
                                     .arg(where, k.id, key, keys.isEmpty() ? QString("none") : keys.join(", ")));
        }
        const QJsonValue x = it.value();
        switch (p->type) {
        case EffectParam::Double: {
            if (x.isBool()) {
                out[key] = x.toBool() ? 1.0 : 0.0;
                break;
            }
            if (!x.isDouble()) fail("BAD_ARGUMENT", QString("'%1' must be a number").arg(name));
            const double d = x.toDouble();
            if (p->min < p->max && (d < p->min || d > p->max))
                fail("BAD_ARGUMENT", QString("'%1' must be between %2 and %3, got %4").arg(name).arg(p->min).arg(p->max).arg(d));
            out[key] = d;
            break;
        }
        case EffectParam::Color: out[key] = colorOf(x, "'" + name + "'").name(QColor::HexArgb); break;
        case EffectParam::Bool:
            if (!x.isBool() && !x.isDouble()) fail("BAD_ARGUMENT", QString("'%1' must be true or false").arg(name));
            out[key] = x.isBool() ? x.toBool() : x.toDouble() != 0;
            break;
        case EffectParam::Choice:
            if (!p->choices.contains(x.toString()))
                fail("BAD_ARGUMENT", QString("'%1' must be one of %2").arg(name, p->choices.join(", ")));
            out[key] = x.toString();
            break;
        case EffectParam::Path: {
            if (!x.isString()) fail("BAD_ARGUMENT", QString("'%1' must be a text").arg(name));
            QString text = x.toString();
            // The file of a media node is a media pool item (imported on first use)
            if (k.id == QLatin1String(::Comp::kMediaType) && key == QLatin1String(::Comp::kPathKey) && !text.isEmpty())
                text = s.media(absolute(text)).path;
            out[key] = text;
            break;
        }
        }
    }
    return out;
}

// [from, to, input?] or {from, to, input?}; a missing input = the first input of `to` nothing is linked into yet
// (unlink: the input `from` feeds)
CompLink linkOf(const QJsonValue& v, const CompGraph& g, const QString& where, bool unlink = false)
{
    CompLink l;
    QJsonValue input;
    if (v.isArray()) {
        const QJsonArray a = v.toArray();
        if (a.size() < 2 || a.size() > 3 || !a[0].isDouble() || !a[1].isDouble())
            fail("BAD_ARGUMENT", where + ": a link is [from, to, input?] (node ids, input name)");
        l.from = a[0].toInt();
        l.to = a[1].toInt();
        input = a.size() > 2 ? a[2] : QJsonValue();
    } else if (v.isObject()) {
        const QJsonObject o = v.toObject();
        l.from = o["from"].toInt();
        l.to = o["to"].toInt();
        input = o["input"];
    } else {
        fail("BAD_ARGUMENT", where + ": a link is [from, to, input?] (node ids, input name)");
    }
    const CompNode* to = g.node(l.to);
    if (!to) fail("NOT_FOUND", QString("%1: no node %2 in the graph").arg(where).arg(l.to));
    if (!g.node(l.from)) fail("NOT_FOUND", QString("%1: no node %2 in the graph").arg(where).arg(l.from));
    if (input.isString()) {
        l.input = input.toString();
        return l;
    }
    if (!input.isUndefined() && !input.isNull()) fail("BAD_ARGUMENT", where + ": the input is a name (\"src\", \"bg\", \"mask\" …)");
    if (unlink) {
        for (const CompLink& x : g.links)
            if (x.from == l.from && x.to == l.to) {
                l.input = x.input;
                return l;
            }
        fail("NOT_FOUND", QString("%1: node %2 is not linked into node %3").arg(where).arg(l.from).arg(l.to));
    }
    QStringList inputs;
    PC::kindInfo(to->type, &inputs);
    for (const QString& in : inputs)
        if (!g.linkInto(l.to, in)) {
            l.input = in;
            return l;
        }
    fail("BAD_ARGUMENT", QString("%1: node %2 (%3) has no free input; name one (inputs: %4)")
                             .arg(where).arg(l.to).arg(to->type, inputs.isEmpty() ? QString("none") : inputs.join(", ")));
}

struct Parsed {
    CompGraph graph;
    QHash<int, QVariantMap> values;
};

Parsed parseGraph(Session& s, const QJsonValue& v)
{
    if (!v.isObject()) fail("BAD_ARGUMENT", "'graph' must be an object {nodes:[{id, type, params?}], links:[[from, to, input?]], out?}");
    const QJsonObject o = v.toObject();
    for (auto it = o.begin(); it != o.end(); ++it)
        if (!QStringList{"nodes", "links", "out"}.contains(it.key()))
            fail("BAD_ARGUMENT", QString("graph: unknown key '%1' (nodes, links, out)").arg(it.key()));
    Parsed p;
    const QJsonArray nodes = o["nodes"].toArray();
    if (nodes.isEmpty()) fail("BAD_ARGUMENT", "graph: 'nodes' must be a non-empty list [{id, type, params?}]");
    int nextId = 1;
    for (const QJsonValue& nv : nodes) {
        const QJsonObject n = nv.toObject();
        for (auto it = n.begin(); it != n.end(); ++it)
            if (!QStringList{"id", "type", "params", "x", "y"}.contains(it.key()))
                fail("BAD_ARGUMENT", QString("graph node: unknown key '%1' (id, type, params, x, y)").arg(it.key()));
        CompNode node;
        node.id = n.contains("id") ? n["id"].toInt() : nextId;
        if (node.id < 1) fail("BAD_ARGUMENT", "graph node: 'id' must be a whole number >= 1");
        if (p.graph.node(node.id)) fail("BAD_ARGUMENT", QString("graph: node id %1 twice").arg(node.id));
        const QString where = QString("node %1").arg(node.id);
        node.type = kindOf(n["type"].toString(), where).id;
        node.pos = QPointF(n["x"].toDouble(160.0 * p.graph.nodes.size()), n["y"].toDouble(0));
        p.graph.nodes << node;
        p.values[node.id] = nodeValues(s, kindOf(node.type, where), n["params"], where);
        nextId = std::max(nextId, node.id + 1);
    }
    for (const QJsonValue& lv : o["links"].toArray()) {
        const CompLink l = linkOf(lv, p.graph, "graph link");
        p.graph.links << l;
    }
    p.graph.out = o.contains("out") ? o["out"].toInt() : p.graph.nodes.last().id;
    QString err;
    if (!p.graph.validate(kKinds, &err)) fail("BAD_ARGUMENT", "graph: " + err);
    return p;
}

CompGraph graphOfClip(const Clip& c, bool need = true)
{
    CompGraph g;
    const EffectInstance* e = EffectRegistry::instance(c, ::Comp::kEffectId);
    if ((!e || !::Comp::graphOf(*e, &g)) && need)
        fail("NOT_FOUND", QString("clip %1 has no compositing graph (make one with {op:'comp', graph} or {op:'node', add})").arg(c.id));
    return g;
}

void opComp(Session& s, const QJsonObject& op)
{
    // TODO(0.19 package I): recipes ('greenscreen', 'text_behind', 'blur_face', 'pip_frame', 'glitch_title',
    // 'sticker_track') build a graph here from op 'with' and hand it to Editor::setCompGraph like 'graph' does
    if (op.contains("recipe"))
        fail("NOT_SUPPORTED", "comp recipes are not in this version yet: build the graph with graph:{nodes, links, out} "
                              "or node by node with {op:'node', add}");
    const QVector<int> ids = clipsOn(s, op, TrackKind::Video, "a compositing graph");
    if (op.value("remove").toBool()) {
        const QVector<int> removed = s.editor.removeComp(ids);
        if (removed.isEmpty()) fail("NOT_FOUND", "none of the clips has a compositing graph");
        return;
    }
    if (!op.contains("graph")) fail("BAD_ARGUMENT", "comp: give graph:{nodes, links, out} or remove:true");
    const Parsed p = parseGraph(s, op.value("graph"));
    QString err;
    if (!s.editor.setCompGraph(ids, p.graph, p.values, kKinds, &err)) fail("FAILED", err);
}

void opCompFrom(Session& s, const QJsonObject& op)
{
    const QVector<int> ids = clipIds(s, op);
    s.selection.set(QSet<int>(ids.begin(), ids.end()));
    const int current = s.project.currentSequence();
    const int seq = s.editor.compFrom(op.value("name").toString());
    s.project.setCurrentSequence(current);
    if (!seq) fail("FAILED", "could not make a comp clip (no video clip among the clips, or they are locked)");
    int clip = 0;
    for (int id : s.selection.ids())
        if (TrackRef r; TimelineOps::findClip(s.project.timeline(), id, &r) && r.kind == TrackKind::Video) clip = id;
    const Clip* c = TimelineOps::findClip(s.project.timeline(), clip);
    QJsonObject note{{"clip", clip}, {"timeline", seq}};
    if (c) note["graph"] = compJson(*EffectRegistry::instance(*c, ::Comp::kEffectId), s.format());
    s.notes << QJsonObject{{"comp_from", note}};
}

void opNode(Session& s, const QJsonObject& op)
{
    const Clip& c = clipOf(s, op, "clip");
    const int clip = c.id;
    QString err;
    const auto done = [&](bool ok) {
        if (!ok) fail("FAILED", err);
    };
    if (op.contains("add")) {
        const QJsonObject add = op.value("add").toObject();
        for (auto it = add.begin(); it != add.end(); ++it)
            if (it.key() != "type" && it.key() != "params")
                fail("BAD_ARGUMENT", QString("node add: unknown key '%1' (type, params)").arg(it.key()));
        const PC::NodeType& k = kindOf(add["type"].toString(), "node add");
        const QVariantMap values = nodeValues(s, k, add["params"], k.id);
        const CompGraph g = graphOfClip(c, false);
        const int after = op.value("after").toInt();
        if (after && !g.node(after)) fail("NOT_FOUND", QString("no node %1 in the graph of clip %2").arg(after).arg(clip));
        QString input = op.value("input").toString();
        if (!op.contains("input") && !k.inputs.isEmpty()) input = k.inputs.first(); // merge: the chain is its bg
        if (op.value("input").isBool() && !op.value("input").toBool()) input.clear(); // input:false = not linked
        const int id = s.editor.addCompNode(clip, k.id, values, after, input, kKinds, &err);
        done(id > 0);
        s.notes << QJsonObject{{"node", QJsonObject{{"clip", clip}, {"id", id}, {"type", k.id},
                                                    {"graph", compJson(*EffectRegistry::instance(
                                                                  *TimelineOps::findClip(s.project.timeline(), clip),
                                                                  ::Comp::kEffectId), s.format())}}}};
        return;
    }
    const CompGraph g = graphOfClip(c);
    if (op.contains("link") || op.contains("unlink")) {
        const bool on = op.contains("link");
        const CompLink l = linkOf(op.value(on ? "link" : "unlink"), g, on ? "link" : "unlink", !on);
        done(s.editor.linkCompNodes(clip, l, on, kKinds, &err));
        return;
    }
    if (!op.contains("id"))
        fail("BAD_ARGUMENT", "node: give add:{type, params?}, link/unlink:[from, to, input], or id with params, out:true or remove:true");
    const int id = op.value("id").toInt();
    const CompNode* n = g.node(id);
    if (!n) fail("NOT_FOUND", QString("no node %1 in the graph of clip %2").arg(id).arg(clip));
    if (op.value("remove").toBool()) {
        done(s.editor.removeCompNode(clip, id, kKinds, &err));
        return;
    }
    bool any = false;
    if (op.contains("params")) {
        const QVariantMap values = nodeValues(s, kindOf(n->type, QString("node %1").arg(id)), op.value("params"),
                                              QString("node %1").arg(id));
        if (!values.isEmpty()) done(s.editor.setCompNodeValues(clip, id, values, &err));
        any = true;
    }
    if (op.value("out").toBool()) {
        done(s.editor.setCompOut(clip, id, kKinds, &err));
        any = true;
    }
    if (!any) fail("BAD_ARGUMENT", "node: nothing to do (params, out:true or remove:true)");
}

void opTrackNode(Session& s, const QJsonObject& op)
{
    const Clip& c = clipOf(s, op, "clip");
    const Clip clip = c; // the timeline changes below
    if (!op.contains("node")) fail("BAD_ARGUMENT", "missing 'node' (id of a tracker node)");
    const int node = op.value("node").toInt();
    const CompGraph g = graphOfClip(clip);
    const CompNode* n = g.node(node);
    if (!n) fail("NOT_FOUND", QString("no node %1 in the graph of clip %2").arg(node).arg(clip.id));
    if (n->type != QLatin1String(::Comp::kTrackerType))
        fail("BAD_ARGUMENT", QString("node %1 is a %2 node, not a tracker").arg(node).arg(n->type));
    // Timeline frames in, clip frames for the analysis (backwards when to < from)
    const int last = clip.length() - 1;
    const int from = std::clamp(timeOf(s, op, "from", clip.start) - clip.start, 0, last);
    const int to = std::clamp(timeOf(s, op, "to", clip.start + last) - clip.start, 0, last);
    const PC::TrackRun r = PC::trackNode(s.format(), clip, node, from, to);
    if (!r.error.isEmpty()) fail("FAILED", r.error);
    if (r.points.isEmpty()) fail("FAILED", QString("nothing tracked for node %1").arg(node));
    if (!s.editor.trackCompNode(clip.id, node, r.points)) fail("FAILED", "could not store the tracking result");
    QJsonObject note{{"clip", clip.id}, {"node", node}, {"from", clip.start + from}, {"to", clip.start + to},
                     {"points", int(r.points.size())}, {"lost_frames", int(r.lostFrames.size())},
                     {"gave_up", r.gaveUp}, {"outliers", r.outliers}};
    note["lost_at"] = r.lostAt >= 0 ? QJsonValue(clip.start + r.lostAt) : QJsonValue();
    note["found_at"] = r.foundAt >= 0 ? QJsonValue(clip.start + r.foundAt) : QJsonValue();
    if (r.gaveUp)
        note["hint"] = "lost for too long: the track stops there; start a new track_node from a frame where it is "
                       "visible again (from), or move the start points (ax, ay)";
    s.notes << QJsonObject{{"track_node", note}};
}

} // namespace

QJsonArray compNodeKindsJson()
{
    QJsonArray list;
    for (const PC::NodeType& k : PC::all()) {
        QJsonObject o{{"type", k.id}, {"name", k.name}, {"output", k.output == PC::Buf::Mask ? "mask" : "picture"},
                      {"inputs", QJsonArray::fromStringList(k.inputs)}};
        if (!k.description.isEmpty()) o["description"] = k.description;
        if (k.varying) o["varying"] = true; // changes every frame (not cached)
        QJsonArray params;
        QStringList more;
        for (const EffectParam& p : k.params) {
            if (p.sparse) { // the many optional values (mask points 5..32, bezier handles)
                more << p.key;
                continue;
            }
            QJsonObject po{{"key", p.key}, {"type", typeName(p.type)}, {"label", p.label}};
            if (p.type == EffectParam::Double) {
                po["min"] = p.min;
                po["max"] = p.max;
            }
            if (p.type == EffectParam::Choice) po["choices"] = QJsonArray::fromStringList(p.choices);
            po["default"] = p.type == EffectParam::Color ? QJsonValue(p.defaultValue.value<QColor>().name(QColor::HexArgb))
                                                         : QJsonValue::fromVariant(p.defaultValue);
            if (!p.description.isEmpty()) po["description"] = p.description;
            params << po;
        }
        o["params"] = params;
        if (!more.isEmpty()) o["more_params"] = QString("%1 more of the same kind (%2 … %3)").arg(more.size()).arg(more.first(), more.last());
        if (k.id == QLatin1String("fx")) o["note"] = "the effect's own values as e_<key> (see `effects effect:<id>`)";
        if (k.id == QLatin1String(::Comp::kTrackerType))
            o["note"] = "ax, ay (bx, by) = start points as 0..1 of the CLIP's own picture (without letterbox bars) at "
                        "the start frame; edit op track_node writes x, y, scale, angle as keyframes and rx, ry (the "
                        "reference point); followers (follow = this id) move by (x - rx, y - ry) and scale/turn around (x, y)";
        list << o;
    }
    return list;
}

QJsonObject compJson(const EffectInstance& e, const ProjectFormat& f)
{
    CompGraph g;
    if (!::Comp::graphOf(e, &g)) return {{"broken", true}};
    QHash<int, QJsonObject> values;
    for (auto it = e.params.begin(); it != e.params.end(); ++it) {
        int id = 0;
        QString key;
        if (CompGraph::splitParamKey(it.key(), &id, &key))
            values[id][key] = it.value().typeId() == QMetaType::QColor
                                  ? QJsonValue(it.value().value<QColor>().name(QColor::HexArgb))
                                  : QJsonValue::fromVariant(it.value());
    }
    QJsonArray nodes, links;
    for (const CompNode& n : g.nodes) {
        QJsonObject o{{"id", n.id}, {"type", n.type}};
        if (!values.value(n.id).isEmpty()) o["params"] = values.value(n.id);
        if (!PC::find(n.type)) o["available"] = false; // a newer version's kind: passed through
        nodes << o;
    }
    for (const CompLink& l : g.links) links << QJsonArray{l.from, l.to, l.input};
    QJsonObject o{{"nodes", nodes}, {"links", links}, {"out", g.out}};
    QJsonArray unused;
    const QVector<int> order = g.evalOrder();
    for (const CompNode& n : g.nodes)
        if (!order.contains(n.id)) unused << n.id;
    if (!unused.isEmpty()) o["unused"] = unused; // do not reach the output
    o["cost_ms"] = std::round(ExportCheck::compCostMs(e, f.size()) * 10) / 10; // rough, one core
    return o;
}

void checkCompParam(Session& s, const QVector<int>& ids, AnimParam p, double* lo, double* hi)
{
    QString effectId, full;
    if (!Keys::effectParamOf(p, &effectId, &full) || effectId != QLatin1String(::Comp::kEffectId)) return;
    int node = 0;
    QString key;
    CompGraph::splitParamKey(full, &node, &key);
    *lo = -1e300;
    *hi = 1e300;
    for (int id : ids) {
        const Clip* c = TimelineOps::findClip(s.project.timeline(), id);
        if (!c) continue;
        const CompGraph g = graphOfClip(*c);
        const CompNode* n = g.node(node);
        if (!n) fail("NOT_FOUND", QString("comp.%1: no node %2 in the graph of clip %3").arg(full).arg(node).arg(id));
        const PC::NodeType* k = PC::find(n->type);
        const EffectParam* ep = k ? k->param(key) : nullptr;
        if (k && !ep && !(k->id == QLatin1String("fx") && key.startsWith(QLatin1String("e_"))))
            fail("BAD_ARGUMENT", QString("comp.%1: node %2 (%3) has no parameter '%4'").arg(full).arg(node).arg(n->type, key));
        if (ep && ep->type != EffectParam::Double)
            fail("BAD_ARGUMENT", QString("comp.%1: only numbers have keyframes (%2 is a %3)").arg(full, key, typeName(ep->type)));
        if (ep && ep->min < ep->max) {
            *lo = std::max(*lo, ep->min);
            *hi = std::min(*hi, ep->max);
        }
    }
}

Timeline compNodeTimeline(Session& s, const QJsonObject& a, int* start, int* end)
{
    Timeline tl = s.project.renderTimeline();
    const int clipId = a.value("clip").toInt();
    Clip target;
    bool found = false;
    for (Track& t : tl.video)
        for (const Clip& c : t.clips)
            if (c.id == clipId) {
                target = c;
                found = true;
            }
    if (!found) fail("NOT_FOUND", QString("no video clip with id %1 in the timeline").arg(clipId));
    const QString view = a.value("view").toString();
    if (!view.isEmpty() && view != "matte") fail("BAD_ARGUMENT", "view: 'matte' (alpha as gray) or leave it out");
    if (a.contains("node") || !view.isEmpty()) {
        EffectInstance* e = EffectRegistry::instance(target, ::Comp::kEffectId);
        CompGraph g;
        if (!e || !::Comp::graphOf(*e, &g)) fail("NOT_FOUND", QString("clip %1 has no compositing graph").arg(clipId));
        const int node = a.contains("node") ? a.value("node").toInt() : g.out;
        const CompNode* n = g.node(node);
        if (!n) fail("NOT_FOUND", QString("no node %1 in the graph of clip %2").arg(node).arg(clipId));
        const PC::NodeType* k = PC::find(n->type);
        int shown = node;
        if (k && k->output == PC::Buf::Rgba && view == "matte") { // its alpha as a mask
            const int m = g.nextId();
            g.nodes << CompNode{m, "matte", {}};
            g.links << CompLink{node, m, "src"};
            e->params[CompGraph::paramKey(m, "channel")] = "alpha";
            shown = m;
        }
        if (k && (k->output == PC::Buf::Mask || shown != node)) { // a mask as gray: white on black through it
            const int white = g.nextId(), black = white + 1, merge = white + 2;
            g.nodes << CompNode{white, "color", {}} << CompNode{black, "color", {}} << CompNode{merge, "merge", {}};
            g.links << CompLink{black, merge, "bg"} << CompLink{white, merge, "fg"} << CompLink{shown, merge, "mask"};
            e->params[CompGraph::paramKey(white, "color")] = "#ffffffff";
            e->params[CompGraph::paramKey(black, "color")] = "#ff000000";
            shown = merge;
        }
        g.out = shown;
        ::Comp::setGraph(*e, g);
    }
    // The clip alone, as the graph makes it: no other clips, no transform, fades or transitions over it
    for (Track& t : tl.video) t.clips.clear();
    for (Track& t : tl.audio) t.clips.clear();
    tl.subtitles.clear();
    for (int p = int(AnimParam::ZoomX); p <= int(AnimParam::Opacity); ++p) target.keys.remove(AnimParam(p));
    target.transform = ClipTransform{};
    target.fadeIn = target.fadeOut = target.transIn = target.transOut = 0;
    if (!tl.video.isEmpty()) {
        tl.video[0].clips = {target};
        tl.video[0].hidden = false;
    }
    *start = target.start;
    *end = target.end();
    return tl;
}

void addCompOps(QVector<OpDef>& ops)
{
    ops << OpDef{"comp", "{op:'comp', clips, graph:{nodes:[{id, type, params?}], links:[[from, to, input?]], out?}}",
                 "compositing graph on clips: nodes (picture, mask, key, merge, text …) linked into one picture; "
                 "{op:'comp', clips, remove:true} takes it off",
                 "The graph replaces the clip's picture (after its other effects, before crop/transform). nodes: id "
                 "(whole number >= 1, default: in order), type (see `effects kind:'nodes'`), params {key: value} "
                 "(numbers in their range, colors \"#rrggbb\"/\"#aarrggbb\", choices by id; missing = default). links: "
                 "[from, to, input]: node `from`'s picture goes into input `input` of node `to` (merge: bg, fg, mask; "
                 "most others: src, mask); without input the first free one. out: the node whose picture is the "
                 "result (default: the last). Errors name the node: unknown type, an input the type does not have, a "
                 "cycle. Node values are animated with {op:'keyframe', clips, param:'comp.n3.opacity', at, value}. "
                 "Example (green screen over a background file, a title on top): "
                 R"({"op":"comp","clips":[4],"graph":{"nodes":[{"id":1,"type":"clip"},{"id":2,"type":"key","params":{"color":"#00ff00"}},{"id":3,"type":"media","params":{"path":"/videos/beach.jpg"}},{"id":4,"type":"merge"},{"id":5,"type":"text","params":{"text":"Hello"}},{"id":6,"type":"merge"}],"links":[[1,2,"src"],[3,4,"bg"],[2,4,"fg"],[4,6,"bg"],[5,6,"fg"]],"out":6}}. )"
                 "Look at a node's picture with `frames` {clip, node, view:'matte'} (is the key clean?). recipe "
                 "(ready graphs) comes in a later version.",
                 opComp, QStringList{"clip", "linked", "remove", "recipe", "with"}}
        << OpDef{"comp_from", "{op:'comp_from', clips, name?}",
                 "the clips as one comp clip: a compound clip whose graph lays its tracks on each other (same picture)",
                 "Like {op:'compound'}, but the new clip carries a graph: a 'clip' node per inner video track (param "
                 "track) and a chain of normal merges, so the picture stays exactly the same and the chain can be "
                 "rebuilt with 'node' ops (put a key, mask or blur between). The answer's notes name the new clip "
                 "and its graph.",
                 opCompFrom, kClipKeys}
        << OpDef{"node", "{op:'node', clip, add:{type, params?}, after?, input?}",
                 "one node of a clip's graph: add (answer: its id), {op:'node', clip, id, params} changes values, "
                 "{op:'node', clip, id, remove:true}, {op:'node', clip, link:[from, to, input]} / "
                 "{op:'node', clip, unlink:[from, to, input]}, {op:'node', clip, id, out:true}",
                 "add: a node of `type` (see `effects kind:'nodes'`) goes behind node `after` (default: the output "
                 "node): `after` feeds its input `input` (default: its first, merge: bg) and what read `after` reads "
                 "the new node; input:false only adds it (a text or color to link yourself). A clip without a graph "
                 "gets one ('clip' node first). The answer's notes give the new id and the graph. remove: what read "
                 "the node reads its first input instead (a chain stays a chain). link replaces what was in that "
                 "input; a cycle or an input the type does not have is an error. Example: blur only in a mask - "
                 R"({"op":"node","clip":4,"add":{"type":"blur","params":{"radius":12}}} then {"op":"node","clip":4,"add":{"type":"mask","params":{"shape":"ellipse"}},"input":false} and {"op":"node","clip":4,"link":[3,2,"mask"]}.)",
                 opNode, QStringList{"id", "params", "remove", "link", "unlink", "out"}}
        << OpDef{"track_node", "{op:'track_node', clip, node, from?, to?}",
                 "run a tracker node: follows its point(s) or a face over the clip; masks/transforms with follow:<id> "
                 "go along",
                 "node = id of a 'tracker' node (mode point|two|face, start points ax, ay, bx, by at frame `from`, as 0..1 "
                 "of the clip's own picture without letterbox bars). "
                 "from/to = timeline times inside the clip (default: the whole clip; to before from = backwards). "
                 "The result becomes keyframes of the node's x, y, scale, angle (replacing those in the range) and its "
                 "reference point rx, ry; followers move by (x - rx, y - ry). The "
                 "notes say points, lost_at/found_at (timeline frames where it lost/found the point again), gave_up "
                 "(lost too long: stopped) and outliers (two points: frames that used one). Face mode needs the "
                 "'faces' extension.",
                 opTrackNode, QStringList{"clips"}};
}

} // namespace Cli::detail
