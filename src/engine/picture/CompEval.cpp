// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/picture/CompEval.h"
#include "engine/picture/CompTrack.h"

#include "core/CompGraph.h"
#include "core/CompTrack.h"
#include "core/Keyframes.h"
#include "core/Types.h"

#include <QDataStream>
#include <QHash>
#include <QIODevice>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Picture::Comp {

// ---- Cache ----

std::shared_ptr<const Buf> Cache::find(uint64_t key)
{
    std::lock_guard lock(m_mutex);
    const auto it = m_items.find(key);
    return it == m_items.end() ? nullptr : it->second;
}

void Cache::put(uint64_t key, std::shared_ptr<const Buf> b)
{
    if (!b) return;
    const size_t n = b->px.size();
    if (n > kMaxBytes / 2) return;
    std::lock_guard lock(m_mutex);
    if (m_items.count(key)) return;
    while (m_bytes + n > kMaxBytes && !m_order.empty()) {
        const auto it = m_items.find(m_order.front());
        if (it != m_items.end()) {
            m_bytes -= it->second->px.size();
            m_items.erase(it);
        }
        m_order.erase(m_order.begin());
    }
    m_items.emplace(key, std::move(b));
    m_order.push_back(key);
    m_bytes += n;
}

void Cache::clear()
{
    std::lock_guard lock(m_mutex);
    m_items.clear();
    m_order.clear();
    m_bytes = 0;
}

size_t Cache::bytes() const
{
    std::lock_guard lock(m_mutex);
    return m_bytes;
}

namespace {

uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

// Hash of a node's own values (keys sorted: QVariantMap), its kind and the output it is made for
uint64_t ownHash(const QString& type, const QVariantMap& values, const Env& env)
{
    QByteArray bytes;
    {
        QDataStream s(&bytes, QIODevice::WriteOnly);
        s << type << values << env.size << env.scale;
    }
    return mix(qHashBits(bytes.constData(), size_t(bytes.size()), 0x51ab),
               qHashBits(bytes.constData(), size_t(bytes.size()), 0x7f31));
}

struct NodeState {
    const CompNode* node = nullptr;
    const NodeType* kind = nullptr;
    Buf own;                          // made here (pooled)
    std::shared_ptr<const Buf> kept;  // from / into the cache
    const Buf* pic = nullptr;         // the picture (own, kept or borrowed), nullptr = nothing
    int readers = 0;                  // nodes still to read it
    bool isStatic = false;
    uint64_t key = 0;
    bool needed = false;              // the output depends on it (and no cached node in between)
    bool hit = false;                 // static and found in the cache: its inputs are not evaluated
    bool fusedInto = false;           // a compound track painted straight onto the merge that reads it
    int through = -1;                 // passes this node's picture on (unknown kind, tracker): every read is its read
    int track = -1;
};

} // namespace

QStringList unknownKinds(const EffectInstance& comp)
{
    QStringList out;
    CompGraph g;
    if (!::Comp::graphOf(comp, &g)) return out;
    for (int id : g.evalOrder())
        if (const CompNode* n = g.node(id); n && !find(n->type) && !out.contains(n->type)) out << n->type;
    return out;
}

