// SPDX-License-Identifier: AGPL-3.0-only
// Editing the compositing graph of a clip (effect "comp", core/CompGraph.h): whole graph, nodes, links, output. Each
// op is one undo step; the node kinds come from the caller (core knows only their names).
#include "core/Editor.h"

#include "core/CompGraph.h"
#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"

#include <QSet>
#include <algorithm>

namespace {

bool failWith(QString* err, const QString& m)
{
    if (err) *err = m;
    return false;
}

// Keyframes of node values whose node is not in the graph any more
void dropOrphanKeys(Clip& c, const CompGraph& g)
{
    c.keys.removeIf([&](const auto& it) {
        QString effectId, key;
        int id = 0;
        return Keys::effectParamOf(it.key(), &effectId, &key) && effectId == QLatin1String(Comp::kEffectId)
               && CompGraph::splitParamKey(key, &id, nullptr) && !g.node(id);
    });
}

// The first connected input of a node, in the order a picture passes through (src, bg, fg, then any)
const CompLink* throughLink(const CompGraph& g, int id)
{
    for (const char* name : {"src", "bg", "fg"})
        if (const CompLink* l = g.linkInto(id, QString::fromLatin1(name))) return l;
    for (const CompLink& l : g.links)
        if (l.to == id) return &l;
    return nullptr;
}

} // namespace

bool Editor::editComp(int clipId, const QString& text,
                      const std::function<bool(CompGraph&, EffectInstance&, QString*)>& fn,
                      const CompGraph::KindInfo& kinds, QString* err)
{
    TrackRef ref;
    const Clip* found = TimelineOps::findClip(m_project->timeline(), clipId, &ref);
    if (!found) return failWith(err, QStringLiteral("no clip with id %1").arg(clipId));
    if (ref.kind != TrackKind::Video) return failWith(err, QStringLiteral("clip %1 has no picture (audio)").arg(clipId));
    if (editable({clipId}).isEmpty()) return failWith(err, QStringLiteral("clip %1 is on a locked track").arg(clipId));
    Clip next = *found;
    if (!EffectRegistry::has(next, Comp::kEffectId)) EffectRegistry::add(next, Comp::kEffectId);
    EffectInstance* e = EffectRegistry::instance(next, Comp::kEffectId);
    if (!e) return failWith(err, QStringLiteral("cannot add the effect comp"));
    CompGraph g;
    if (e->params.contains(Comp::kGraphKey) && !Comp::graphOf(*e, &g))
        return failWith(err, QStringLiteral("the clip's graph is broken; set a new one with {op:'comp', graph}"));
    if (!fn(g, *e, err)) return false;
    QString why;
    if (kinds && !g.validate(kinds, &why)) return failWith(err, why); // no kinds: the graph's form did not change
    Comp::setGraph(*e, g);
    Comp::dropOrphanParams(*e);
    dropOrphanKeys(next, g);
    if (next == *found) return true; // nothing to change: no empty undo step
    modifyClips({clipId}, text, [&](Clip& c) {
        c.effects = next.effects;
        c.keys = next.keys;
    });
    return true;
}

bool Editor::setCompGraph(const QVector<int>& ids, const CompGraph& graph, const QHash<int, QVariantMap>& values,
                          const CompGraph::KindInfo& kinds, QString* err)
{
    QString why;
    if (!graph.validate(kinds, &why)) return failWith(err, why);
    QVector<int> targets;
    for (int id : editable(ids)) {
        TrackRef ref;
        if (TimelineOps::findClip(m_project->timeline(), id, &ref) && ref.kind == TrackKind::Video) targets << id;
    }
    if (targets.isEmpty()) return failWith(err, QStringLiteral("no video clip on an unlocked track among the clips"));
    modifyClips(targets, T("Comp-Graph setzen"), [&](Clip& c) {
        EffectRegistry::remove(c, Comp::kEffectId); // old values and keyframes go
        EffectRegistry::add(c, Comp::kEffectId);
        EffectInstance* e = EffectRegistry::instance(c, Comp::kEffectId);
        if (!e) return;
        Comp::setGraph(*e, graph);
        for (auto it = values.begin(); it != values.end(); ++it)
            if (graph.node(it.key()))
                for (auto v = it->begin(); v != it->end(); ++v) e->params[CompGraph::paramKey(it.key(), v.key())] = v.value();
    });
    return true;
}

QVector<int> Editor::removeComp(const QVector<int>& ids)
{
    QVector<int> targets;
    for (int id : editable(ids))
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id); c && EffectRegistry::has(*c, Comp::kEffectId))
            targets << id;
    modifyClips(targets, T("Comp-Graph entfernen"), [](Clip& c) { EffectRegistry::remove(c, Comp::kEffectId); });
    return targets;
}

