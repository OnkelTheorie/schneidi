// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Evaluation of a clip's compositing graph (effect "comp", core/CompGraph.h) at one clip time into one picture.
// Only the nodes the output depends on are evaluated (CompGraph::evalOrder); intermediate pictures come from the
// caller's buffer pool and go back to it as soon as the last node reading them is done (at most as many alive as the
// graph is wide); a static node (kind not varying, no keyframes, static inputs) is kept in a Cache keyed by its
// parameter hash + output size + its inputs' keys and evaluated once. A node kind this version does not know passes
// its first connected input through (a newer project's graph shows what it can instead of nothing).

#include "engine/picture/CompNodes.h"

#include <QSet>
#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

struct Clip;
struct EffectInstance;

namespace Picture::Comp {

// Results of static nodes, shared by the frames of one renderer (thread-safe), bounded in bytes
class Cache {
public:
    std::shared_ptr<const Buf> find(uint64_t key);
    void put(uint64_t key, std::shared_ptr<const Buf> b);
    void clear();
    size_t bytes() const;
    static constexpr size_t kMaxBytes = size_t(96) << 20;

private:
    mutable std::mutex m_mutex;
    std::map<uint64_t, std::shared_ptr<const Buf>> m_items;
    std::vector<uint64_t> m_order; // insertion order: the oldest goes first
    size_t m_bytes = 0;
};

struct Env {
    double t = 0;        // clip frame
    int64_t frameNo = 0; // material frame (c.in + t)
    QSize size;          // output size
    double scale = 1;    // project pixels -> output pixels
    double fps = 25;
    const std::atomic_bool* cancel = nullptr;
    const Clip* clip = nullptr; // keyframes of "comp" "n<id>.<key>"; null = none

    // The clip's own picture with its effects (node "clip", track -1); may render on first use
    std::function<const Buf*()> clipPicture;
    // A compound clip's inner track alone, painted onto `onto` exactly as the timeline lays tracks on each other
    // (node "clip" with track >= 0; empty = not a compound)
    std::function<bool(int track, Buf& onto)> paintTrack;
    // A media pool file's picture at clip time, at output size, fitted like a clip
    std::function<Buf(const QString& path, double clipTime)> mediaPicture;
    // Pooled storage of n bytes, cleared; and its return (empty = plain allocation)
    std::function<std::vector<uint8_t>(size_t n)> takeBytes;
    std::function<void(std::vector<uint8_t>&& v)> giveBytes;
};

struct Stats {
    int evaluated = 0;      // node evaluations run
    int cached = 0;         // static results taken from the cache
    int unknown = 0;        // nodes of unknown kinds passed through
    int peakBuffers = 0;    // pooled pictures alive at once
    QSet<QString> unknownKinds;
};

// The graph of `comp` evaluated into *out (Rgba, env.size). false = no graph, broken graph, cancelled or the output
// node made nothing: the clip's picture stays as it is
bool evaluate(const EffectInstance& comp, const Env& env, Cache* cache, Buf* out, Stats* stats = nullptr);

// Node kinds of a graph this version does not know (skippedEffects, missingEffects)
QStringList unknownKinds(const EffectInstance& comp);

// The graph as it works at clip frame t (0.22): nodes outside their time range (Comp::rangeAt: from/until, keyframes of
// `clip` when it has them) taken out of the picture's way - what read such a node reads its picture input instead
// (src, else bg, else fg: a merge out of its range shows its bg); a node without one (a generator) is read as one added
// transparent color node, also as the output. false = every node works at t (*out untouched: use comp). evaluate()
// does this itself on the graph it parses; the card's planner gets the result
bool activeGraph(const EffectInstance& comp, double t, double fps, const Clip* clip, EffectInstance* out);

} // namespace Picture::Comp
