// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Compositing graph (0.19) without pixels: nodes, links, output node. It lives as the effect "comp" on a clip
// (EffectInstance::params): params["graph"] = toVariant(), the node values flat as "n<id>.<key>" next to it
// (like the windows "w<n>.<key>" of the grade), so keyframes work through Keys::effectParam("comp", "n3.size").
// Older versions keep the unknown effect as read and render the clip without it.
// The node kinds (inputs, parameters, pixels) are in engine/picture/CompNodes.h; core only knows their names.

#include <QPointF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

#include <functional>

struct Clip;
struct EffectInstance;

struct CompNode {
    int id = 0;
    QString type; // node kind id ("merge", "mask", "key", ...)
    QPointF pos;  // place on the node canvas (app); no meaning for the picture
    bool operator==(const CompNode&) const = default;
};

// Picture of node `from` goes into input `input` of node `to` ("bg", "fg", "mask", "src", ...)
struct CompLink {
    int from = 0;
    int to = 0;
    QString input;
    bool operator==(const CompLink&) const = default;
};

struct CompGraph {
    static constexpr int kVersion = 1; // graph version: a newer one may hold node kinds this version skips
    QVector<CompNode> nodes;
    QVector<CompLink> links;
    int out = 0; // id of the node whose picture is the result (0 = none: the clip's picture stays)
    int version = kVersion;
    bool operator==(const CompGraph&) const = default;

    bool isEmpty() const { return nodes.isEmpty(); }
    const CompNode* node(int id) const;
    int nextId() const; // a free id (largest + 1, ids start at 1)
    // The link into an input of a node (nullptr = not connected)
    const CompLink* linkInto(int to, const QString& input) const;

    // Storage form: {nodes:[{id, type, x, y}], links:[[from, to, input]], out, v}
    QVariantMap toVariant() const;
    // false + err for a broken form (no list, ids not unique or < 1, a link to a missing node); unknown fields are
    // ignored. Does not check node kinds (validate does)
    static bool fromVariant(const QVariant& v, CompGraph* out, QString* err = nullptr);

    // Checks against the node kinds: kinds(type, &inputs) returns false for an unknown kind, else fills its inputs.
    // Errors: unknown kind (only when !allowUnknown), link into an input the kind does not have, two links into one
    // input, a cycle, `out` missing. Message in English for CLI/MCP
    using KindInfo = std::function<bool(const QString& type, QStringList* inputs)>;
    bool validate(const KindInfo& kinds, QString* err, bool allowUnknown = false) const;

    // Ids of the nodes `out` depends on (itself included), each after all nodes it reads: the evaluation order.
    // Empty when out is missing or the graph has a cycle
    QVector<int> evalOrder() const;

    // Parameter keys of node values in EffectInstance::params: "n<id>.<key>"
    static QString paramKey(int id, const QString& key);
    static bool splitParamKey(const QString& full, int* id, QString* key);
};

