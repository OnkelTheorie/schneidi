// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/comp/CompShared.h"

#include "core/CompRecipes.h"
#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "engine/picture/CompNodes.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QUndoStack>

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

QString nodeSummary(const Clip& c, const CompNode& n)
{
    const PC::NodeType* k = PC::find(n.type);
    if (!k) return {};
    const QVariantMap v = nodeValues(c, n.id);
    const auto choice = [&](const QString& key) {
        const EffectParam* p = k->param(key);
        return p ? p->choiceText(PC::textValue(*k, v, key)) : QString();
    };
    const auto number = [&](double x, int decimals) {
        return QString::number(x, 'f', decimals).replace('.', I18n::decimalPoint());
    };
    const auto oneLine = [](QString t) { return t.replace('\n', ' ').simplified(); };
    const QString id = k->id;
    if (id == QLatin1String("merge")) {
        const double opacity = PC::numValue(*k, v, QStringLiteral("opacity"));
        return opacity < 0.9995 ? QStringLiteral("%1 · %2 %").arg(choice("mode"), number(opacity * 100, 0)) : choice("mode");
    }
    if (id == QLatin1String("fx")) {
        const QString effect = PC::textValue(*k, v, QStringLiteral("effect"));
        const EffectDescriptor* d = EffectRegistry::find(effect);
        return d ? d->name : effect;
    }
    if (id == QLatin1String("text") || id == QLatin1String("text3d"))
        return QStringLiteral("„%1“").arg(oneLine(PC::textValue(*k, v, QStringLiteral("text"))));
    if (id == QLatin1String("color")) return PC::colorValue(*k, v, QStringLiteral("color")).name().toUpper();
    if (id == QLatin1String("blur")) return T("Radius %1").arg(number(PC::numValue(*k, v, QStringLiteral("radius")), 1));
    // Otherwise: a file's name, else the first choice (mask shape, key mode, tracker mode …)
    for (const EffectParam& p : k->params)
        if (p.type == EffectParam::Path && Comp::fileKeys(id).contains(p.key))
            if (const QString f = PC::textValue(*k, v, p.key); !f.isEmpty()) return QFileInfo(f).fileName();
    for (const EffectParam& p : k->params)
        if (p.type == EffectParam::Choice) return choice(p.key);
    return {};
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

bool applyRecipe(Editor* editor, const QVector<int>& ids, const QString& idOrName, const QVariantMap& with, QString* err)
{
    QString why;
    const std::optional<CompRecipe> r = CompRecipes::find(idOrName, &why);
    CompGraph g;
    QHash<int, QVariantMap> values;
    if (!r || !CompRecipes::apply(*r, with, &g, &values, &why)) {
        if (err) *err = why;
        return false;
    }
    QUndoStack* undo = editor->project()->undoStack();
    const int before = undo->index();
    undo->beginMacro(T("Rezept „%1“ anwenden").arg(CompRecipes::shownName(*r)));
    const bool ok = editor->setCompGraph(ids, g, values, kinds(), &why);
    undo->endMacro();
    if (!ok && undo->index() > before) undo->undo(); // an empty macro: gone again
    if (!ok && err) *err = why;
    return ok;
}

QString applyRecipeAsking(QWidget* parent, Editor* editor, const QVector<int>& ids, const QString& idOrName)
{
    QString err;
    const std::optional<CompRecipe> r = CompRecipes::find(idOrName, &err);
    QVariantMap with;
    if (r)
        for (const CompRecipeSlot& slot : r->slotList) {
            if (!slot.required || (slot.type != QLatin1String("media") && slot.type != QLatin1String("file"))) continue;
            const QString file = QFileDialog::getOpenFileName(
                parent, T("Rezept „%1“: %2").arg(CompRecipes::shownName(*r), slot.description.isEmpty() ? slot.id : slot.description));
            if (file.isEmpty()) return {};
            with.insert(slot.id, file);
        }
    if (r && applyRecipe(editor, ids, r->id, with, &err)) return r->note.isNull() ? QStringLiteral("") : r->note;
    QMessageBox::warning(parent, T("Rezept anwenden"), T("Das Rezept lässt sich nicht anwenden: %1").arg(err));
    return {};
}

} // namespace CompUi
