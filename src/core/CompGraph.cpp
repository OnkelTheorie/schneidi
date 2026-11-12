// SPDX-License-Identifier: AGPL-3.0-only
#include "core/CompGraph.h"

#include "core/Keyframes.h"
#include "core/Types.h"

#include <QDebug>
#include <QHash>
#include <QSet>
#include <QVariantList>

#include <algorithm>
#include <cmath>
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
    {"text3d", {"size", "depth", "bevel"}},
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

bool outputsMask(const QString& type)
{
    return type == QLatin1String("mask") || type == QLatin1String("matte");
}

bool maskBehindPicture(const QString& type, const QString& fromType)
{
    return outputsMask(type) && (fromType.isEmpty() || !outputsMask(fromType));
}

void followHead(Clip& c, int frames, double fps)
{
    if (frames == 0) return;
    if (fps <= 0) { // never half a shift (offset moved, titles not): every edit path hands the rate on
        qDebug() << "Comp::followHead without a frame rate: clip" << c.id << "keeps its graph values";
        return;
    }
    EffectInstance* e = nullptr;
    for (EffectInstance& x : c.effects)
        if (x.effectId == QLatin1String(kEffectId)) e = &x;
    CompGraph g;
    if (!e || !graphOf(*e, &g)) return;
    for (const CompNode& n : g.nodes) {
        if (n.type == QLatin1String(kMediaType)) {
            // It plays frame offset + t * speed at clip frame t (engine defaults: offset 0, speed 1)
            const QString key = CompGraph::paramKey(n.id, QStringLiteral("offset"));
            const QString speedKey = CompGraph::paramKey(n.id, QStringLiteral("speed"));
            // An animated speed: the engine plays offset + t * speed(t), which no single offset keeps for every frame;
            // the speed at the new first frame (keys are source frames: `c` has its new in already) keeps that one
            const AnimParam sp = Keys::effectParam(QString::fromLatin1(kEffectId), speedKey);
            const double speed = Keys::animated(c, sp) ? Keys::valueAt(c, sp, 0) : e->params.value(speedKey, 1.0).toDouble();
            const double shift = frames * speed;
            e->params[key] = e->params.value(key, 0.0).toDouble() + shift;
            const auto k = c.keys.find(Keys::effectParam(QString::fromLatin1(kEffectId), key));
            if (k != c.keys.end())
                for (Keyframe& f : *k) f.value += shift;
        }
        // The time range (every kind that has one; also a newer version's kind: from/until mean the same for all).
        // 0 means "from the start" / "to the end" and stays; any other value moves, also below 0 (from < 0 = from
        // the start, until < 0 = over before the start: never; inRange), so trimming back out gives the exact values
        // again. A value landing on 0 stays just below it (not "the end")
        const double s = frames / fps;
        const auto moved = [s](double v) { return std::abs(v - s) < 1e-9 ? -1e-9 : v - s; };
        for (const char* name : {kFromKey, kUntilKey}) {
            const QString k = CompGraph::paramKey(n.id, QString::fromLatin1(name));
            if (const double v = e->params.value(k, 0.0).toDouble(); v != 0) e->params[k] = moved(v);
        }
    }
}

bool hasRange(const QString& type)
{
    return !type.isEmpty() && !outputsMask(type) && type != QLatin1String("tracker");
}

bool rangeSet(double from, double until) { return from > 0 || until != 0; }

bool inRange(double from, double until, double s)
{
    if (until < 0) return false; // over before the clip's start (its head was trimmed or cut off)
    if (from <= 0 && until <= 0) return true;
    const double end = until > 0 ? until : 1e12;
    return s >= std::max(0.0, from) && s < end;
}

namespace {
bool rangeIn(const EffectInstance& e, const CompGraph& g, int node, NodeRange* r)
{
    const CompNode* n = g.node(node);
    if (!n || !hasRange(n->type)) return false;
    const double from = e.params.value(CompGraph::paramKey(node, QString::fromLatin1(kFromKey)), 0.0).toDouble();
    const double until = e.params.value(CompGraph::paramKey(node, QString::fromLatin1(kUntilKey)), 0.0).toDouble();
    if (!rangeSet(from, until)) return false;
    if (r) *r = {node, n->type, from, until};
    return true;
}
} // namespace

bool activeRange(const EffectInstance& e, int node, NodeRange* r)
{
    CompGraph g;
    return graphOf(e, &g) && rangeIn(e, g, node, r);
}

QVector<NodeRange> nodeRanges(const EffectInstance& e)
{
    QVector<NodeRange> out;
    CompGraph g;
    if (!graphOf(e, &g)) return out;
    for (const CompNode& n : g.nodes)
        if (NodeRange r; rangeIn(e, g, n.id, &r)) out << r;
    return out;
}

void rangeFrames(const Clip& c, const NodeRange& r, double fps, int* first, int* end)
{
    fps = std::max(1e-6, fps);
    // The first clip frame t with t / fps >= from, the first one with t / fps >= until (inRange, frame by frame)
    const int len = c.length();
    int a = r.from > 0 ? int(std::ceil(r.from * fps - 1e-6)) : 0;
    int b = r.until > 0 ? int(std::ceil(r.until * fps - 1e-6)) : r.until < 0 ? 0 : len;
    a = std::clamp(a, 0, len);
    b = std::clamp(b, a, len);
    *first = c.start + a;
    *end = c.start + b;
}

void rangeValues(const Clip& c, int first, int end, double fps, double* from, double* until)
{
    fps = std::max(1e-6, fps);
    const int a = std::clamp(first - c.start, 0, c.length()), b = std::clamp(end - c.start, a, c.length());
    *from = a / fps;
    // A range that ends where it starts would read as "to the end" (until 0): one frame at least
    *until = std::max(b, a + 1) / fps;
}

} // namespace Comp
