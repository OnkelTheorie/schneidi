// SPDX-License-Identifier: AGPL-3.0-only
#include "core/CompGraph.h"

#include "core/Types.h"

#include <QHash>
#include <QSet>
#include <QVariantList>

#include <algorithm>
#include <iterator>

const CompNode* CompGraph::node(int id) const
{
    for (const CompNode& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

int CompGraph::nextId() const
{
    int m = 0;
    for (const CompNode& n : nodes) m = std::max(m, n.id);
    return m + 1;
}

const CompLink* CompGraph::linkInto(int to, const QString& input) const
{
    for (const CompLink& l : links)
        if (l.to == to && l.input == input) return &l;
    return nullptr;
}

QVariantMap CompGraph::toVariant() const
{
    QVariantList ns, ls;
    for (const CompNode& n : nodes)
        ns << QVariantMap{{"id", n.id}, {"type", n.type}, {"x", n.pos.x()}, {"y", n.pos.y()}};
    for (const CompLink& l : links) ls << QVariant(QVariantList{l.from, l.to, l.input});
    return {{"nodes", ns}, {"links", ls}, {"out", out}, {"v", version}};
}

bool CompGraph::fromVariant(const QVariant& v, CompGraph* out, QString* err)
{
    auto fail = [err](const QString& m) {
        if (err) *err = m;
        return false;
    };
    const QVariantMap m = v.toMap();
    if (m.isEmpty()) return fail(QStringLiteral("graph is not an object"));
    CompGraph g;
    g.version = m.value("v", kVersion).toInt();
    g.out = m.value("out").toInt();
    const QVariantList ns = m.value("nodes").toList();
    if (ns.size() > 4096) return fail(QStringLiteral("too many nodes (at most 4096)"));
    QSet<int> ids;
    for (const QVariant& nv : ns) {
        const QVariantMap nm = nv.toMap();
        CompNode n;
        n.id = nm.value("id").toInt();
        n.type = nm.value("type").toString();
        n.pos = QPointF(nm.value("x").toDouble(), nm.value("y").toDouble());
        if (n.id < 1) return fail(QStringLiteral("node id must be a number >= 1"));
        if (n.type.isEmpty()) return fail(QStringLiteral("node %1 has no type").arg(n.id));
        if (ids.contains(n.id)) return fail(QStringLiteral("node id %1 used twice").arg(n.id));
        ids.insert(n.id);
        g.nodes << n;
    }
    const QVariantList ls = m.value("links").toList();
    if (ls.size() > 16384) return fail(QStringLiteral("too many links"));
    for (const QVariant& lv : ls) {
        CompLink l;
        if (lv.typeId() == QMetaType::QVariantMap) { // also accepted: {from, to, input}
            const QVariantMap lm = lv.toMap();
            l = {lm.value("from").toInt(), lm.value("to").toInt(), lm.value("input").toString()};
        } else {
            const QVariantList a = lv.toList();
            if (a.size() < 2) return fail(QStringLiteral("a link is [from, to, input]"));
            l = {a[0].toInt(), a[1].toInt(), a.value(2).toString()};
        }
        if (!ids.contains(l.from) || !ids.contains(l.to))
            return fail(QStringLiteral("link %1 -> %2 names a missing node").arg(l.from).arg(l.to));
        g.links << l;
    }
    if (g.out != 0 && !ids.contains(g.out)) return fail(QStringLiteral("out names missing node %1").arg(g.out));
    *out = std::move(g);
    return true;
}

bool CompGraph::validate(const KindInfo& kinds, QString* err, bool allowUnknown) const
{
    auto fail = [err](const QString& m) {
        if (err) *err = m;
        return false;
    };
    QHash<int, QStringList> inputs;
    for (const CompNode& n : nodes) {
        QStringList in;
        if (!kinds(n.type, &in)) {
            if (!allowUnknown) return fail(QStringLiteral("node %1: unknown type '%2'").arg(n.id).arg(n.type));
            in.clear();
            inputs.insert(n.id, in);
            continue;
        }
        inputs.insert(n.id, in);
    }
    QSet<QPair<int, QString>> taken;
    for (const CompLink& l : links) {
        const CompNode* to = node(l.to);
        const CompNode* from = node(l.from);
        if (!to || !from) return fail(QStringLiteral("link %1 -> %2 names a missing node").arg(l.from).arg(l.to));
        if (l.from == l.to) return fail(QStringLiteral("node %1 is linked to itself").arg(l.to));
        QStringList dummy;
        const bool known = kinds(to->type, &dummy);
        if (known && !inputs.value(l.to).contains(l.input))
            return fail(QStringLiteral("node %1 (%2) has no input '%3' (inputs: %4)")
                            .arg(l.to)
                            .arg(to->type, l.input, inputs.value(l.to).join(", ")));
        if (taken.contains({l.to, l.input}))
            return fail(QStringLiteral("input '%1' of node %2 is linked twice").arg(l.input).arg(l.to));
        taken.insert({l.to, l.input});
    }
    if (out == 0 && !nodes.isEmpty()) return fail(QStringLiteral("graph has no output node (out)"));
    if (out != 0 && !node(out)) return fail(QStringLiteral("out names missing node %1").arg(out));
    // Cycle: depth-first search over all nodes (not only those out reaches: a cycle anywhere is an error)
    QHash<int, int> state; // 0 new, 1 on the stack, 2 done
    std::function<bool(int)> visit = [&](int id) {
        state[id] = 1;
        for (const CompLink& l : links)
            if (l.to == id) {
                const int s = state.value(l.from);
                if (s == 1) return false;
                if (s == 0 && !visit(l.from)) return false;
            }
        state[id] = 2;
        return true;
    };
    for (const CompNode& n : nodes)
        if (state.value(n.id) == 0 && !visit(n.id)) return fail(QStringLiteral("graph has a cycle"));
    return true;
}

QVector<int> CompGraph::evalOrder() const
{
    QVector<int> order;
    if (!node(out)) return order;
    QHash<int, int> state;
    bool cycle = false;
    std::function<void(int)> visit = [&](int id) {
        state[id] = 1;
        for (const CompLink& l : links)
            if (l.to == id) {
                const int s = state.value(l.from);
                if (s == 1) cycle = true;
                else if (s == 0) visit(l.from);
                if (cycle) return;
            }
        state[id] = 2;
        order << id;
    };
    visit(out);
    if (cycle) order.clear();
    return order;
}

QString CompGraph::paramKey(int id, const QString& key) { return QStringLiteral("n%1.%2").arg(id).arg(key); }

bool CompGraph::splitParamKey(const QString& full, int* id, QString* key)
{
    if (!full.startsWith('n')) return false;
    const qsizetype dot = full.indexOf('.');
    if (dot < 2 || dot == full.size() - 1) return false;
    bool ok = false;
    const int n = full.mid(1, dot - 1).toInt(&ok);
    if (!ok || n < 1) return false;
    if (id) *id = n;
    if (key) *key = full.mid(dot + 1);
    return true;
}

namespace Comp {

bool graphOf(const EffectInstance& e, CompGraph* g)
{
    if (e.effectId != QLatin1String(kEffectId)) return false;
    return CompGraph::fromVariant(e.params.value(kGraphKey), g);
}

void setGraph(EffectInstance& e, const CompGraph& g) { e.params[kGraphKey] = g.toVariant(); }

void dropOrphanParams(EffectInstance& e)
{
    CompGraph g;
    if (!graphOf(e, &g)) return;
    for (auto it = e.params.begin(); it != e.params.end();) {
        int id = 0;
        if (CompGraph::splitParamKey(it.key(), &id, nullptr) && !g.node(id)) it = e.params.erase(it);
        else ++it;
    }
}

namespace {

struct KindKeys {
    const char* type;
    const char* keys[12]; // up to the first nullptr
};

QStringList keysOf(const KindKeys* table, size_t n, const QString& type)
{
    for (size_t i = 0; i < n; ++i)
        if (type == QLatin1String(table[i].type)) {
            QStringList out;
            for (const char* k : table[i].keys) {
                if (!k) break;
                out << QString::fromLatin1(k);
            }
            return out;
        }
    return {};
}

// File parameters per node kind. A NEW node kind or parameter holding a file goes here (see fileKeys in the header)
const KindKeys kFileKeys[] = {
    {kMediaType, {kPathKey}},
    {kGradeType, {kLutKey}},
    {kTextType, {kTextureKey}},
};

// Values in project pixels per node kind (× ctx.scale in the engine). A NEW pixel parameter goes here (see pixelKeys
// in the header); positions and sizes as 0..1 of the picture do not
const KindKeys kPixelKeys[] = {
    {kTextType,
     {"size", "outline_width", "box_padding", "edge_depth", "shadow_x", "shadow_y", "shadow_soft", "glow_radius",
      "texture_x", "texture_y"}},
    {"blur", {"radius"}},
    {"crop", {"soft"}},
    {"mask", {"soft"}},
    {"shadow", {"x", "y", "soft"}},
    {"glitch", {"rgb"}},
    {"cutout", {"soft", "grow"}},
    {"matte_tune", {"grow", "soft"}},
};

} // namespace

QStringList fileKeys(const QString& type) { return keysOf(kFileKeys, std::size(kFileKeys), type); }

QStringList pixelKeys(const QString& type) { return keysOf(kPixelKeys, std::size(kPixelKeys), type); }

void forEachFile(EffectInstance& e, const std::function<void(const QString& key, QString& path)>& fn)
{
    CompGraph g;
    if (!graphOf(e, &g)) return;
    for (const CompNode& n : g.nodes)
        for (const QString& k : fileKeys(n.type)) {
            const QString key = CompGraph::paramKey(n.id, k);
            QString path = e.params.value(key).toString();
            if (path.isEmpty()) continue;
            const QString before = path;
            fn(key, path);
            if (path != before) e.params[key] = path;
        }
}

QStringList mediaPaths(const EffectInstance& e)
{
    QStringList out;
    EffectInstance copy = e; // forEachFile may change values; this only reads them
    forEachFile(copy, [&](const QString&, QString& path) {
        if (!out.contains(path)) out << path;
    });
    return out;
}

int redirectMedia(EffectInstance& e, const std::function<QString(const QString&)>& moved)
{
    int n = 0;
    forEachFile(e, [&](const QString&, QString& path) {
        const QString now = moved(path);
        if (!now.isEmpty() && now != path) {
            path = now;
            ++n;
        }
    });
    return n;
}

} // namespace Comp