int Editor::addCompNode(int clipId, const QString& type, const QVariantMap& values, int after, const QString& input,
                        const CompGraph::KindInfo& kinds, QString* err)
{
    QStringList inputs;
    if (!kinds(type, &inputs)) {
        failWith(err, QStringLiteral("unknown node type '%1'").arg(type));
        return 0;
    }
    if (!input.isEmpty() && !inputs.contains(input)) {
        failWith(err, QStringLiteral("node type %1 has no input '%2' (inputs: %3)")
                          .arg(type, input, inputs.isEmpty() ? QStringLiteral("none") : inputs.join(", ")));
        return 0;
    }
    int added = 0;
    const bool ok = editComp(clipId, T("Knoten hinzufügen"), [&](CompGraph& g, EffectInstance& e, QString* why) {
        if (g.isEmpty()) { // a fresh graph starts with the clip's own picture as its output
            g.nodes << CompNode{1, QStringLiteral("clip"), {}};
            g.out = 1;
        }
        int from = after;
        if (from == 0) from = g.out;
        if (after > 0 && !g.node(after)) return failWith(why, QStringLiteral("no node %1 in the graph").arg(after));
        CompNode n;
        n.id = g.nextId();
        n.type = type;
        if (const CompNode* a = g.node(from)) n.pos = a->pos + QPointF(160, 0);
        g.nodes << n;
        if (!input.isEmpty() && from > 0) {
            for (CompLink& l : g.links)
                if (l.from == from) l.from = n.id; // the readers of `from` read the new node
            if (g.out == from) g.out = n.id;
            g.links << CompLink{from, n.id, input};
        }
        for (auto it = values.begin(); it != values.end(); ++it) e.params[CompGraph::paramKey(n.id, it.key())] = it.value();
        added = n.id;
        return true;
    }, kinds, err);
    return ok ? added : 0;
}

bool Editor::setCompNodeValues(int clipId, int node, const QVariantMap& values, QString* err)
{
    return editComp(clipId, T("Knoten ändern"), [&](CompGraph& g, EffectInstance& e, QString* why) {
        if (!g.node(node)) return failWith(why, QStringLiteral("no node %1 in the graph").arg(node));
        for (auto it = values.begin(); it != values.end(); ++it) e.params[CompGraph::paramKey(node, it.key())] = it.value();
        return true;
    }, {}, err);
}

bool Editor::removeCompNode(int clipId, int node, const CompGraph::KindInfo& kinds, QString* err)
{
    return editComp(clipId, T("Knoten entfernen"), [&](CompGraph& g, EffectInstance&, QString* why) {
        if (!g.node(node)) return failWith(why, QStringLiteral("no node %1 in the graph").arg(node));
        const CompLink* through = throughLink(g, node);
        const int source = through ? through->from : 0;
        QVector<CompLink> links;
        for (const CompLink& l : g.links) {
            if (l.to == node) continue;
            if (l.from == node) {
                if (source) links << CompLink{source, l.to, l.input};
                continue;
            }
            links << l;
        }
        g.links = links;
        g.nodes.removeIf([&](const CompNode& n) { return n.id == node; });
        if (g.out == node) g.out = source;
        if (g.out == 0 && !g.nodes.isEmpty()) g.out = g.nodes.last().id;
        return true;
    }, kinds, err);
}

bool Editor::linkCompNodes(int clipId, const CompLink& l, bool on, const CompGraph::KindInfo& kinds, QString* err)
{
    return editComp(clipId, on ? T("Knoten verbinden") : T("Verbindung lösen"),
                    [&](CompGraph& g, EffectInstance&, QString* why) {
                        if (!g.node(l.to)) return failWith(why, QStringLiteral("no node %1 in the graph").arg(l.to));
                        if (on && !g.node(l.from)) return failWith(why, QStringLiteral("no node %1 in the graph").arg(l.from));
                        const qsizetype before = g.links.size();
                        g.links.removeIf([&](const CompLink& x) { return x.to == l.to && x.input == l.input; });
                        if (on) g.links << l;
                        else if (g.links.size() == before)
                            return failWith(why, QStringLiteral("nothing is linked into input '%1' of node %2").arg(l.input).arg(l.to));
                        return true;
                    }, kinds, err);
}

bool Editor::setCompOut(int clipId, int node, const CompGraph::KindInfo& kinds, QString* err)
{
    return editComp(clipId, T("Ausgabe-Knoten setzen"), [&](CompGraph& g, EffectInstance&, QString* why) {
        if (!g.node(node)) return failWith(why, QStringLiteral("no node %1 in the graph").arg(node));
        g.out = node;
        return true;
    }, kinds, err);
}