namespace Comp {
inline constexpr const char* kEffectId = "comp";
inline constexpr const char* kGraphKey = "graph";
// The node that reads a media file keeps it in this parameter ("n<id>.path"); the file is a media pool item
inline constexpr const char* kMediaType = "media";
inline constexpr const char* kPathKey = "path";

// Graph of an instance of effect "comp" (false = none or broken)
bool graphOf(const EffectInstance& e, CompGraph* g);
void setGraph(EffectInstance& e, const CompGraph& g);
// Removes the values of nodes that are no longer in the graph (after removing a node)
void dropOrphanParams(EffectInstance& e);

// Further node kinds with a file parameter (the engine's kinds, core knows only their ids and these keys)
inline constexpr const char* kGradeType = "grade";
inline constexpr const char* kLutKey = "lut";
inline constexpr const char* kTextType = "text";
inline constexpr const char* kTextureKey = "texture";

// Parameter keys of a node kind whose value is a FILE path: media "path", grade "lut", text "texture"; an unknown
// kind has none. (A Path parameter is not always a file: the text node keeps its words and font in Path ones.)
// Everything that follows files - project file relative paths and LUT fallback, relink, templates (copy, export,
// redirect), media pool "in use", render cache key - goes through this table: a NEW node kind or key holding a file
// must be added here (CompGraph.cpp), or its file is missed.
QStringList fileKeys(const QString& type);
// Parameter keys of a node kind whose value is in PROJECT pixels (the engine multiplies them by ctx.scale): they scale
// with the project's resolution (ProjectFormat::scaleTimeline), static value and keyframes. A NEW pixel parameter must
// be added here (CompGraph.cpp), or it stays as it was on a 1080p -> 4K change. Unknown kind: none
QStringList pixelKeys(const QString& type);

// The clip's first frame moved `frames` further into its content (a head trim, the right piece of a cut; negative =
// back out): values that count from the clip's start follow, so the picture stays where it was in the timeline - a
// media node's offset (frames × its speed - an animated one: the speed at the new first frame; keyframes too) and the time
// range of every node (from/until in seconds, fps <= 0: nothing moves). from 0 (= from the start) and until 0 (= to the end) stay, others move below 0 too (until < 0
// = over before the start: invisible), so trimming back out restores them exactly. A NEW node kind with a value counted
// from the clip start must be added there (CompGraph.cpp)
void followHead(Clip& c, int frames, double fps);

// Node kinds whose picture is a mask (gray), not RGBA: "mask", "matte" (Picture::Comp::NodeType::output == Mask). Such
// a node never is the graph's output (the engine would ignore the whole graph). A NEW mask kind goes here
// (CompGraph.cpp); test_comp_graph compares the table with the engine's kinds
bool outputsMask(const QString& type);
// Adding a node of `type` behind a node of `fromType` (empty = none): a mask behind a picture is only added, not wired
// in (its input combines masks; it goes into a merge's mask input) - else the output would become a mask. The rule of
// Editor::addCompNode, shared with the node canvas
bool maskBehindPicture(const QString& type, const QString& fromType);

// ---- Time range of a node (0.22) ----
// Every node kind with a picture output has the values "from"/"until": seconds from the clip's start in which the
// node works (from 0 = from the start, until 0 = to the end; below 0 after a head trim, see followHead). Outside it a
// node with a picture input hands that on unchanged (src, else bg: a merge without its fg), a generator gives nothing
// (Picture::Comp::activeGraph). Mask kinds and the tracker (its picture is its input's) have none.
inline constexpr const char* kFromKey = "from";
inline constexpr const char* kUntilKey = "until";
bool hasRange(const QString& type);
// A range other than the whole clip (from > 0 or until != 0)
bool rangeSet(double from, double until);
// Whether a node with this range works at second `s` of its clip (until < 0: over before the start, never)
bool inRange(double from, double until, double s);
struct NodeRange {
    int node = 0;
    QString type;
    double from = 0, until = 0; // seconds from the clip's start
    bool keyed = false;         // from/until have keyframes: the values at the asked clip frame (not draggable)
};
// from/until of node `node` at clip frame t as the engine reads them: keyframes of `clip` (when given and set) win over
// the stored values. keyed = one of them has keyframes
void rangeAt(const EffectInstance& e, const Clip* clip, int node, double t, double* from, double* until,
             bool* keyed = nullptr);
// The range of node `node` (rangeAt at clip frame t). false = no such node, a kind without a range or no range set
// (the whole clip)
bool activeRange(const EffectInstance& e, int node, NodeRange* r = nullptr, const Clip* clip = nullptr, double t = 0);
// The nodes of the graph with a range set, in the graph's node order (rangeAt at clip frame t)
QVector<NodeRange> nodeRanges(const EffectInstance& e, const Clip* clip = nullptr, double t = 0);
// A range as timeline frames [first, end) of clip `c` (clamped to the clip; first == end when it lies outside it)
void rangeFrames(const Clip& c, const NodeRange& r, double fps, int* first, int* end);
// Timeline frames [first, end) of clip `c` as stored values: first at the clip's start = from 0; end at the clip's end
// = until 0 ("to the end", it grows with the clip) when openEnd, else the exact time
void rangeValues(const Clip& c, int first, int end, double fps, double* from, double* until, bool openEnd = false);

// ---- Shared camera (0.22, engine/picture/CompCamera.h) ----
// A node "camera" has no picture, only values; layers stand at the distance "z" from it (0 = not in its space: the
// camera leaves them alone) and name it in their value "camera" (read like a tracker through `follow`, not a link;
// 0 = the graph's camera: one camera per graph)
inline constexpr const char* kCameraType = "camera";
inline constexpr const char* kCameraKey = "camera";
inline constexpr const char* kLayerDepthKey = "z";
// The kinds that stand in a camera's space (values z and camera): transform, media, text, text3d. A NEW layer kind
// goes here (CompGraph.cpp)
bool takesCamera(const QString& type);
// The graph's camera: its first node of kind camera, 0 = none
int graphCamera(const CompGraph& g);
// A node's number at clip frame t as the engine reads it: keyframes of `clip` (when given and set) win over the stored
// value (missing = fallback)
double nodeNumber(const EffectInstance& e, const Clip* clip, int node, const QString& key, double t, double fallback = 0);
// Above 0 anywhere: the stored value, or (keyframed in `clip`) any of its keyframes
bool everPositive(const EffectInstance& e, const Clip* clip, int node, const QString& key);
// The camera node a layer node names (nodeNumber at clip frame t): its value camera when that is a node of kind
// camera, camera 0 = the graph's camera; 0 = none (also for a kind without a camera). Its depth is not looked at
int cameraOf(const EffectInstance& e, const CompGraph& g, int node, const Clip* clip = nullptr, double t = 0);
// A layer in a camera's space at some time (everPositive z and a camera): that camera, else 0 (check, info)
int layerCameraOf(const EffectInstance& e, const CompGraph& g, int node, const Clip* clip = nullptr);
// The nodes read through a value, not a link: trackers named by `follow`, cameras named by `camera` and the
// trackers those cameras follow, for the nodes in `nodes` (check and info count them as used)
QSet<int> valueReads(const EffectInstance& e, const CompGraph& g, const QVector<int>& nodes, const Clip* clip = nullptr);

// Every file the graph reads (fileKeys of its nodes: media, LUTs, textures), in node order, without duplicates
QStringList mediaPaths(const EffectInstance& e);
// Calls fn for every non-empty file value (fileKeys) with its full key "n<id>.<key>"; a changed path is stored
void forEachFile(EffectInstance& e, const std::function<void(const QString& key, QString& path)>& fn);
// Rewrites every file path found in `moved` (relink, templates); returns how many were changed
int redirectMedia(EffectInstance& e, const std::function<QString(const QString&)>& moved);
} // namespace Comp
