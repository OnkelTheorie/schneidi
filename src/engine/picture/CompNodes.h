// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Node kinds of the compositing graph (0.19, effect "comp", graph model in core/CompGraph.h).
// A node kind = inputs + parameters + a pure function that makes one picture (or mask) from its input pictures at
// clip time t. The engine (CompEval, package A) walks CompGraph::evalOrder(), hands each node its inputs and keeps
// static parts cached. Every package registers its kinds in its own file (CompNodesCore.cpp, CompNodesMask.cpp, …),
// listed once in CompNodes.cpp.
//
// Conventions (all node kinds):
// - Pictures are 8-bit RGBA with straight alpha in the OUTPUT size (ctx.size: the clip picture as the engine renders
//   it, preview or export), rows without gaps. Masks are one 8-bit plane of the same size (255 = inside).
// - Positions/sizes in parameters are relative to the picture (0..1, x right, y down; size 1 = picture width),
//   pixel amounts (blur radius, edge width, shadow offset) are in PROJECT pixels: multiply by ctx.scale.
// - A missing input is not an error: the node does what makes sense (merge without fg = bg; key without src = empty).
// - Thread-safe and without global state: several frames are evaluated in parallel (export). No allocation of
//   full pictures except through ctx.take (pooled).
// - Neutral defaults: a new node changes nothing it does not show (no glow, glitch or sound switched on by itself).

#include "core/EffectRegistry.h"

#include <QColor>
#include <QHash>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace Picture::Comp {

struct Buf {
    enum Kind { Rgba, Mask };
    Kind kind = Rgba;
    int w = 0, h = 0;
    std::vector<uint8_t> px; // w * h * 4 (Rgba) or w * h (Mask)
    // Where the content is; outside it the picture is transparent (Rgba) or 0 (Mask). Empty = the whole picture.
    // Lets later nodes skip empty pixels (a title is only as big as its text)
    QRect area;

    Buf() = default;
    Buf(Kind k, int width, int height) : kind(k), w(width), h(height), px(size_t(width) * height * channels(k), 0) {}
    static int channels(Kind k) { return k == Rgba ? 4 : 1; }
    int channels() const { return channels(kind); }
    bool isNull() const { return w <= 0 || h <= 0 || px.empty(); }
    uint8_t* data() { return px.data(); }
    const uint8_t* data() const { return px.data(); }
    int stride() const { return w * channels(); }
    QRect contentArea() const { return area.isEmpty() ? QRect(0, 0, w, h) : area; }
};

// Everything a node sees while it makes its picture
struct NodeCtx {
    int node = 0;         // id of this node (CompGraph)
    double t = 0;         // clip frame (fractional; keyframes already resolved in num/color/text)
    int64_t frameNo = 0;  // material frame (c.in + t): seeds per-frame patterns (noise, glitch)
    QSize size;           // output size in pixels
    double scale = 1;     // project pixels -> output pixels
    double fps = 25;      // project frame rate
    const std::atomic_bool* cancel = nullptr; // set = stop early (preview gave up on this frame); result unused

    // This node's parameter values at t (missing = the kind's default; keyframes of "comp" "n<id>.<key>")
    std::function<double(const QString& key)> num;
    std::function<QColor(const QString& key)> color;
    std::function<QString(const QString& key)> text;
    // Another node's value at t (a node following a tracker: "follow" = tracker id, then numOf(id, "x")).
    // Follow rule, the same for every follower (core/CompTrack.h): the tracker's reference point numOf(id, "rx"/"ry")
    // is where the tracked thing is at the start frame; the follower's own positions move by (x - rx, y - ry) and
    // scale by "scale" / turn by "angle" (clockwise) around (x, y), in pixels. A transform keeps its own x/y as its
    // placement and adds that. Not analyzed: x/y = rx/ry, scale 1, angle 0 = nothing moves
    std::function<double(int node, const QString& key)> numOf;

    // Picture at an input (nullptr = not connected or the node there made nothing)
    std::function<const Buf*(const QString& input)> input;
    // A cleared buffer of the output size from the pool (use it for the result and big scratch pictures)
    std::function<Buf(Buf::Kind)> take;
    // Gives a scratch buffer that came from take back to the pool once the node no longer needs it (only those)
    std::function<void(Buf&& scratch)> give;

    // Engine services for generator kinds (set by the engine; may be empty in the single-node test runner):
    // the clip's own picture with its effects (node "clip"), a media pool file's picture at a clip time
    // (node "media"; at output size, fitted like a clip). Empty Buf = none.
    std::function<const Buf*()> clipPicture;
    std::function<Buf(const QString& path, double clipTime)> mediaPicture;