bool evaluate(const EffectInstance& comp, const Env& env, Cache* cache, Buf* out, Stats* stats)
{
    if (env.size.isEmpty()) return false;
    CompGraph g;
    if (!::Comp::graphOf(comp, &g)) return false;
    const QVector<int> order = g.evalOrder();
    if (order.isEmpty()) return false;

    // Values per node and which of them have keyframes
    QHash<int, QVariantMap> values;
    for (auto it = comp.params.cbegin(); it != comp.params.cend(); ++it) {
        int id = 0;
        QString key;
        if (CompGraph::splitParamKey(it.key(), &id, &key)) values[id].insert(key, it.value());
    }
    QHash<int, QSet<QString>> animated;
    if (env.clip)
        for (auto it = env.clip->keys.cbegin(); it != env.clip->keys.cend(); ++it) {
            QString effect, full, key;
            int id = 0;
            if (!it.value().isEmpty() && Keys::effectParamOf(it.key(), &effect, &full)
                && effect == QLatin1String(::Comp::kEffectId) && CompGraph::splitParamKey(full, &id, &key))
                animated[id].insert(key);
        }

    QHash<int, NodeState> state;
    state.reserve(order.size());
    for (int id : order) {
        NodeState& s = state[id];
        s.node = g.node(id);
        s.kind = s.node ? find(s.node->type) : nullptr;
    }
    // Lookups without copying a node's values (read per parameter per frame)
    static const QVariantMap kNoValues;
    auto valuesOf = [&](int id) -> const QVariantMap& {
        const auto it = values.constFind(id);
        return it == values.cend() ? kNoValues : *it;
    };
    auto isAnimated = [&](int id, const QString& key) {
        const auto it = animated.constFind(id);
        return it != animated.cend() && it->contains(key);
    };
    auto valueOf = [&](int id, const NodeType* k, const QString& key) -> double {
        if (env.clip && isAnimated(id, key))
            return Keys::valueAt(*env.clip, Keys::effectParam(::Comp::kEffectId, CompGraph::paramKey(id, key)), env.t);
        if (!k) return valuesOf(id).value(key).toDouble();
        return numValue(*k, valuesOf(id), key);
    };

    QHash<int, ::Comp::FollowTime> followTimes; // per tracker followed (gaps, lost mode) at env.t

    // Static: kind not varying, no keyframes, every connected input static; keyed by its values and the inputs' keys
    for (int id : order) {
        NodeState& s = state[id];
        if (!s.node) continue;
        bool isStatic = s.kind && !varies(*s.kind, valuesOf(id)) && !animated.contains(id);
        uint64_t key = isStatic ? ownHash(s.node->type, valuesOf(id), env) : 0;
        // A follower reads the followed node's values (numOf): static only while they are fixed, keyed by them (a new
        // analysis or another value is a new key)
        if (isStatic && s.kind->param(QStringLiteral("follow"))) {
            if (const int f = ::Comp::followId(valueOf(id, s.kind, QStringLiteral("follow"))); f > 0) {
                const CompNode* fn = g.node(f);
                const NodeType* fk = fn ? find(fn->type) : nullptr;
                if (animated.contains(f) || (fk && varies(*fk, valuesOf(f)))) isStatic = false;
                else key = mix(key, ownHash(fn ? fn->type : QString(), valuesOf(f), env));
            }
        }
        for (const CompLink& l : g.links)
            if (l.to == id) {
                const auto it = state.constFind(l.from);
                if (it == state.cend() || !it->isStatic) {
                    isStatic = false;
                } else {
                    key = mix(key, it->key);
                    key = mix(key, qHash(l.input));
                }
            }
        s.isStatic = isStatic;
        s.key = key;
    }
    // From the output back: a static node found in the cache needs nothing below it
    state[g.out].needed = true;
    for (auto it = order.crbegin(); it != order.crend(); ++it) {
        NodeState& s = state[*it];
        if (!s.needed) continue;
        if (s.isStatic && cache && (s.kept = cache->find(s.key))) {
            s.hit = true;
            continue;
        }
        for (const CompLink& l : g.links)
            if (l.to == *it && state.contains(l.from)) state[l.from].needed = true;
    }
    for (const CompLink& l : g.links)
        if (state.contains(l.from) && state.contains(l.to) && state[l.to].needed && !state[l.to].hit)
            ++state[l.from].readers;
    ++state[g.out].readers; // the result is read by the caller

    // Compound tracks painted straight onto the merge reading them (normal mode, full opacity, no mask): exactly the
    // timeline's way of laying a track on the ones below, no picture of the track alone
    if (env.paintTrack)
        for (int id : order) {
            NodeState& s = state[id];
            // Only a merge that is evaluated: its read is then the track's only one (readers counts no other)
            if (!s.needed || s.hit || !s.kind || s.kind->id != QLatin1String("merge") || g.linkInto(id, "mask"))
                continue;
            const CompLink* fg = g.linkInto(id, "fg");
            if (!fg || !state.contains(fg->from)) continue;
            NodeState& f = state[fg->from];
            if (!f.kind || f.kind->id != QLatin1String("clip") || f.readers != 1) continue;
            const int track = int(std::lround(valueOf(fg->from, f.kind, "track")));
            if (track < 0 || isAnimated(fg->from, QStringLiteral("track"))) continue;
            if (numValue(*s.kind, valuesOf(id), "mode") != 0 || isAnimated(id, QStringLiteral("opacity"))
                || valueOf(id, s.kind, "opacity") < 1.0)
                continue;
            f.fusedInto = true;
            f.track = track;
        }

    int alive = 0, peak = 0;
    auto take = [&](Buf::Kind kind) {
        Buf b;
        b.kind = kind;
        b.w = env.size.width();
        b.h = env.size.height();
        const size_t n = size_t(b.w) * b.h * Buf::channels(kind);
        b.px = env.takeBytes ? env.takeBytes(n) : std::vector<uint8_t>(n, 0);
        if (b.px.size() != n) b.px.assign(n, 0);
        peak = std::max(peak, ++alive);
        return b;
    };
    auto release = [&](NodeState& s) {
        if (!s.own.px.empty()) {
            if (env.giveBytes) env.giveBytes(std::move(s.own.px));
            s.own.px = {};
            --alive;
        }
        s.kept.reset();
        s.pic = nullptr;
    };
    auto inputOf = [&](int id, const QString& name) -> const Buf* {
        const CompLink* l = g.linkInto(id, name);
        if (!l) return nullptr;
        const auto it = state.constFind(l->from);
        return it == state.cend() ? nullptr : it->pic;
    };
    // The compound track painted straight onto merge id (no copy of a NodeState)
    auto fusedFg = [&](int id) -> const NodeState* {
        const CompLink* l = g.linkInto(id, "fg");
        if (!l) return nullptr;
        const auto it = state.constFind(l->from);
        return it != state.cend() && it->fusedInto ? &*it : nullptr;
    };
    // A node whose picture is its input's: no copy; the node owning the picture stays alive for this one's readers
    auto passOn = [&](NodeState& s, const CompLink* l) {
        if (!l) return;
        const auto it = state.find(l->from);
        if (it == state.end() || !it->pic) return;
        s.pic = it->pic;
        s.through = it->through >= 0 ? it->through : l->from;
        state[s.through].readers += s.readers;
    };
    std::function<void(int)> drop = [&](int id) { // one read of node id done
        NodeState& from = state[id];
        if (--from.readers == 0) release(from);
        if (from.through >= 0) drop(from.through);
    };

    for (int id : order) {
        if (env.cancel && env.cancel->load(std::memory_order_relaxed)) {
            for (NodeState& s : state) release(s);
            return false;
        }
        NodeState& s = state[id];
        const CompNode* n = s.node;
        if (!n || !s.needed) continue;
        const bool isStatic = s.isStatic;
        const uint64_t key = s.key;

        if (s.hit) {
            s.pic = s.kept.get();
            if (stats) ++stats->cached;
            continue; // read nothing
        } else if (s.fusedInto) {
            // made by the merge reading it
        } else if (s.kind && !s.kind->passThrough.isEmpty()) {
            passOn(s, g.linkInto(id, s.kind->passThrough));
        } else if (!s.kind) {
            // Unknown kind (newer version): its first connected input passed through
            if (stats) {
                ++stats->unknown;
                stats->unknownKinds.insert(n->type);
            }
            const CompLink* through = nullptr;
            for (const char* name : {"src", "bg", "fg"})
                if ((through = g.linkInto(id, name))) break;
            if (!through)
                for (const CompLink& l : g.links)
                    if (l.to == id) {
                        through = &l;
                        break;
                    }
            passOn(s, through);
        } else if (s.kind->id == QLatin1String("clip") && env.clipPicture) {
            const int track = int(std::lround(valueOf(id, s.kind, "track")));
            if (track < 0 || !env.paintTrack) {
                s.pic = env.clipPicture(); // borrowed, no copy
            } else {
                s.own = take(Buf::Rgba);
                if (env.paintTrack(track, s.own)) s.pic = &s.own;
                if (stats) ++stats->evaluated;
            }
        } else if (const NodeState* fused = s.kind->id == QLatin1String("merge") ? fusedFg(id) : nullptr) {
            const NodeState& f = *fused;
            const Buf* bg = inputOf(id, "bg");
            s.own = take(Buf::Rgba);
            if (bg && bg->kind == Buf::Rgba && bg->px.size() == s.own.px.size())
                std::memcpy(s.own.data(), bg->data(), bg->px.size());
            env.paintTrack(f.track, s.own);
            s.pic = &s.own;
            if (stats) ++stats->evaluated;
        } else if (s.kind->eval) {
            NodeCtx ctx;
            ctx.node = id;
            ctx.t = env.t;
            ctx.frameNo = env.frameNo;
            ctx.size = env.size;
            ctx.scale = env.scale;
            ctx.fps = env.fps;
            ctx.cancel = env.cancel;
            const NodeType* k = s.kind;
            const QVariantMap& own = valuesOf(id);
            ctx.num = [&, id, k](const QString& key) { return valueOf(id, k, key); };
            ctx.color = [&, id, k](const QString& key) {
                if (env.clip && isAnimated(id, key)) return Keys::toColor(valueOf(id, k, key));
                return colorValue(*k, own, key);
            };
            ctx.text = [&own, k](const QString& key) { return textValue(*k, own, key); };
            ctx.numOf = [&](int other, const QString& key) {
                const CompNode* o = g.node(other);
                const NodeType* ok = o ? find(o->type) : nullptr;
                // A tracker's x/y/rx/ry by the follow rule (core/CompTrack.h): not analyzed yet = at its reference
                // point, so a follower does not move (Picture::Comp::trackerValue)
                if (ok && o->type == QLatin1String(::Comp::kTrackerType)) {
                    // Losing the point (0.22): its gaps and lost mode (seen, held values)
                    auto ft = followTimes.constFind(other); // once per tracker and frame (numOf runs ~7x a follower)
                    if (ft == followTimes.cend())
                        ft = followTimes.insert(other, followTime(env.clip, other, valuesOf(other), env.t, env.fps));
                    return ::Comp::followValue(
                        key, textValue(*ok, valuesOf(other), QStringLiteral("mode")),
                        [&](const QString& q) { return valuesOf(other).contains(q) || isAnimated(other, q); },
                        [&](const QString& q) { return valueOf(other, ok, q); }, &*ft);
                }
                return valueOf(other, ok, key);
            };
            ctx.input = [&, id](const QString& name) { return inputOf(id, name); };
            ctx.take = take;
            ctx.give = [&](Buf&& b) {
                if (b.px.empty()) return;
                if (env.giveBytes) env.giveBytes(std::move(b.px));
                b.px = {};
                --alive;
            };
            ctx.clipPicture = env.clipPicture;
            ctx.mediaPicture = env.mediaPicture;
            Buf result;
            if (k->eval(ctx, &result) && !result.isNull() && result.w == env.size.width()
                && result.h == env.size.height()) {
                if (isStatic && cache) {
                    s.kept = std::make_shared<const Buf>(std::move(result));
                    cache->put(key, s.kept);
                    s.pic = s.kept.get();
                    --alive; // kept by the cache now, not the pool
                } else {
                    s.own = std::move(result);
                    s.pic = &s.own;
                }
            } else if (!result.px.empty()) {
                if (env.giveBytes) env.giveBytes(std::move(result.px));
                --alive;
            }
            if (stats) ++stats->evaluated;
        }
        // Inputs read for the last time go back to the pool
        for (const CompLink& l : g.links)
            if (l.to == id && state.contains(l.from)) drop(l.from);
    }
    if (stats) stats->peakBuffers = std::max(stats->peakBuffers, peak);

    NodeState& res = state[g.out];
    const bool ok = res.pic && res.pic->kind == Buf::Rgba && res.pic->w == env.size.width()
                    && res.pic->h == env.size.height();
    if (ok) {
        if (res.pic == &res.own) {
            *out = std::move(res.own);
            res.own.px = {};
            --alive;
        } else {
            *out = take(Buf::Rgba);
            std::memcpy(out->data(), res.pic->data(), res.pic->px.size());
            out->area = res.pic->area;
            --alive;
        }
    }
    for (NodeState& s : state) release(s);
    return ok;
}

} // namespace Picture::Comp
