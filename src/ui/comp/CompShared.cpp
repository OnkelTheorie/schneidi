// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/comp/CompShared.h"

#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "engine/picture/CompNodes.h"

#include <algorithm>

namespace PC = Picture::Comp;

namespace CompUi {

CompGraph::KindInfo kinds()
{
    return [](const QString& type, QStringList* inputs) { return PC::kindInfo(type, inputs); };
}

bool graphOf(const Clip& c, CompGraph* g)
{
    const EffectInstance* e = EffectRegistry::instance(c, Comp::kEffectId);
    return e && Comp::graphOf(*e, g);
}

QVariantMap nodeValues(const Clip& c, int id)
{
    QVariantMap out;
    const EffectInstance* e = EffectRegistry::instance(c, Comp::kEffectId);
    if (!e) return out;
    for (auto it = e->params.begin(); it != e->params.end(); ++it) {
        int node = 0;
        QString key;
        if (CompGraph::splitParamKey(it.key(), &node, &key) && node == id) out.insert(key, it.value());
    }
    return out;
}

int clipFrame(const Clip& c, int playhead) { return std::clamp(playhead - c.start, 0, std::max(0, c.length() - 1)); }

static AnimParam paramOf(int node, const QString& key)
{
    return Keys::effectParam(Comp::kEffectId, CompGraph::paramKey(node, key));
}

double number(const Clip& c, int node, const PC::NodeType& k, const QString& key, int t)
{
    const AnimParam p = paramOf(node, key);
    if (Keys::animated(c, p)) return Keys::valueAt(c, p, t);
    return PC::numValue(k, nodeValues(c, node), key);
}

bool keyAt(const Clip& c, int node, const QString& key, int t) { return Keys::keyAt(c, paramOf(node, key), t) != nullptr; }

bool animated(const Clip& c, int node, const QString& key) { return Keys::animated(c, paramOf(node, key)); }

QString nodeName(const CompNode& n)
{
    const PC::NodeType* k = PC::find(n.type);
    return QStringLiteral("%1 %2").arg(k ? k->name : n.type).arg(n.id);
}

Timeline viewTimeline(Timeline tl, int clipId, int node, bool matte)
{
    if (node == 0 && !matte) return tl;
    for (Track& track : tl.video)
        for (Clip& c : track.clips) {
            if (c.id != clipId) continue;
            EffectInstance* e = EffectRegistry::instance(c, Comp::kEffectId);
            CompGraph g;
            if (!e || !Comp::graphOf(*e, &g)) return tl;
            const int shownNode = node ? node : g.out;
            const CompNode* n = g.node(shownNode);
            if (!n) return tl;
            const PC::NodeType* k = PC::find(n->type);
            int shown = shownNode;
            if (k && k->output == PC::Buf::Rgba && matte) { // its alpha as a mask
                const int m = g.nextId();
                g.nodes << CompNode{m, QStringLiteral("matte"), {}};
                g.links << CompLink{shownNode, m, QStringLiteral("src")};
                e->params[CompGraph::paramKey(m, QStringLiteral("channel"))] = QStringLiteral("alpha");
                shown = m;
            }
            if (k && (k->output == PC::Buf::Mask || shown != shownNode)) { // a mask as gray: white on black through it
                const int white = g.nextId(), black = white + 1, merge = white + 2;
                g.nodes << CompNode{white, QStringLiteral("color"), {}} << CompNode{black, QStringLiteral("color"), {}}
                        << CompNode{merge, QStringLiteral("merge"), {}};
                g.links << CompLink{black, merge, QStringLiteral("bg")} << CompLink{white, merge, QStringLiteral("fg")}
                        << CompLink{shown, merge, QStringLiteral("mask")};
                e->params[CompGraph::paramKey(white, QStringLiteral("color"))] = QStringLiteral("#ffffffff");
                e->params[CompGraph::paramKey(black, QStringLiteral("color"))] = QStringLiteral("#ff000000");
                shown = merge;
            }
            g.out = shown;
            Comp::setGraph(*e, g);
            return tl;
        }
    return tl;
}

} // namespace CompUi