    bool cancelled() const { return cancel && cancel->load(std::memory_order_relaxed); }
};

struct NodeType {
    QString id;          // stored in CompNode::type, never translated ("merge", "mask", …)
    QString name;        // shown, translated (T of the German key)
    QString description; // tooltip / `effects {kind:'nodes'}`
    QStringList inputs;  // input names ("bg", "fg", "mask", "src"); all optional
    // Parameters with their keys WITHOUT the "n<id>." prefix; `anim` is ignored (every Double is animatable through
    // Keys::effectParam("comp", "n<id>.<key>")); Path = a media pool file; Choice values are stored as they are
    QVector<EffectParam> params;
    Buf::Kind output = Buf::Rgba;
    // The picture changes with time even without keyframes and with static inputs (noise, glitch, media, clip,
    // a follower of a tracker). The engine caches a node when it is not varying, has no keyframes and all its inputs
    // are static
    bool varying = false;
    // Set: whether a node of this kind with these values (keys without prefix) varies, instead of `varying` (fx: only
    // an effect drawing a new pattern each frame)
    std::function<bool(const NodeType& k, const QVariantMap& values)> isVarying;
    // Set: the node's picture is this input's, unchanged (tracker). The engine hands that picture on without a copy;
    // eval is only used by the single-node runner
    QString passThrough;
    // Makes the node's picture into *out (taken from ctx.take or a moved input). false = nothing (as if not connected)
    std::function<bool(NodeCtx& ctx, Buf* out)> eval;

    const EffectParam* param(const QString& key) const;
};

// All registered kinds (registration order = order in lists); thread-safe after the first call
const QVector<NodeType>& all();
const NodeType* find(const QString& id);
// For CompGraph::validate
bool kindInfo(const QString& type, QStringList* inputs);

// ---- Single-node runner (tests of packages B–E, `frames node:` before the full engine): evaluates one kind with
// fixed parameter values (keys without prefix, missing = default) and input pictures. No keyframes, no engine
// services unless given.
struct RunOptions {
    QSize size{64, 36};
    double t = 0;
    int64_t frameNo = 0;
    double scale = 1;
    double fps = 25;
    std::function<Buf(const QString& path, double clipTime)> mediaPicture;
    const Buf* clipPicture = nullptr;
    QHash<int, QVariantMap> otherNodes; // values of other nodes for numOf (id -> {key: value})
};
// false = unknown kind or the node made nothing
bool runNode(const QString& type, const QVariantMap& params, const QHash<QString, Buf>& inputs, const RunOptions& opt,
             Buf* out);

// Value of a parameter in a QVariantMap of node values (missing = default), as num/color/text read it
double numValue(const NodeType& k, const QVariantMap& values, const QString& key);
QColor colorValue(const NodeType& k, const QVariantMap& values, const QString& key);
QString textValue(const NodeType& k, const QVariantMap& values, const QString& key);
// Does a node of kind k with these values change with time by itself (isVarying, else varying)?
bool varies(const NodeType& k, const QVariantMap& values);
// A Choice parameter's value as the index of its choice: the choice id ("ellipse") or its index (1, "1"); anything
// else (unknown id, index out of range) = the default. num() returns this index, text() the choice id (an id this
// version does not list as it is)
int choiceValue(const NodeType& k, const QVariantMap& values, const QString& key);

// Registration functions, one per package file (CompNodes.cpp calls each once)
// When a generator node (text, 3D text) shows within its clip: values "from"/"until" (seconds from the clip start,
// until 0 = to the end) and "fade" (seconds in and out). visibleRangeParams() = those three values,
// visibleWeight() = 0 outside, 1 inside, a ramp in the fades; rangeVaries() = the node changes with time because of it
QVector<EffectParam> visibleRangeParams();
double visibleWeight(const NodeCtx& ctx);
bool rangeVaries(const QVariantMap& values);

// The alpha of a picture's content area scaled by `opacity` (0..1; a generator's "opacity": fade a text in or out)
void fadeContent(Buf& b, double opacity);

void registerCoreNodes(QVector<NodeType>& out);  // A: clip, media, color, gradient, merge, transform, crop, blur, color grade, fx
void registerMaskNodes(QVector<NodeType>& out);  // B: mask, matte
void registerKeyNodes(QVector<NodeType>& out);   // C: key, matte_tune
void registerTrackNodes(QVector<NodeType>& out); // D: tracker
void registerStyleNodes(QVector<NodeType>& out); // E: text, shadow, glitch
void registerCutoutNodes(QVector<NodeType>& out); // cutout (AI person matte, CompNodesCutout.cpp)
void registerText3dNodes(QVector<NodeType>& out); // 3D text (round 3): text3d

} // namespace Picture::Comp
