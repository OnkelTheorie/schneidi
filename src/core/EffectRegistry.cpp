#include "core/EffectRegistry.h"

#include "core/I18n.h"

#include <QColor>

namespace EffectRegistry {

const QVector<EffectDescriptor>& all()
{
    static const QVector<EffectDescriptor> effects = {
        // frei0r statt avfilter.chromakey: liefert echte Transparenz und stürzt beim Beenden nicht ab
        {"chromakey", "Green Screen", "frei0r.bluescreen0r", true,
         {
             {"color", T("Farbe"), "0", EffectParam::Color, QColor(0, 255, 0), 0, 0},
             {"distance", T("Toleranz"), "1", EffectParam::Double, 0.3, 0.0, 1.0},
         }},
        // Einfache Farbkorrektur (alle Werte -100..+100, 0 = unverändert). Engine: avfilter.colorchannelmixer
        // (Temperatur/Tönung als Weißabgleich, Sättigung) + avfilter.colorlevels (Helligkeit, Kontrast)
        {"color", T("Farbkorrektur"), {}, true,
         {
             {"brightness", T("Helligkeit"), {}, EffectParam::Double, 0.0, -100, 100, AnimParam::FxBrightness, 0.5, 2},
             {"contrast", T("Kontrast"), {}, EffectParam::Double, 0.0, -100, 100, AnimParam::FxContrast, 0.5, 2},
             {"saturation", T("Sättigung"), {}, EffectParam::Double, 0.0, -100, 100, AnimParam::FxSaturation, 0.5, 2},
             {"temperature", T("Temperatur"), {}, EffectParam::Double, 0.0, -100, 100, AnimParam::FxTemp, 0.5, 2},
             {"tint", T("Tönung"), {}, EffectParam::Double, 0.0, -100, 100, AnimParam::FxTint, 0.5, 2},
         },
         true},
        // Stärke relativ zur Bildhöhe -> sieht in jeder Vorschau-/Exportgröße gleich aus. Engine: avfilter.gblur
        {"blur", T("Gaußsche Unschärfe"), {}, true,
         {
             {"strength", T("Stärke"), {}, EffectParam::Double, 25.0, 0, 100, AnimParam::FxBlur, 0.25, 2},
         },
         true},
    };
    return effects;
}

const EffectDescriptor* find(const QString& id)
{
    for (const auto& e : all())
        if (e.id == id) return &e;
    return nullptr;
}

bool paramFor(AnimParam p, const EffectDescriptor** effect, const EffectParam** param)
{
    if (p == AnimParam::Count) return false;
    for (const auto& e : all())
        for (const auto& x : e.params)
            if (x.anim == p) {
                if (effect) *effect = &e;
                if (param) *param = &x;
                return true;
            }
    return false;
}

const EffectInstance* instance(const Clip& c, const QString& id)
{
    for (const auto& e : c.effects)
        if (e.effectId == id) return &e;
    return nullptr;
}

EffectInstance* instance(Clip& c, const QString& id)
{
    for (auto& e : c.effects)
        if (e.effectId == id) return &e;
    return nullptr;
}

bool has(const Clip& c, const QString& id) { return instance(c, id) != nullptr; }

bool add(Clip& c, const QString& id)
{
    const EffectDescriptor* d = find(id);
    if (!d || has(c, id)) return false;
    EffectInstance inst;
    inst.effectId = id;
    for (const auto& p : d->params) inst.params[p.key] = p.defaultValue;
    c.effects << inst;
    return true;
}

void remove(Clip& c, const QString& id)
{
    c.effects.removeIf([&](const EffectInstance& e) { return e.effectId == id; });
    if (const EffectDescriptor* d = find(id))
        for (const auto& p : d->params)
            if (p.anim != AnimParam::Count) c.keys.remove(p.anim);
}

QVariant value(const Clip& c, const QString& id, const QString& key)
{
    if (const EffectInstance* e = instance(c, id); e && e->params.contains(key)) return e->params.value(key);
    if (const EffectDescriptor* d = find(id))
        for (const auto& p : d->params)
            if (p.key == key) return p.defaultValue;
    return {};
}

} // namespace EffectRegistry
