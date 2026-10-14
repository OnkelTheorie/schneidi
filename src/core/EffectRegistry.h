#pragma once
// Catalog of the available effects. An effect = description + parameters (Inspector, project file, keyframes).
// Simple effects map their parameters directly to an MLT filter (mltService/mltProperty);
// effects with their own conversion (color correction, blur) are built by the TimelineBuilder by id.

#include "core/Types.h"

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

struct EffectParam {
    // Path = file path (project file also stores it relative); Bool = checkbox; Choice = one of `choices`
    enum Type { Double, Color, Path, Bool, Choice };
    QString key;         // Name in EffectInstance::params
    QString label;       // display in the Inspector
    QString mltProperty; // property on the MLT filter (empty = conversion in the TimelineBuilder)
    Type type = Double;
    QVariant defaultValue;
    double min = 0, max = 1;
    AnimParam anim = AnimParam::Count; // Count = not animatable
    double step = 1;                   // number field: step per pixel when dragging
    int decimals = 2;
    QStringList choices; // Choice: allowed values (passed to MLT as they are)
    QString description; // Tooltip
    // frei0r: parameter name of the plugin (MLT also accepts it instead of the index in mltProperty; Kdenlive's
    // effect files mostly use it)
    QString mltName;
    // Choice: texts shown instead of `choices` (same order; the values stay in the project file). Empty = the values
    QStringList choiceLabels;
    // Changing it needs a new analysis pass (stabilization: what vid.stab measures, not how it applies)
    bool analysis = false;
    // Shown text of a Choice value (label if there is one)
    QString choiceText(const QString& value) const
    {
        const int i = int(choices.indexOf(value));
        return i >= 0 && i < choiceLabels.size() ? choiceLabels[i] : value;
    }
};

struct EffectDescriptor {
    QString id;
    QString name;
    QString mltService;
    bool video = true; // false = audio effect (audio clips, Inspector tab Audio, Effects Library Audio FX)
    QVector<EffectParam> params;
    // Can be offered in the Effects Library (Open FX -> Filter); such effects get their own, removable
    // section in the Inspector. No = fixed section in the Inspector (Green Screen).
    bool library = false;
    QString category;    // Effects Library: empty = Open FX → Filter, "frei0r" = Open FX → frei0r
    QString description; // Tooltip
};

namespace EffectRegistry {
inline constexpr const char* Frei0rCategory = "frei0r";

const QVector<EffectDescriptor>& all();
// Add more effects (frei0r plugins, engine/Frei0r). Only at startup, before anyone holds pointers from find();
// ids that already exist are skipped.
void registerEffects(const QVector<EffectDescriptor>& effects);
const EffectDescriptor* find(const QString& id);
// Effect and parameter for an animatable effect parameter (false = not an effect parameter)
bool paramFor(AnimParam p, const EffectDescriptor** effect, const EffectParam** param);

// Helpers on the clip
const EffectInstance* instance(const Clip& c, const QString& id);
EffectInstance* instance(Clip& c, const QString& id);
bool has(const Clip& c, const QString& id);
// New instance with default values (at the end = rendered last). false = unknown or already present
bool add(Clip& c, const QString& id);
// Removes the instance together with the keyframes of its parameters
void remove(Clip& c, const QString& id);
// Value of a parameter (missing = default)
QVariant value(const Clip& c, const QString& id, const QString& key);
} // namespace EffectRegistry
