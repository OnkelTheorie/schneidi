#include "core/EffectRegistry.h"

#include <QColor>

namespace EffectRegistry {

const QVector<EffectDescriptor>& all()
{
    static const QVector<EffectDescriptor> effects = {
        {"chromakey", "Green Screen", "avfilter.chromakey", true,
         {
             {"color", "Farbe", "av.color", EffectParam::Color, QColor(0, 255, 0), 0, 0},
             {"similarity", "Toleranz", "av.similarity", EffectParam::Double, 0.15, 0.01, 1.0},
             {"blend", "Weichheit", "av.blend", EffectParam::Double, 0.05, 0.0, 1.0},
         }},
        {"volume", "Lautstärke", "volume", false,
         {
             {"gain", "Pegel (dB)", "level", EffectParam::Double, 0.0, -60.0, 12.0},
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
