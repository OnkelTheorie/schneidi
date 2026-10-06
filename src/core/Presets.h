#pragma once
// Effect presets from Shotcut and Kdenlive (effects folder, subfolder "Presets"). Both programs save MLT filters plus
// their properties, so a preset becomes schneidi effects wherever the filter is one schneidi knows (frei0r plugins,
// Green Screen; avfilter.lut3d -> LUT of the color correction). Read formats:
// - Kdenlive: custom effect (<effect tag="frei0r.glow"> with <parameter>/<property>) and effect group
//   (<effectgroup> with several <effect>, e.g. /usr/share/kdenlive/effect-templates)
// - Shotcut: filter set (MLT XML with <filter> elements, Shotcut/filter-sets) and filter preset (lines
//   "property=value", Shotcut/presets/<filter>/<name>: the filter comes from the folder name)
// Keyframes (MLT animation strings: Kdenlive "1=0;10=15", Shotcut "00:00:00.000=0.2;00:00:01.000=0.9") become
// schneidi keyframes for number parameters; interpolation types are approximated (smooth -> Bezier, eases -> Ease
// In/Out, discrete -> hold). Checkbox/color/choice parameters cannot be animated: their first value is used.

#include "core/Types.h"

#include <QHash>
#include <QMap>
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
    int parentIn = 0; // Kdenlive effect group: source in point of the clip it was saved from (keyframe offset)
    QVector<Filter> filters;
    QString error; // not readable / unknown format
};

// folderName = name of the folder the file lies in (Shotcut filter presets: filter name)
Preset parse(const QByteArray& data, const QString& fileName, const QString& folderName = {});
Preset load(const QString& path);

// MLT animation string -> keys in file order (frame relative to the start, negative = from the end; value text;
// MLT keyframe type character of the segment after the key: none = linear, '|' discrete, '~' smooth, 'a'.. eases).
// false = not an animation (plain value)
struct RawKey {
    int frame = 0;
    QString value;
    QChar type;
};
bool parseAnimation(const QString& v, double fps, QVector<RawKey>* keys);

// Number keys -> KeyTrack with interpolation approximated (see above). Negative frames count from the end
// (length + frame); frames stay relative to the start.
struct AnimKey {
    int frame = 0;
    double value = 0;
    QChar type;
};
KeyTrack toKeyTrack(QVector<AnimKey> keys, int length);

// Own presets (Inspector / Timeline → Clip → "Effekte als Preset speichern"): the clip's effects as a Kdenlive
// <effectgroup>, so parse() reads it back and Kdenlive can open it. MLT filters keep their service and property
// names (frei0r, Green Screen); schneidi's own effects (color correction, blur) are written with their registry id
// (only schneidi knows them). Keyframes become animation strings in frames from the clip start ("0=0.1;24a=0.8",
// eases as MLT sinusoidal types, Bezier as smooth); disabled effects get disable=1. Not saved: the Color page grade
// and effects whose plugin is missing (listed in `skipped`).
QByteArray toXml(const Clip& c, const QString& name, QStringList* skipped = nullptr);
// File for an own preset in the presets folder (name made safe as a file name)
QString userPresetPath(const QString& name);
bool save(const Clip& c, const QString& name, const QString& path, QString* error, QStringList* skipped = nullptr);

// Shotcut preset folder name (QML objectName or MLT service) -> MLT service, empty = unknown
QString shotcutService(const QString& folderName);

// A preset as schneidi effects
struct Mapped {
    QVector<EffectInstance> effects; // in preset order, parameters in the registry's types
    QString lut;                     // avfilter.lut3d: LUT file (empty = none or not found)
    QStringList skipped;             // filters schneidi does not know (MLT service)
    // Keyframes of number parameters; frames relative to the clip start (negative = from the clip end, like MLT)
    QMap<AnimParam, QVector<AnimKey>> keys;
    bool keyframes = false;          // animated values that cannot be animated here: only the first value taken
    bool empty() const { return effects.isEmpty() && lut.isEmpty(); }
};
// fps: project frame rate (keyframe times in clock format)
Mapped map(const Preset& p, double fps = 25.0);

// Apply to a clip: existing effects of the same kind get the preset's values (and keyframes), others are appended
void apply(Clip& c, const Mapped& m);

} // namespace Presets
