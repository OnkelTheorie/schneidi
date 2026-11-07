// SPDX-License-Identifier: AGPL-3.0-only
// A custom effect instance on a clip's picture (Renderer::applyEffects): its values and keyframes at clip time
#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "engine/picture/CustomEffects.h"

#include <QSet>

#include <mutex>

namespace Picture::Custom {

namespace {

// Each reason once per run (a broken effect would otherwise log every frame)
void warnOnce(const QString& effectId, const QString& why)
{
    static std::mutex mutex;
    static QSet<QString> said;
    std::lock_guard lock(mutex);
    const QString key = effectId + QLatin1Char('|') + why;
    if (said.contains(key) || said.size() > 100) return;
    said.insert(key);
    qWarning("Picture engine: effect %s skipped: %s", qPrintable(effectId), qPrintable(why));
}

} // namespace

bool applyToClip(const Clip& c, const EffectInstance& inst, double t, double fps, uint8_t* rgba, int width, int height)
{
    const EffectDescriptor* d = EffectRegistry::find(inst.effectId);
    if (!d) return false;
    const double seconds = fps > 0 ? t / fps : 0.0;
    QString why;
    bool ok = false;
    if (inst.effectId == QLatin1String(kFilterId)) {
        ok = applyFilter(inst.params.value(QLatin1String(kChainKey)).toString(), rgba, width, height, seconds, &why);
    } else if (inst.effectId == QLatin1String(kShaderId)) {
        ShaderValues v;
        v.seconds = seconds;
        v.frame = t;
        v.fps = fps;
        for (const EffectParam& p : d->params) {
            if (p.type == EffectParam::Double && p.key.size() == 2 && p.key[0] == QLatin1Char('p')) {
                const int i = p.key[1].digitValue() - 1;
                if (i < 0 || i >= 8) continue;
                v.p[i] = p.anim != AnimParam::Count && Keys::animated(c, p.anim)
                             ? Keys::valueAt(c, p.anim, t)
                             : inst.params.value(p.key, p.defaultValue).toDouble();
            } else if (p.type == EffectParam::Color) {
                (p.key == QLatin1String("color1") ? v.color1 : v.color2) =
                    inst.params.value(p.key, p.defaultValue).value<QColor>();
            }
        }
        ok = applyShader(inst.params.value(QLatin1String(kCodeKey)).toString(), v, rgba, width, height, &why);
    }
    if (!ok) warnOnce(inst.effectId, why);
    return ok;
}

} // namespace Picture::Custom
