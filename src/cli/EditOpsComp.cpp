// SPDX-License-Identifier: AGPL-3.0-only
// Edit ops of the compositing graph (0.19, effect "comp" on a clip, core/CompGraph.h): the whole graph at once
// ('comp'), the compound comp of a selection ('comp_from'), single nodes and links ('node'), the tracker's analysis
// ('track_node'); plus what `effects kind:'nodes'`, `info`, `frames clip/node` and `check` show of graphs. The node
// kinds are the engine's (engine/picture/CompNodes.h); every change goes through an Editor op (one undo step), so the
// Comp page does the same.

#include "cli/CommandsDetail.h"

#include "core/CodeTrust.h"
#include "core/CompGraph.h"
#include "core/CompRecipes.h"
#include "core/CompTrack.h"
#include "core/EffectRegistry.h"
#include "core/ExportCheck.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/TimelineOps.h"
#include "engine/ColorGrade.h"
#include "engine/picture/CompCutout.h"
#include "engine/picture/CompNodes.h"
#include "engine/picture/CompTrack.h"
#include "engine/picture/CustomEffects.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QUndoStack>

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

// Node values being read are a recipe's (recipeGraph): its custom code is not the caller's own
thread_local bool t_recipeValues = false;

const char* typeName(EffectParam::Type t)
{
    static const char* names[] = {"number", "color", "path", "boolean", "choice", "text"};
    return names[t];
}

