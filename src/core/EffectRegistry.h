#pragma once
// Katalog der verfügbaren Effekte. Ein Effekt = Beschreibung + Parameter (Inspector, Projektdatei, Keyframes).
// Einfache Effekte bilden ihre Parameter direkt auf einen MLT-Filter ab (mltService/mltProperty);
// Effekte mit eigener Umrechnung (Farbkorrektur, Unschärfe) baut der TimelineBuilder anhand der id.

#include "core/Types.h"

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

struct EffectParam {
    // Path = file path (project file also stores it relative); Bool = checkbox; Choice = one of `choices`
    enum Type { Double, Color, Path, Bool, Choice };
    QString key;         // Name in EffectInstance::params
    QString label;       // Anzeige im Inspector
    QString mltProperty; // Property am MLT-Filter (leer = Umrechnung im TimelineBuilder)
    Type type = Double;
    QVariant defaultValue;
    double min = 0, max = 1;
    AnimParam anim = AnimParam::Count; // Count = nicht animierbar
    double step = 1;                   // Zahlenfeld: Schritt pro Pixel beim Ziehen
    int decimals = 2;
    QStringList choices; // Choice: allowed values (passed to MLT as they are)
    QString description; // Tooltip
    // frei0r: parameter name of the plugin (MLT also accepts it instead of the index in mltProperty; Kdenlive's
    // effect files mostly use it)
    QString mltName;
};

struct EffectDescriptor {
    QString id;
    QString name;
    QString mltService;
    bool video = true; // false = Audioeffekt
    QVector<EffectParam> params;
    // In der Effects Library (Open FX → Filter) anbietbar; solche Effekte bekommen im Inspector einen eigenen,
    // entfernbaren Bereich. Nein = fester Bereich im Inspector (Green Screen).
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
// Effekt und Parameter zu einem animierbaren Effekt-Parameter (false = kein Effekt-Parameter)
bool paramFor(AnimParam p, const EffectDescriptor** effect, const EffectParam** param);

// Hilfen am Clip
const EffectInstance* instance(const Clip& c, const QString& id);
EffectInstance* instance(Clip& c, const QString& id);
bool has(const Clip& c, const QString& id);
// Neue Instanz mit Default-Werten (ans Ende = zuletzt gerendert). false = unbekannt oder schon vorhanden
bool add(Clip& c, const QString& id);
// Entfernt die Instanz samt Keyframes ihrer Parameter
void remove(Clip& c, const QString& id);
// Wert eines Parameters (fehlend = Default)
QVariant value(const Clip& c, const QString& id, const QString& key);
} // namespace EffectRegistry
