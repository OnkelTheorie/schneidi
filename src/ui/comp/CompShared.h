// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Shared helpers of the Comp page (ui/comp): the clip's graph and node values as the page shows them, the node kinds
// for the Editor's comp ops, and the timeline the viewer plays while it shows one node or a matte.

#include "core/CompGraph.h"
#include "core/Types.h"

#include <QColor>
#include <QString>
#include <QVariantMap>

class Editor;
class Project;
class QUndoStack;
class QWidget;
struct CompRecipe;

namespace Picture::Comp {
struct NodeType;
}

namespace CompUi {

// The node kinds as the Editor's comp ops check them (Picture::Comp::kindInfo)
CompGraph::KindInfo kinds();
// The clip's graph (false = no comp effect or a broken graph)
bool graphOf(const Clip& c, CompGraph* g);
// Values of node `id` without the "n<id>." prefix (only those stored; missing = the kind's default)
QVariantMap nodeValues(const Clip& c, int id);
// Clip frame of timeline frame `playhead` (limited to the clip)
int clipFrame(const Clip& c, int playhead);
// A node's number as the picture uses it at clip frame t: keyframes, else the stored value, else the default
double number(const Clip& c, int node, const Picture::Comp::NodeType& k, const QString& key, int t);
// Is there a keyframe of the number at clip frame t / any keyframe at all?
bool keyAt(const Clip& c, int node, const QString& key, int t);
bool animated(const Clip& c, int node, const QString& key);
// Shown name of a node: its kind's name and id ("Maske 3"); unknown kinds by their id
QString nodeName(const CompNode& n);
// An input port as the canvas shows it ("bg" -> "Hintergrund"; the stored name stays) and what it is for (tooltip)
QString portLabel(const QString& input);
// A new node from the node search: linked behind the selected one (default) or only added (the user wires it)
bool autoLink();
void setAutoLink(bool on);
QString portHelp(const QString& input);

// One line about what the node does, for its box on the canvas: the kind's key value (merge mode and opacity,
// mask shape, the text, the wrapped effect's name, the media file, a color …); empty = nothing to say
QString nodeSummary(const Clip& c, const CompNode& n);

// The custom code (filter chain, shader) of fx node `node` that is not trusted yet (core/CodeTrust.h) and so skipped
// when rendering; empty = none, or trusted
QString untrustedCode(const Clip& c, int node);

// Tracker node `node` of clip c lost its point (gaps its followers do not bridge, core/CompTrack.h): where, for the
// node's warning badge and the Inspector ("Punkt verloren: 00:12–00:14 · ab 00:20 nicht wiedergefunden"); empty = no
// such gap, or no tracker. fps = the timeline's (the times are timeline times)
QString trackerWarning(const Clip& c, int node, double fps);

// `tl` with the graph of clip `clipId` ending at node `node` (0 = its own output) and, with `matte`, the alpha of
// that picture as gray (a mask node always shows as gray): what the viewer plays on the Comp page. Unchanged when the
// clip has no graph or the node is missing.
Timeline viewTimeline(Timeline tl, int clipId, int node, bool matte);

// ---- Recipes (core/CompRecipes.h) in the app ----
// Effects Library entry / drag data of a recipe: "recipe:<id>" (sent like an effect id; the Edit page's effect drop
// and double-click hand it to applyRecipe)
inline constexpr const char* kRecipePrefix = "recipe:";
inline bool isRecipe(const QString& effectId) { return effectId.startsWith(QLatin1String(kRecipePrefix)); }
// Recipe `idOrName` on the video clips `ids` with the slots of `with` (CompRecipes::apply + Editor::setCompGraph,
// one undo step named after the recipe). Media/file slot values are taken as absolute paths. False + err when the
// recipe is missing, a slot does not fit or no clip takes it
// A step whose edits were all refused (an empty macro on top, above index `before`): taken off the history, also from
// redo (QUndoCommand::setObsolete), so no "Redo: …" of nothing is left
void dropRefusedStep(QUndoStack* undo, int before);

bool applyRecipe(Editor* editor, const QVector<int>& ids, const QString& idOrName, const QVariantMap& with = {},
                 QString* err = nullptr);
// The same from a GUI act: asks for the files of the recipe's required media/file slots first (a file dialog over
// `parent`; cancelled = nothing happens) and shows a failure in a message box. Returns the recipe's note ("next
// steps") on success (may be empty), a null string when nothing was applied. Custom code of the recipe that is not
// trusted (one from a ZIP, core/CodeTrust.h) is asked about right after applying (askTrust)
QString applyRecipeAsking(QWidget* parent, Editor* editor, const QVector<int>& ids, const QString& idOrName);
// The TrustDialog for the untrusted custom code on the comp graphs of clips `ids` after recipe `r` was applied (the
// apply stays its own undo step): "Ausführen" trusts it and remembers it for the recipe file, "Nicht ausführen" keeps
// it skipped. Nothing untrusted: no question
void askTrust(QWidget* parent, Editor* editor, const QVector<int>& ids, const CompRecipe& r);

} // namespace CompUi