// Node values from JSON, checked against the kind's parameters (keys without the "n<id>." prefix). fx: the
// effect's own values as e_<key>, checked against that effect (given in params, else `fxEffect` = the node's now)
QVariantMap nodeValues(Session& s, const PC::NodeType& k, const QJsonValue& v, const QString& where,
                       const QString& fxEffect = {})
{
    QVariantMap out;
    if (v.isUndefined() || v.isNull()) return out;
    if (!v.isObject()) fail("BAD_ARGUMENT", where + ": 'params' must be an object {key: value}");
    const QJsonObject o = v.toObject();
    const bool fx = k.id == QLatin1String("fx");
    const QString effectId = fx && o.value("effect").isString() ? o.value("effect").toString() : fxEffect;
    const EffectDescriptor* effect = fx && !effectId.isEmpty() ? EffectRegistry::find(effectId) : nullptr;
    for (auto it = o.begin(); it != o.end(); ++it) {
        const QString key = it.key();
        const QString name = QString("%1.%2").arg(where, key);
        const EffectParam* p = k.param(key);
        if (!p && fx && key.startsWith(QLatin1String("e_"))) {
            const EffectParam* ep = nullptr;
            if (effect)
                ep = EffectRegistry::findParam(*effect, key.mid(2)); // stored key or readable alias
            if (effect && !ep) {
                QStringList keys;
                for (const EffectParam& q : effect->params) keys << "e_" + q.name();
                fail("BAD_ARGUMENT", QString("%1: effect %2 has no value '%3' (values: %4)")
                                         .arg(where, effectId, key.mid(2), keys.isEmpty() ? QString("none") : keys.join(", ")));
            }
            // Numbers in the effect's range, switches as true/false; other values as the effect takes them
            if (ep && ep->type == EffectParam::Double) {
                if (!it.value().isDouble()) fail("BAD_ARGUMENT", QString("'%1' must be a number").arg(name));
                const double d = it.value().toDouble();
                if (ep->min < ep->max && (d < ep->min || d > ep->max))
                    fail("BAD_ARGUMENT", QString("'%1' must be between %2 and %3, got %4").arg(name).arg(ep->min).arg(ep->max).arg(d));
            } else if (ep && ep->type == EffectParam::Bool && !it.value().isBool()) {
                fail("BAD_ARGUMENT", QString("'%1' must be true or false").arg(name));
            } else if (ep && ep->type == EffectParam::Color) {
                out["e_" + ep->key] = colorOf(it.value(), "'" + name + "'").name(QColor::HexArgb);
                continue;
            }
            out[ep ? "e_" + ep->key : key] = it.value().toVariant(); // the stored key, also when given by alias
            // A custom effect's code (filter chain, shader) is checked here, as in op 'effect'
            QString why;
            if (key == QLatin1String("e_chain")) why = Picture::Custom::checkFilter(it.value().toString(), s.format().size());
            else if (key == QLatin1String("e_code")) why = Picture::Custom::checkShader(it.value().toString());
            if (!why.isEmpty()) fail("BAD_ARGUMENT", QString("%1: %2").arg(name, why));
            // Written by the caller: trusted in this run, unless it is the project's untrusted code sent back; a
            // recipe's code only through its own record (CompRecipes::apply)
            if ((key == QLatin1String("e_chain") || key == QLatin1String("e_code")) && !t_recipeValues)
                CodeTrust::trustWritten(it.value().toString(), s.project.data());
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
        case EffectParam::Text:
            if (!x.isString()) fail("BAD_ARGUMENT", QString("'%1' must be a text").arg(name));
            out[key] = x.toString();
            break;
        case EffectParam::Path: {
            if (!x.isString()) fail("BAD_ARGUMENT", QString("'%1' must be a text").arg(name));
            QString text = x.toString();
            if (!text.isEmpty() && ::Comp::fileKeys(k.id).contains(key)) { // a file (empty = none)
                if (k.id == QLatin1String(::Comp::kMediaType) && key == QLatin1String(::Comp::kPathKey)) {
                    text = s.media(absolute(text)).path; // a media pool item (imported on first use)
                } else if (k.id == QLatin1String(::Comp::kGradeType) && key == QLatin1String(::Comp::kLutKey)) {
                    text = findAsset(Asset::Lut, text); // by name as op 'look', or a file
                    QString error;
                    if (!ColorGrade::parseLut(text, &error))
                        fail("BAD_ARGUMENT", QString("'%1': cannot read the LUT: %2").arg(name, error));
                } else {
                    text = absolute(text);
                }
                if (!QFileInfo(text).isFile() || !QFileInfo(text).isReadable())
                    fail("NOT_FOUND", QString("'%1': cannot read the file '%2'").arg(name, text));
            }
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

// The effect an existing fx node wraps (empty: none or another kind)
QString fxEffectOf(const Clip& c, int node)
{
    const EffectInstance* e = EffectRegistry::instance(c, ::Comp::kEffectId);
    return e ? e->params.value(CompGraph::paramKey(node, "effect")).toString() : QString();
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

// A recipe (core/CompRecipes.h) with its slots filled from `with`, every value checked like a graph given by hand
Parsed recipeGraph(Session& s, const QJsonObject& op)
{
    QString err;
    const std::optional<CompRecipe> r = CompRecipes::find(op.value("recipe").toString(), &err);
    if (!r) fail("NOT_FOUND", err);
    const QJsonValue w = op.value("with");
    if (!w.isUndefined() && !w.isNull() && !w.isObject())
        fail("BAD_ARGUMENT", "'with' must be an object {slot: value} (`effects kind:'recipes'` lists the slots)");
    QVariantMap with = w.toObject().toVariantMap();
    for (const CompRecipeSlot& slot : r->slotList) // files of the caller: absolute
        if ((slot.type == QLatin1String("media") || slot.type == QLatin1String("file")) && with.value(slot.id).typeId() == QMetaType::QString
            && !with.value(slot.id).toString().isEmpty())
            with[slot.id] = absolute(with.value(slot.id).toString());
    Parsed p;
    QHash<int, QVariantMap> values;
    if (!CompRecipes::apply(*r, with, &p.graph, &values, &err)) fail("BAD_ARGUMENT", err);
    struct RecipeValues {
        RecipeValues() { t_recipeValues = true; }
        ~RecipeValues() { t_recipeValues = false; }
    } recipeValues;
    for (const CompNode& n : p.graph.nodes) {
        const QString where = QString("recipe %1 node %2").arg(r->id).arg(n.id);
        p.values[n.id] = nodeValues(s, kindOf(n.type, where), QJsonObject::fromVariantMap(values.value(n.id)), where);
    }
    if (!p.graph.validate(kKinds, &err)) fail("BAD_ARGUMENT", QString("recipe %1: %2").arg(r->id, err));
    return p;
}

// {op:'comp', clips:[one], save_recipe:'name', description?, replace?}: the clip's graph into the own recipes
void saveRecipe(Session& s, const QJsonObject& op)
{
    const QVector<int> ids = clipsOn(s, op, TrackKind::Video, "a compositing graph");
    if (ids.size() != 1) fail("BAD_ARGUMENT", "save_recipe: give one clip");
    const Clip* c = TimelineOps::findClip(s.project.timeline(), ids.first());
    graphOfClip(*c);
    const QString name = op.value("save_recipe").toString().trimmed();
    if (name.isEmpty()) fail("BAD_ARGUMENT", "save_recipe: give the recipe's name");
    QJsonObject note{{"clip", c->id}, {"name", name}, {"id", CompRecipes::idForName(name)}};
    if (s.dryRun) {
        note["saved"] = false;
        s.notes << QJsonObject{{"save_recipe", note}};
        return;
    }
    CompRecipes::SaveOptions o;
    o.description = op.value("description").toString();
    o.replace = op.value("replace").toBool();
    QString err, code;
    const std::optional<CompRecipe> r = CompRecipes::save(*EffectRegistry::instance(*c, ::Comp::kEffectId), name, o, &err, &code);
    if (!r) fail(code, err);
    note["id"] = r->id;
    note["folder"] = QDir::toNativeSeparators(r->dir);
    note["slots"] = recipeSlotsJson(*r);
    s.notes << QJsonObject{{"save_recipe", note}};
}

void opComp(Session& s, const QJsonObject& op)
{
    if (op.contains("save_recipe")) {
        if (op.contains("recipe") || op.contains("graph") || op.contains("remove"))
            fail("BAD_ARGUMENT", "save_recipe saves the clip's graph as it is: give it alone (without recipe, graph, remove)");
        saveRecipe(s, op);
        return;
    }
    if (op.contains("recipe")) {
        if (op.contains("graph") || op.contains("remove")) fail("BAD_ARGUMENT", "comp: give recipe, graph or remove, not several");
        const QVector<int> ids = clipsOn(s, op, TrackKind::Video, "a compositing graph");
        const Parsed p = recipeGraph(s, op);
        QString err;
        if (!s.editor.setCompGraph(ids, p.graph, p.values, kKinds, &err)) fail("FAILED", err);
        const std::optional<CompRecipe> r = CompRecipes::find(op.value("recipe").toString());
        QJsonObject note{{"recipe", r->id}, {"clips", QJsonArray::fromVariantList([&] {
                                                QVariantList l;
                                                for (int id : ids) l << id;
                                                return l;
                                            }())}};
        if (!r->note.isEmpty()) note["next"] = r->note;
        if (const Clip* c = TimelineOps::findClip(s.project.timeline(), ids.first()))
            if (const EffectInstance* e = EffectRegistry::instance(*c, ::Comp::kEffectId)) note["graph"] = compJson(*e, s.format(), c);
        s.notes << QJsonObject{{"comp_recipe", note}};
        return;
    }
    if (op.contains("with")) fail("BAD_ARGUMENT", "'with' fills the slots of a recipe: give recipe too");
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
    if (c) note["graph"] = compJson(*EffectRegistry::instance(*c, ::Comp::kEffectId), s.format(), c);
    s.notes << QJsonObject{{"comp_from", note}};
}

// {op:'node', clip, id, range:[from, to]}: the node works in timeline frames [from, to) of the clip (stored as seconds
// from the clip's start, Comp::rangeValues); range:false = the whole clip again
void opNodeRange(Session& s, const QJsonObject& op, const Clip& c, const CompNode& n)
{
    if (op.contains("params") || op.contains("out") || op.contains("link") || op.contains("unlink"))
        fail("BAD_ARGUMENT", "node: give range alone (then params/out in another op)");
    if (!::Comp::hasRange(n.type))
        fail("BAD_ARGUMENT", QString("node %1 (%2) has no time range: masks and trackers work over the whole clip")
                                 .arg(n.id).arg(n.type));
    const double fps = s.format().rate.fps();
    const QJsonValue v = op.value("range");
    double from = 0, until = 0;
    if (!(v.isNull() || (v.isBool() && !v.toBool()))) {
        const QJsonArray r = v.toArray();
        if (r.size() != 2)
            fail("BAD_ARGUMENT", "node: range is [from, to] in timeline times (frames, \"4.5s\", timecode), to "
                                 "exclusive; range:false = the whole clip");
        const QJsonObject pair{{"from", r[0]}, {"to", r[1]}};
        const int a = timeOf(s, pair, "from"), b = timeOf(s, pair, "to");
        if (b <= a) fail("BAD_ARGUMENT", "node: range 'to' must be after 'from'");
        if (a < c.start || b > c.end())
            fail("BAD_ARGUMENT", QString("node %1: range must lie inside clip %2 (%3..%4, to exclusive), got %5..%6")
                                     .arg(n.id).arg(c.id).arg(c.start).arg(c.end()).arg(a).arg(b));
        if (a > c.start || b < c.end()) ::Comp::rangeValues(c, a, b, fps, &from, &until); // the whole clip = no range
    }
    QString err;
    if (!s.editor.setCompNodeRange(c.id, n.id, from, until, &err)) fail("FAILED", err);
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
        const QJsonValue in = op.value("input");
        if (!in.isUndefined() && !in.isString() && !(in.isBool() && !in.toBool()))
            fail("BAD_ARGUMENT", QString("node add: input is the name of an input of %1 (%2) or false (not linked)")
                                     .arg(k.id, k.inputs.isEmpty() ? QString("none") : k.inputs.join(", ")));
        QString input = in.toString();
        if (in.isUndefined() && !k.inputs.isEmpty()) input = k.inputs.first(); // merge: the chain is its bg
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
    if (op.contains("range")) { // the time range in timeline times (0.22)
        opNodeRange(s, op, c, *n);
        return;
    }
    // params and out:true together are one undo step
    struct Macro {
        QUndoStack* undo = nullptr;
        ~Macro()
        {
            if (undo) undo->endMacro();
        }
    } macro;
    if (op.contains("params") && op.value("out").toBool()) {
        macro.undo = s.project.undoStack();
        macro.undo->beginMacro(T("Knoten ändern"));
    }
    bool any = false;
    if (op.contains("params")) {
        const QVariantMap values = nodeValues(s, kindOf(n->type, QString("node %1").arg(id)), op.value("params"),
                                              QString("node %1").arg(id), fxEffectOf(c, id));
        if (op.contains("at")) { // like the Inspector: an animated value gets a keyframe there, others their value
            const int at = timeOf(s, op, "at");
            if (at < c.start || at >= c.end())
                fail("BAD_ARGUMENT", QString("node %1: at must lie inside clip %2 (%3..%4)")
                                         .arg(id).arg(clip).arg(c.start).arg(c.end() - 1));
            if (!values.isEmpty()) done(s.editor.setCompNodeValuesAt(clip, id, values, at, {}, &err));
        } else {
            // The keyframes of an animated value win over its static value: setting that would do nothing
            for (auto it = values.begin(); it != values.end(); ++it)
                if (Keys::animated(c, Keys::effectParam(QString::fromLatin1(::Comp::kEffectId),
                                                        CompGraph::paramKey(id, it.key()))))
                    fail("BAD_ARGUMENT", QString("node %1: '%2' is animated (keyframes), a plain value would not show; "
                                                 "give at:<time> to set its keyframe there, or use {op:'keyframe', "
                                                 "param:'comp.n%1.%2', at, value}")
                                             .arg(id).arg(it.key()));
            if (!values.isEmpty()) done(s.editor.setCompNodeValues(clip, id, values, &err));
        }
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
    if (!s.editor.trackCompNode(clip.id, node, r.points, ::Comp::TrackLoss{r.lostFrames, r.gaveUp, to}))
        fail("FAILED", "could not store the tracking result");
    QJsonObject note{{"clip", clip.id}, {"node", node}, {"from", clip.start + from}, {"to", clip.start + to},
                     {"points", int(r.points.size())}, {"lost_frames", int(r.lostFrames.size())},
                     {"gave_up", r.gaveUp}, {"outliers", r.outliers}};
    // Where followers lose it, as stored (seen keys: lost frames at the end of a run that did not give up are none)
    QJsonArray lost;
    if (const Clip* now = TimelineOps::findClip(s.project.timeline(), clip.id))
        for (const ::Comp::TrackGap& gap : ::Comp::trackGaps(*now, node)) lost << QJsonArray{clip.start + gap.from, clip.start + gap.to};
    note["lost"] = lost;
    note["lost_at"] = r.lostAt >= 0 ? QJsonValue(clip.start + r.lostAt) : QJsonValue();
    note["found_at"] = r.foundAt >= 0 ? QJsonValue(clip.start + r.foundAt) : QJsonValue();
    if (r.gaveUp)
        note["hint"] = "lost for too long: the track stops there; start a new track_node from a frame where it is "
                       "visible again (from), or move the start points (ax, ay)";
    s.notes << QJsonObject{{"track_node", note}};
}

} // namespace

QJsonArray recipeSlotsJson(const CompRecipe& r)
{
    QJsonArray list;
    for (const CompRecipeSlot& slot : r.slotList) {
        QJsonObject o{{"slot", slot.id}, {"type", slot.type}};
        if (!slot.description.isEmpty()) o["description"] = slot.description;
        if (slot.required) o["required"] = true;
        // The value now: the recipe's, else the node kind's default (numbers back through mul/add)
        const CompRecipeTarget& t = slot.targets.first();
        QVariant v = r.values.value(t.node).value(t.key);
        const CompNode* n = r.graph.node(t.node);
        const PC::NodeType* k = n ? PC::find(n->type) : nullptr;
        const EffectParam* ep = k ? k->param(t.key) : nullptr;
        if (!v.isValid() && ep) v = ep->defaultValue;
        if (slot.type == QLatin1String("number") && v.isValid() && t.mul != 0) v = (v.toDouble() - t.add) / t.mul;
        if (slot.type == QLatin1String("choice") && ep) o["choices"] = QJsonArray::fromStringList(ep->choices);
        if (v.typeId() == QMetaType::QColor) o["default"] = v.value<QColor>().name(QColor::HexArgb);
        else if (v.isValid() && !(slot.type == QLatin1String("media") && v.toString().isEmpty()))
            o["default"] = QJsonValue::fromVariant(v);
        list << o;
    }
    return list;
}

QJsonArray recipesJson()
{
    QJsonArray list;
    for (const CompRecipe& r : CompRecipes::all()) {
        QJsonObject o{{"recipe", r.id}, {"name", r.name}, {"built_in", r.builtIn}, {"nodes", int(r.graph.nodes.size())},
                      {"slots", recipeSlotsJson(r)}};
        if (!r.description.isEmpty()) o["description"] = r.description;
        if (!r.note.isEmpty()) o["note"] = r.note;
        if (!r.dir.isEmpty()) o["folder"] = QDir::toNativeSeparators(r.dir);
        list << o;
    }
    return list;
}

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
            // A Path value that is no file (fileKeys: media path, LUT, texture) is text: the text node's text, font
            const bool text = p.type == EffectParam::Path && !::Comp::fileKeys(k.id).contains(p.key);
            QJsonObject po{{"key", p.key}, {"type", text ? "text" : typeName(p.type)}, {"label", p.label}};
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

QJsonObject compJson(const EffectInstance& e, const ProjectFormat& f, const Clip* clip)
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
        if (QJsonObject v = values.value(n.id); !v.isEmpty()) {
            // An effect node's values by their readable names (e_blur, not e_0)
            if (const QString fxId = v.value("effect").toString(); !fxId.isEmpty())
                for (const QString& k : v.keys())
                    if (k.startsWith(QLatin1String("e_")))
                        if (const QString shown = "e_" + EffectRegistry::shownKey(fxId, k.mid(2)); shown != k)
                            v[shown] = v.take(k);
            o["params"] = v;
        }
        if (::Comp::NodeRange r; clip && ::Comp::activeRange(e, n.id, &r, clip, 0)) { // where it works (timeline frames)
            int first = 0, end = 0;
            ::Comp::rangeFrames(*clip, r, f.rate.fps(), &first, &end);
            o["range"] = QJsonArray{first, end};
            if (r.keyed) o["range_keyframed"] = true; // from/until have keyframes: the range at the clip's start
        }
        if (!PC::find(n.type)) o["available"] = false; // a newer version's kind: passed through
        for (const char* key : {"e_chain", "e_code"}) // custom code not confirmed (core/CodeTrust.h): passed through
            if (!CodeTrust::isTrusted(values.value(n.id).value(QLatin1String(key)).toString())) o["untrusted"] = true;
        if (clip && n.type == QLatin1String(::Comp::kTrackerType)) // where it lost the point (keys n<id>.seen)
            if (const QVector<::Comp::TrackGap> gaps = ::Comp::trackGaps(*clip, n.id); !gaps.isEmpty()) {
                QJsonArray lost;
                for (const ::Comp::TrackGap& gap : gaps) lost << QJsonArray{clip->start + gap.from, clip->start + gap.to};
                o["lost"] = lost;
                if (gaps.last().openEnd || gaps.first().openStart) o["lost_ended"] = true;
            }
        if (n.type == QLatin1String(PC::kCutoutType)) // not analysed: shows nothing (EditOpsCutout.cpp)
            if (const QString hint = PC::cutoutHint(values.value(n.id).value(PC::kCutoutCacheKey).toString()); !hint.isEmpty())
                o["hint"] = hint;
        nodes << o;
    }
    for (const CompLink& l : g.links) links << QJsonArray{l.from, l.to, l.input};
    QJsonObject o{{"nodes", nodes}, {"links", links}, {"out", g.out}};
    QJsonArray unused;
    const QVector<int> order = g.evalOrder();
    QSet<int> followed; // trackers read through `follow` (a value, not a link) are used
    for (int id : order)
        if (const int f = ::Comp::followId(e.params.value(CompGraph::paramKey(id, "follow")).toDouble())) followed.insert(f);
    for (const CompNode& n : g.nodes)
        if (!order.contains(n.id) && !followed.contains(n.id)) unused << n.id;
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
                 "{op:'comp', clips, recipe:'greenscreen', with:{background:'beach.jpg'}} puts a ready one on; "
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
                 "Look at a node's picture with `frames` {clip, node, view:'matte'} (is the key clean?). "
                 "Recipes = ready graphs, `effects kind:'recipes'` lists them with their slots: greenscreen "
                 "(background, color, tolerance …), text_behind (background, text, color, size, x, y; the subject "
                 "from a key), blur_face (x, y, size, radius, track; then track_node on node 2), pip_frame (x, y, "
                 "size, frame_color, shadow), glitch_title (text, color, size, strength - mild), sticker_track "
                 "(sticker = a file, x, y, size; then track_node on node 2). with:{slot: value} fills them (files = "
                 "media pool items, imported), '<node id>.<key>' sets any node value; missing slots keep the "
                 "recipe's values; an unknown slot is an error. The answer's notes give the graph and the next step. "
                 R"(Example: {"op":"comp","clips":[4],"recipe":"greenscreen","with":{"background":"/videos/beach.jpg","tolerance":0.35}}. )"
                 "{op:'comp', clips:[id], save_recipe:'name', description?, replace?} saves the clip's graph as an own "
                 "recipe (its files copied along; media and text nodes become slots); `effects kind:'recipes'` with "
                 "export/to and import shares recipes as ZIP.",
                 opComp, QStringList{"clip", "linked", "remove", "recipe", "with", "save_recipe", "description", "replace"}}
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
                 "input; a cycle or an input the type does not have is an error. params of an animated value (keyframes) "
                 "need at:<time>: a keyframe there (like the Inspector; other values just change). A mask kind (mask, matte) behind a "
                 "picture node is only added (link it into a merge's mask input). unlink takes the link from `from` "
                 "(without input: the one it feeds). Time range: {op:'node', clip, id, range:[from, to]} (timeline times, to "
                 "exclusive; range:false or the whole clip = no range): the node works only there - outside, a node with a picture "
                 "input hands it on unchanged (a merge shows its bg), a generator (text, media, color) shows nothing; "
                 "stored as params from/until in seconds from the clip's start (params:{from:2, until:5.5} works too, "
                 "until 0 = to the end), they follow cuts and trims. Masks and trackers have none. `info` gives each "
                 "node's range in timeline frames. File values (media path, grade lut, text texture) must be "
                 "readable files; a lut also by name as in op 'look'. Example: blur only in a mask - "
                 R"({"op":"node","clip":4,"add":{"type":"blur","params":{"radius":12}}} then {"op":"node","clip":4,"add":{"type":"mask","params":{"shape":"ellipse"}},"input":false} and {"op":"node","clip":4,"link":[3,2,"mask"]}.)",
                 opNode, QStringList{"id", "params", "remove", "link", "unlink", "out", "at", "range"}}
        << OpDef{"track_node", "{op:'track_node', clip, node, from?, to?}",
                 "run a tracker node: follows its point(s) or a face over the clip; masks/transforms with follow:<id> "
                 "go along",
                 "node = id of a 'tracker' node (mode point|two|face, start points ax, ay, bx, by at frame `from`, as 0..1 "
                 "of the clip's own picture without letterbox bars). "
                 "from/to = timeline times inside the clip (default: the whole clip; to before from = backwards). "
                 "The result becomes keyframes of the node's x, y, scale, angle (replacing those in the range) and its "
                 "reference point rx, ry; followers move by (x - rx, y - ry). The "
                 "notes say points, lost_at/found_at (timeline frames where it lost/found the point again), gave_up "
                 "(lost too long: stopped after max_lost frames in a row, default 50), outliers (two points: frames "
                 "that used one) and lost: [[from, to], ...] (timeline frames, inclusive) where the point was not "
                 "seen (frames lost at the end without giving up are no gap: followers hold). Those frames become keyframes of the node's 'seen' (1 found, 0 lost). What followers do there "
                 "is the node's value lost (set it with {op:'node', clip, id, params:{lost:'hold'}}): 'auto' "
                 "(default: hold for a face, else fade), 'fade' (fade out over 0.2 s, in again when found), 'hold' (stay at the last place seen) or "
                 "'bridge' (move straight through the gap, like before 0.22); gaps up to 5 frames are bridged "
                 "always. `info` lists each tracker's gaps, `check` warns (tracker_lost, tracker_ended). Face mode "
                 "needs the 'faces' extension.",
                 opTrackNode, QStringList{"clips"}};
}

} // namespace Cli::detail
