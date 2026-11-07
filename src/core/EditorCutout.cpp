// SPDX-License-Identifier: AGPL-3.0-only
// Editor op of the AI cutout (0.19): which analysis (matte cache file) a cutout node of a clip's graph uses.
#include "core/Editor.h"

#include "core/CompGraph.h"
#include "core/I18n.h"

bool Editor::cutoutCompNode(int clipId, int node, const QString& cache, QString* err)
{
    return editComp(clipId, T("Freistellen analysieren"), [&](CompGraph& g, EffectInstance& e, QString* why) {
        const CompNode* n = g.node(node);
        if (!n) {
            if (why) *why = QStringLiteral("no node %1 in the graph").arg(node);
            return false;
        }
        if (n->type != QLatin1String("cutout")) {
            if (why) *why = QStringLiteral("node %1 is a %2 node, not a cutout").arg(node).arg(n->type);
            return false;
        }
        e.params[CompGraph::paramKey(node, QStringLiteral("cache"))] = cache;
        return true;
    }, {}, err);
}
