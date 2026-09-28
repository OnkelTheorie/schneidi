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
    };
    return effects;
}

const EffectDescriptor* find(const QString& id)
{
    for (const auto& e : all())
        if (e.id == id) return &e;
    return nullptr;
}

} // namespace EffectRegistry
