#pragma once
// Effect presets from Shotcut and Kdenlive (effects folder, subfolder "Presets"). Both programs save MLT filters plus
// their properties, so a preset becomes schneidi effects wherever the filter is one schneidi knows (frei0r plugins,
// Green Screen; avfilter.lut3d -> LUT of the color correction). Read formats:
// - Kdenlive: custom effect (<effect tag="frei0r.glow"> with <parameter>/<property>) and effect group
//   (<effectgroup> with several <effect>, e.g. /usr/share/kdenlive/effect-templates)
// - Shotcut: filter set (MLT XML with <filter> elements, Shotcut/filter-sets) and filter preset (lines
//   "property=value", Shotcut/presets/<filter>/<name>: the filter comes from the folder name)
// Keyframes (animation strings "1=0;10=15") are not taken over: the first value is used.

#include "core/Types.h"

#include <QHash>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

namespace Presets {

// Effects Library entry / drag data of a preset: "preset:<path>" (instead of an effect id)
inline constexpr const char* Prefix = "preset:";

struct Filter {
    QString service;                          // MLT service, e.g. "frei0r.glow"
    QVector<QPair<QString, QString>> props;   // MLT property -> value, as in the file
    QHash<QString, double> factors;           // Kdenlive <parameter factor>: shown value = MLT value * factor
};

struct Preset {
    QString name;
    QString description;
    QString source; // "Kdenlive" / "Shotcut"
    QVector<Filter> filters;
    QString error; // not readable / unknown format
};

// folderName = name of the folder the file lies in (Shotcut filter presets: filter name)
Preset parse(const QByteArray& data, const QString& fileName, const QString& folderName = {});
Preset load(const QString& path);

// A preset as schneidi effects
struct Mapped {
    QVector<EffectInstance> effects; // in preset order, parameters in the registry's types
    QString lut;                     // avfilter.lut3d: LUT file (empty = none or not found)
    QStringList skipped;             // filters schneidi does not know (MLT service)
    bool keyframes = false;          // animated values, only the first value taken
    bool empty() const { return effects.isEmpty() && lut.isEmpty(); }
};
Mapped map(const Preset& p);

// Apply to a clip: existing effects of the same kind get the preset's values, others are appended
void apply(Clip& c, const Mapped& m);

// Shotcut preset folder name (QML objectName or MLT service) -> MLT service, empty = unknown
QString shotcutService(const QString& folderName);

} // namespace Presets
