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

// Media files the graph reads (path parameters of "media" nodes), in node order, without duplicates
QStringList mediaPaths(const EffectInstance& e);
// Rewrites every media path found in `moved` (relink, collect project); returns how many were changed
int redirectMedia(EffectInstance& e, const std::function<QString(const QString&)>& moved);
} // namespace Comp
