// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Compositing graph (0.19) without pixels: nodes, links, output node. It lives as the effect "comp" on a clip
// (EffectInstance::params): params["graph"] = toVariant(), the node values flat as "n<id>.<key>" next to it
// (like the windows "w<n>.<key>" of the grade), so keyframes work through Keys::effectParam("comp", "n3.size").
// Older versions keep the unknown effect as read and render the clip without it.
// The node kinds (inputs, parameters, pixels) are in engine/picture/CompNodes.h; core only knows their names.

#include <QPointF>
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
// media node's offset (frames, × its speed; keyframes too) and the visible range of text nodes (from/until in seconds,
// only with fps > 0). from 0 (= from the start) and until 0 (= to the end) stay; a range that ended before the new
// start becomes an empty one (the node shows nothing on this clip, as before). A NEW node kind with a value counted
// from the clip start must be added there (CompGraph.cpp)
void followHead(Clip& c, int frames, double fps);

// Every file the graph reads (fileKeys of its nodes: media, LUTs, textures), in node order, without duplicates
QStringList mediaPaths(const EffectInstance& e);
// Calls fn for every non-empty file value (fileKeys) with its full key "n<id>.<key>"; a changed path is stored
void forEachFile(EffectInstance& e, const std::function<void(const QString& key, QString& path)>& fn);
// Rewrites every file path found in `moved` (relink, templates); returns how many were changed
int redirectMedia(EffectInstance& e, const std::function<QString(const QString&)>& moved);
} // namespace Comp
