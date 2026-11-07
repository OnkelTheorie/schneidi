// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/picture/CompNodes.h"

#include <QHash>

#include <algorithm>
#include <cmath>

namespace Picture::Comp {

const EffectParam* NodeType::param(const QString& key) const
{
    for (const EffectParam& p : params)
        if (p.key == key) return &p;
    return nullptr;
}

const QVector<NodeType>& all()
{
    static const QVector<NodeType> kinds = [] {
        QVector<NodeType> out;
        // One line per package file (keeps merges of parallel packages trivial)
        registerCoreNodes(out);
        registerMaskNodes(out);
        registerKeyNodes(out);
        registerTrackNodes(out);
        registerStyleNodes(out);
        return out;
    }();
    return kinds;
}

const NodeType* find(const QString& id)
{
    for (const NodeType& k : all())
        if (k.id == id) return &k;
    return nullptr;
}

bool kindInfo(const QString& type, QStringList* inputs)
{
    const NodeType* k = find(type);
    if (!k) return false;
    if (inputs) *inputs = k->inputs;
    return true;
}

namespace {

// Index of v among p's choices: an id or an index (number or digits); -1 = neither
int choiceIndexOf(const EffectParam& p, const QVariant& v)
{
    if (!v.isValid()) return -1;
    if (v.typeId() == QMetaType::QString) {
        const qsizetype i = p.choices.indexOf(v.toString());
        if (i >= 0) return int(i);
    }
    bool ok = false;
    const double d = v.toDouble(&ok);
    if (!ok || !std::isfinite(d)) return -1;
    const long i = std::lround(d);
    return i >= 0 && i < p.choices.size() ? int(i) : -1;
}

} // namespace

int choiceValue(const NodeType& k, const QVariantMap& values, const QString& key)
{
    const EffectParam* p = k.param(key);
    if (!p) return 0;
    const int i = choiceIndexOf(*p, values.value(key));
    return i >= 0 ? i : std::max(0, choiceIndexOf(*p, p->defaultValue));
}

double numValue(const NodeType& k, const QVariantMap& values, const QString& key)
{
    const EffectParam* p = k.param(key);
    if (p && p->type == EffectParam::Choice) return double(choiceValue(k, values, key));
    const QVariant v = values.value(key, p ? p->defaultValue : QVariant());
    if (p && p->type == EffectParam::Bool) return v.toBool() ? 1.0 : 0.0;
    return v.toDouble();
}

QColor colorValue(const NodeType& k, const QVariantMap& values, const QString& key)
{
    const EffectParam* p = k.param(key);
    const QVariant v = values.value(key, p ? p->defaultValue : QVariant());
    if (v.typeId() == QMetaType::QColor) return v.value<QColor>();
    return QColor(v.toString());
}

QString textValue(const NodeType& k, const QVariantMap& values, const QString& key)
{
    const EffectParam* p = k.param(key);
    if (p && p->type == EffectParam::Choice) { // an index as its id; an id this version does not list stays as it is
        const QVariant v = values.value(key);
        if (const int i = choiceIndexOf(*p, v); i >= 0) return p->choices.value(i);
        if (v.typeId() == QMetaType::QString && !v.toString().isEmpty()) return v.toString();
        return p->defaultValue.toString();
    }
    return values.value(key, p ? p->defaultValue : QVariant()).toString();
}

bool runNode(const QString& type, const QVariantMap& params, const QHash<QString, Buf>& inputs, const RunOptions& opt,
             Buf* out)
{
    const NodeType* k = find(type);
    if (!k || !k->eval) return false;
    NodeCtx ctx;
    ctx.node = 1;
    ctx.t = opt.t;
    ctx.frameNo = opt.frameNo;
    ctx.size = opt.size;
    ctx.scale = opt.scale;
    ctx.fps = opt.fps;
    ctx.num = [k, &params](const QString& key) { return numValue(*k, params, key); };
    ctx.color = [k, &params](const QString& key) { return colorValue(*k, params, key); };
    ctx.text = [k, &params](const QString& key) { return textValue(*k, params, key); };
    ctx.numOf = [&opt](int node, const QString& key) { return opt.otherNodes.value(node).value(key).toDouble(); };
    ctx.input = [&inputs](const QString& name) -> const Buf* {
        const auto it = inputs.constFind(name);
        return it == inputs.cend() || it->isNull() ? nullptr : &*it;
    };
    ctx.take = [&opt](Buf::Kind kind) { return Buf(kind, opt.size.width(), opt.size.height()); };
    if (opt.clipPicture) ctx.clipPicture = [&opt]() { return opt.clipPicture; };
    ctx.mediaPicture = opt.mediaPicture;
    Buf result;
    if (!k->eval(ctx, &result) || result.isNull()) return false;
    *out = std::move(result);
    return true;
}

} // namespace Picture::Comp
