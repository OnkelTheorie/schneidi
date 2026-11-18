// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Catalog of the available effects. An effect = description + parameters (Inspector, project file, keyframes).
// Simple effects map their parameters directly to an MLT filter (mltService/mltProperty);
// effects with their own conversion (color correction, blur) are applied by the picture engine by id.

#include "core/Types.h"

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

struct EffectParam {
    // Path = file path (project file also stores it relative); Bool = checkbox; Choice = one of `choices`;
    // Text = free text (a filter chain, shader code of the custom effects; set through `edit`, not in the Inspector)
    enum Type { Double, Color, Path, Bool, Choice, Text };
    QString key;         // Name in EffectInstance::params
    QString label;       // display in the Inspector
    QString mltProperty; // parameter key of presets and old projects (empty = converted by the picture engine)
    Type type = Double;
    QVariant defaultValue;
    double min = 0, max = 1;
    AnimParam anim = AnimParam::Count; // Count = not animatable
    double step = 1;                   // number field: step per pixel when dragging
    int decimals = 2;
    QStringList choices; // Choice: allowed values (stored as they are)
    QString description; // Tooltip
    // frei0r: parameter name of the plugin (MLT also accepts it instead of the index in mltProperty; Kdenlive's
    // effect files mostly use it)
    QString mltName;
    // Choice: texts shown instead of `choices` (same order; the values stay in the project file). Empty = the values
    QStringList choiceLabels;
    // Changing it needs a new analysis pass (stabilization: what the analysis measures, not how it applies)
    bool analysis = false;
    // Not written into a new instance (missing = default): many optional values that are mostly unused, like the
    // windows of the Color page (core/GradeWindows)
    bool sparse = false;
    // Readable name for the CLI/MCP where the stored key is a plugin index ("0", "1" of the effects that replaced
    // frei0r): accepted wherever a key is, shown instead of it. Empty = the key. The project file keeps the key
    QString alias;
    QString name() const { return alias.isEmpty() ? key : alias; }
    // Shown text of a Choice value (label if there is one)
    QString choiceText(const QString& value) const
    {
        const int i = int(choices.indexOf(value));
        return i >= 0 && i < choiceLabels.size() ? choiceLabels[i] : value;
    }
};

// What an effect is for (Effects Library subcategory, `effects` category): video and audio effects each list the
// categories that have effects of their kind, in this order. Ids are stable (CLI/MCP filter); new ones go anywhere in
// the list (the order is the shown one, nothing is saved by number)
enum class EffectCategory { None, Color, Blur, Stylize, Keying, Repair, Voice, Custom };
struct EffectCategoryInfo {
    EffectCategory category;
    const char* id;   // "color", "blur", … (CLI/MCP)
    const char* name; // N_(): shown through T()
};
inline constexpr EffectCategoryInfo kEffectCategories[] = {
    {EffectCategory::Color, "color", N_("Farbe & Licht")},
    {EffectCategory::Blur, "blur", N_("Unschärfe")},
    {EffectCategory::Stylize, "stylize", N_("Stilisieren")},
    {EffectCategory::Keying, "keying", N_("Keying & Masken")},
    {EffectCategory::Repair, "repair", N_("Reparieren")},
    {EffectCategory::Voice, "voice", N_("Stimme")},
    {EffectCategory::Custom, "custom", N_("Eigener Code")},
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
    QString description; // Tooltip
    // Fixed stage: rendered at its own place in the chain, wherever it sits in Clip::effects (stabilization first,
    // then the Color page grade, then the Green Screen key; noise reduction per audio clip). It cannot be moved; all
    // other effects render after the fixed stages in the clip's order (Editor::moveEffect)
    bool fixed = false;
    EffectCategory category = EffectCategory::None; // what it is for (every effect has one; test_effects checks)
    QStringList keywords; // further words the Effects Library's search finds it by (both UI languages)
};

namespace EffectRegistry {
const QVector<EffectDescriptor>& all();
// Category id ("color") and shown name (translated) of a category; categoryFromId: false = unknown id
QString categoryId(EffectCategory c);
QString categoryName(EffectCategory c);
bool categoryFromId(const QString& id, EffectCategory* c);
const EffectDescriptor* find(const QString& id);
// Parameter of d by its stored key or its readable alias (EffectParam::alias); nullptr = none
const EffectParam* findParam(const EffectDescriptor& d, const QString& keyOrAlias);
// Stored key of a key or alias of effect `effectId` (unchanged if unknown), and the shown name of a stored key
QString storedKey(const QString& effectId, const QString& keyOrAlias);
QString shownKey(const QString& effectId, const QString& key);
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
// The user sets its place in the effect order: not a fixed stage (also an effect whose plugin is missing here)
bool movable(const QString& id);
// The movable effects of the clip in render order
QStringList order(const Clip& c);
// All effects of the clip in render order: the fixed stages (stabilize, grade, chromakey, then the audio ones), then
// the movable effects in the clip's order, the compositing graph ("comp") last
QVector<const EffectInstance*> renderOrder(const Clip& c);
// Value of a parameter (missing = default)
QVariant value(const Clip& c, const QString& id, const QString& key);
} // namespace EffectRegistry
