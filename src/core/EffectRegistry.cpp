#include "core/EffectRegistry.h"

#include "core/GradeWindows.h"
#include "core/I18n.h"
#include "core/Keyframes.h"

#include <QColor>

#include <algorithm>

namespace EffectRegistry {

namespace {

// The grade gets the parameters of its windows (Color page, core/GradeWindows)
QVector<EffectDescriptor> withWindows(QVector<EffectDescriptor> effects)
{
    for (EffectDescriptor& d : effects)
        if (d.id == QLatin1String("grade")) d.params += GradeWindows::params(d.params);
    return effects;
}

// Number parameter of an effect that replaced a frei0r plugin: key and property = the plugin's index, keyframes like
// the plugin's (Keys::effectParam), mltName = the plugin's name (Kdenlive presets)
EffectParam f0rNumber(const char* effect, const char* index, const QString& label, const char* name, double def,
                      const QString& description = {})
{
    EffectParam p{index, label, index, EffectParam::Double, def, 0.0, 1.0, Keys::effectParam(effect, index), 0.002, 3};
    p.mltName = QString::fromLatin1(name);
    p.description = description;
    return p;
}
EffectParam f0rOther(EffectParam::Type type, const char* index, const QString& label, const char* name, QVariant def)
{
    EffectParam p{index, label, index, type, std::move(def), 0, 0};
    p.mltName = QString::fromLatin1(name);
    return p;
}

// Own picture effects in place of the frei0r plugins schneidi offered (engine/VideoEffects). Same ids, parameter keys
// and value ranges as the plugins, so old projects, keyframes and Kdenlive/Shotcut presets keep working; the
// service stays the frei0r name for the preset files.
QVector<EffectDescriptor> ownLooks()
{
    QVector<EffectDescriptor> out;
    auto add = [&](const char* id, const QString& name, const QString& description, QVector<EffectParam> params) {
        EffectDescriptor d;
        d.id = QString::fromLatin1(id);
        d.name = name;
        d.mltService = d.id;
        d.params = std::move(params);
        d.library = true;
        d.description = description;
        out << d;
    };
    add("frei0r.glow", T("Glühen"), T("Weicher, heller Schein um helle Stellen"),
        {f0rNumber("frei0r.glow", "0", T("Unschärfe"), "Blur", 0.0)});
    add("frei0r.colorhalftone", T("Rasterdruck"), T("Farbige Rasterpunkte wie im Zeitungsdruck"),
        {f0rNumber("frei0r.colorhalftone", "0", T("Punktgröße"), "dot radius", 0.4),
         f0rNumber("frei0r.colorhalftone", "1", T("Winkel Cyan"), "cyan angle", 0.3),
         f0rNumber("frei0r.colorhalftone", "2", T("Winkel Magenta"), "magenta angle", 0.45),
         f0rNumber("frei0r.colorhalftone", "3", T("Winkel Gelb"), "yellow angle", 0.25)});
    add("frei0r.rgbnoise", T("Bildrauschen"), T("Farbiges Rauschen, in jedem Bild anders"),
        {f0rNumber("frei0r.rgbnoise", "0", T("Stärke"), "noise", 0.2)});
    add("frei0r.scanline0r", T("Fernsehzeilen"), T("Jede zweite Zeile dunkler, wie ein altes Fernsehbild"), {});
    add("frei0r.nosync0r", T("Gestörter Bildlauf"), T("Bild seitlich verschoben wie bei einem Fernseher ohne Synchronisation"),
        {f0rNumber("frei0r.nosync0r", "0", T("Versatz"), "HSync", 0.0)});
    {
        EffectParam shape = f0rOther(EffectParam::Choice, "0", T("Form"), "pattern", QString("gradient_linear"));
        shape.choices = {"gradient_linear", "gradient_radial"};
        shape.choiceLabels = {T("Linear"), T("Radial")};
        EffectParam blend = f0rOther(EffectParam::Choice, "10", T("Mischmodus"), "blend mode", QString("normal"));
        blend.choices = {"normal",     "add",       "saturate",   "multiply",   "screen",        "overlay",
                         "darken",     "lighten",   "colordodge", "colorburn",  "hardlight",     "softlight",
                         "difference", "exclusion", "hslhue",     "hslsaturation", "hslcolor",   "hslluminosity"};
        blend.choiceLabels = {T("Normal"),        T("Addieren"),        T("Sättigen"),      T("Multiplizieren"),
                              T("Negativ multiplizieren"), T("Überlagern"), T("Abdunkeln"), T("Aufhellen"),
                              T("Farbig abwedeln"), T("Farbig nachbelichten"), T("Hartes Licht"), T("Weiches Licht"),
                              T("Differenz"),     T("Ausschluss"),      T("Farbton"),       T("Sättigung"),
                              T("Farbe"),         T("Luminanz")};
        const char* g = "frei0r.cairogradient";
        add(g, T("Verlauf"), T("Farbverlauf über dem Bild (linear oder rund), mit Deckkraft und Mischmodus"),
            {shape, f0rOther(EffectParam::Color, "1", T("Startfarbe"), "start color", QColor(0, 0, 0)),
             f0rNumber(g, "2", T("Deckkraft Start"), "start opacity", 0.5),
             f0rOther(EffectParam::Color, "3", T("Endfarbe"), "end color", QColor(255, 255, 255)),
             f0rNumber(g, "4", T("Deckkraft Ende"), "end opacity", 0.5), f0rNumber(g, "5", T("Start X"), "start x", 0.5),
             f0rNumber(g, "6", T("Start Y"), "start y", 0.0), f0rNumber(g, "7", T("Ende X"), "end x", 0.5),
             f0rNumber(g, "8", T("Ende Y"), "end y", 1.0),
             f0rNumber(g, "9", T("Versatz"), "offset", 0.0, T("Wo auf der Linie die erste Farbe beginnt")), blend});
    }
    {
        const char* a = "frei0r.alpha0ps_alpha0ps";
        add(a, T("Alphakanal"), T("Kante der Transparenz verkleinern, vergrößern, weich machen oder anzeigen"),
            {f0rNumber(a, "0", T("Anzeige"), "Display", 0.0,
                       T("0 Bild, 0,21 Alpha als Grau, 0,36 Grau + Rot, 0,5/0,64/0,79/1 Auswahl auf Schwarz/Grau/Weiß/Karos")),
             f0rOther(EffectParam::Bool, "1", T("Eingangs-Alpha zeigen"), "Display input alpha", false),
             f0rNumber(a, "2", T("Operation"), "Operation", 0.0,
                       T("0 nichts, 0,2 rasieren, 0,3/0,4 verkleinern hart/weich, 0,6/0,7 vergrößern hart/weich, "
                         "0,8 Schwelle, 1 weichzeichnen")),
             f0rNumber(a, "3", T("Schwelle"), "Threshold", 0.5),
             f0rNumber(a, "4", T("Stärke"), "Shrink/Grow/Blur amount", 0.333),
             f0rOther(EffectParam::Bool, "5", T("Umkehren"), "Invert", false)});
    }
    return out;
}

QVector<EffectDescriptor>& registry()
{
    static QVector<EffectDescriptor> effects = withWindows({
        // Own keyer (engine/VideoEffects): distance in the chroma plane, soft edge, spill suppression; real
        // transparency. Fixed stage after the grade: it keys the camera picture, the effects then work on the keyed
        // picture. The service and the properties "0"/"1" are those of frei0r.bluescreen0r it replaced (presets).
        {"chromakey", T("Green Screen"), "frei0r.bluescreen0r", true,
         {
             {"color", T("Farbe"), "0", EffectParam::Color, QColor(0, 255, 0), 0, 0},
             {"distance", T("Toleranz"), "1", EffectParam::Double, 0.3, 0.0, 1.0, AnimParam::Count, 0.002, 3, {},
              T("Wie weit eine Farbe von der Key-Farbe entfernt sein darf und noch durchsichtig wird")},
             {"softness", T("Weiche Kante"), "softness", EffectParam::Double, 0.2, 0.0, 1.0, AnimParam::Count, 0.002, 3,
              {}, T("Breite des Übergangs zwischen durchsichtig und sichtbar")},
             {"spill", T("Farbsaum entfernen"), "spill", EffectParam::Double, 1.0, 0.0, 1.0, AnimParam::Count, 0.002, 3,
              {}, T("Nimmt den Schimmer der Key-Farbe (grüner Rand, Widerschein) aus dem, was sichtbar bleibt")},
         },
         false, {}, {}, true},
        // Simple color correction (all values -100..+100, 0 = unchanged). Engine: avfilter.colorchannelmixer
        // (temperature/tint as white balance, saturation) + avfilter.colorlevels (brightness, contrast)
        {"color", T("Farbkorrektur"), {}, true,
         {
             {"brightness", T("Helligkeit"), {}, EffectParam::Double, 0.0, -100, 100, AnimParam::FxBrightness, 0.5, 2},
             {"contrast", T("Kontrast"), {}, EffectParam::Double, 0.0, -100, 100, AnimParam::FxContrast, 0.5, 2},
             {"saturation", T("Sättigung"), {}, EffectParam::Double, 0.0, -100, 100, AnimParam::FxSaturation, 0.5, 2},
             {"temperature", T("Temperatur"), {}, EffectParam::Double, 0.0, -100, 100, AnimParam::FxTemp, 0.5, 2},
             {"tint", T("Tönung"), {}, EffectParam::Double, 0.0, -100, 100, AnimParam::FxTint, 0.5, 2},
         },
         true},
        // Strength relative to the picture height -> looks the same in every preview/export size. Engine: avfilter.gblur
        {"blur", T("Gaußsche Unschärfe"), {}, true,
         {
             {"strength", T("Stärke"), {}, EffectParam::Double, 25.0, 0, 100, AnimParam::FxBlur, 0.25, 2},
         },
         true},
        // Stabilization (DaVinci: Inspector → Stabilization). Engine: MLT vid.stab (engine/Stabilizer) – needs an
        // analysis pass that runs in the background; until it is done the clip plays unstabilized. Smoothing, zoom and
        // border apply live, shakiness/accuracy start a new analysis.
        {"stabilize", T("Stabilisieren"), {}, true,
         {
             {"smoothing", T("Glättung"), {}, EffectParam::Double, 15.0, 0, 100, AnimParam::Count, 0.25, 0, {},
              T("Wie ruhig die Kamera wirkt: Anzahl Frames, über die die Bewegung geglättet wird")},
             {"zoom", T("Zoom"), {}, EffectParam::Double, 0.0, -50, 100, AnimParam::Count, 0.25, 0, {},
              T("Zusätzlicher Zoom in Prozent (positiv = Rand weiter aus dem Bild)")},
             {"border", T("Rand"), {}, EffectParam::Choice, QString("auto"), 0, 0, AnimParam::Count, 1, 2,
              {"auto", "adaptive", "black", "keep"},
              T("Was an den Bildrändern passiert, die das Verschieben freilegt"), {},
              {T("Automatisch zoomen"), T("Mitlaufend zoomen"), T("Schwarz"), T("Vorheriges Bild")}},
             {"shakiness", T("Wackeln"), {}, EffectParam::Double, 5.0, 1, 10, AnimParam::Count, 0.05, 0, {},
              T("Wie stark die Aufnahme wackelt (1 = kaum, 10 = sehr). Neue Analyse"), {}, {}, true},
             {"accuracy", T("Genauigkeit"), {}, EffectParam::Double, 9.0, 1, 15, AnimParam::Count, 0.05, 0, {},
              T("Genauigkeit der Bewegungserkennung (höher = langsamere Analyse). Neue Analyse"), {}, {}, true},
         },
         true, {}, T("Beruhigt verwackelte Aufnahmen. Analysiert den Clip im Hintergrund (Fortschritt im Inspector)"),
         true},
        // Noise reduction for voice recordings (DaVinci: Fairlight FX → Noise Reduction). Engine: own spectral noise
        // suppression (engine/Denoise, real time); defaults work for typical hiss/hum under speech.
        {"denoise", T("Rauschunterdrückung"), {}, false,
         {
             {"reduction", T("Stärke"), {}, EffectParam::Double, 12.0, 0, 40, AnimParam::Count, 0.1, 1, {},
              T("Um wie viel dB das Rauschen abgesenkt wird")},
             {"floor", T("Rauschpegel"), {}, EffectParam::Double, -50.0, -80, -20, AnimParam::Count, 0.1, 1, {},
              T("Pegel des Rauschens in dBFS – gilt, wenn „Rauschen verfolgen“ aus ist")},
             {"track", T("Rauschen verfolgen"), {}, EffectParam::Bool, true, 0, 0, AnimParam::Count, 1, 2, {},
              T("Misst das Rauschen laufend in der Aufnahme (in Sprechpausen) statt den festen Rauschpegel zu nehmen")},
         },
         true, {}, T("Senkt gleichmäßiges Rauschen (Rauschen, Brummen, Lüfter) in Sprachaufnahmen ab"), true},
        // Color page (DaVinci "Primaries - Color Wheels", own area instead of Effects Library): values as in DaVinci
        // (Lift/Gamma 0 = neutral, Gain 1, Offset 25, Contrast 1, Pivot 0.435, Saturation 50), plus LUT (.cube).
        // Engine: own MLT filter (engine/ColorGrade), applied before all other effects.
        {"grade", T("Farbkorrektur (Color)"), {}, true,
         {
             {"liftY", T("Lift"), {}, EffectParam::Double, 0.0, -1, 1, AnimParam::GradeLiftY, 0.002, 2},
             {"liftR", T("Lift") + " R", {}, EffectParam::Double, 0.0, -1, 1, AnimParam::GradeLiftR, 0.002, 2},
             {"liftG", T("Lift") + " G", {}, EffectParam::Double, 0.0, -1, 1, AnimParam::GradeLiftG, 0.002, 2},
             {"liftB", T("Lift") + " B", {}, EffectParam::Double, 0.0, -1, 1, AnimParam::GradeLiftB, 0.002, 2},
             {"gammaY", T("Gamma"), {}, EffectParam::Double, 0.0, -1, 1, AnimParam::GradeGammaY, 0.002, 2},
             {"gammaR", T("Gamma") + " R", {}, EffectParam::Double, 0.0, -1, 1, AnimParam::GradeGammaR, 0.002, 2},
             {"gammaG", T("Gamma") + " G", {}, EffectParam::Double, 0.0, -1, 1, AnimParam::GradeGammaG, 0.002, 2},
             {"gammaB", T("Gamma") + " B", {}, EffectParam::Double, 0.0, -1, 1, AnimParam::GradeGammaB, 0.002, 2},
             {"gainY", T("Gain"), {}, EffectParam::Double, 1.0, 0, 4, AnimParam::GradeGainY, 0.005, 2},
             {"gainR", T("Gain") + " R", {}, EffectParam::Double, 1.0, 0, 4, AnimParam::GradeGainR, 0.005, 2},
             {"gainG", T("Gain") + " G", {}, EffectParam::Double, 1.0, 0, 4, AnimParam::GradeGainG, 0.005, 2},
             {"gainB", T("Gain") + " B", {}, EffectParam::Double, 1.0, 0, 4, AnimParam::GradeGainB, 0.005, 2},
             {"offsetY", T("Offset"), {}, EffectParam::Double, 25.0, 0, 100, AnimParam::GradeOffsetY, 0.1, 2},
             {"offsetR", T("Offset") + " R", {}, EffectParam::Double, 25.0, 0, 100, AnimParam::GradeOffsetR, 0.1, 2},
             {"offsetG", T("Offset") + " G", {}, EffectParam::Double, 25.0, 0, 100, AnimParam::GradeOffsetG, 0.1, 2},
             {"offsetB", T("Offset") + " B", {}, EffectParam::Double, 25.0, 0, 100, AnimParam::GradeOffsetB, 0.1, 2},
             {"contrast", T("Kontrast"), {}, EffectParam::Double, 1.0, 0, 2, AnimParam::GradeContrast, 0.002, 3},
             {"pivot", T("Pivot"), {}, EffectParam::Double, 0.435, 0, 1, AnimParam::GradePivot, 0.001, 3},
             {"saturation", T("Sättigung"), {}, EffectParam::Double, 50.0, 0, 100, AnimParam::GradeSaturation, 0.1, 2},
             {"temperature", T("Temperatur"), {}, EffectParam::Double, 0.0, -4000, 4000, AnimParam::GradeTemp, 5, 1},
             {"tint", T("Tönung"), {}, EffectParam::Double, 0.0, -100, 100, AnimParam::GradeTint, 0.1, 2},
             {"exposure", T("Belichtung"), {}, EffectParam::Double, 0.0, -4, 4, AnimParam::GradeExposure, 0.01, 2},
             {"lut", T("LUT"), {}, EffectParam::Path, QString(), 0, 0},
         },
         false, {}, {}, true},
    }) + ownLooks(); // after the built-in ones: the Effects Library lists them behind blur and color correction
    return effects;
}

} // namespace

const QVector<EffectDescriptor>& all() { return registry(); }

void registerEffects(const QVector<EffectDescriptor>& effects)
{
    for (const EffectDescriptor& e : effects)
        if (!find(e.id)) registry() << e;
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
    if (QString effectId, key; Keys::effectParamOf(p, &effectId, &key)) { // dynamic (frei0r): direct lookup
        const EffectDescriptor* e = find(effectId);
        if (!e) return false; // plugin missing
        for (const auto& x : e->params)
            if (x.key == key && x.anim == p) {
                if (effect) *effect = e;
                if (param) *param = &x;
                return true;
            }
        return false;
    }
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
    for (const auto& p : d->params)
        if (!p.sparse) inst.params[p.key] = p.defaultValue;
    c.effects << inst;
    return true;
}

void remove(Clip& c, const QString& id)
{
    c.effects.removeIf([&](const EffectInstance& e) { return e.effectId == id; });
    if (const EffectDescriptor* d = find(id))
        for (const auto& p : d->params)
            if (p.anim != AnimParam::Count) c.keys.remove(p.anim);
    // Dynamic parameters also when the effect is unknown here (plugin missing)
    QString effectId;
    c.keys.removeIf([&](const auto& it) { return Keys::effectParamOf(it.key(), &effectId, nullptr) && effectId == id; });
}

bool movable(const QString& id)
{
    const EffectDescriptor* d = find(id);
    return !d || !d->fixed;
}

QStringList order(const Clip& c)
{
    QStringList out;
    for (const EffectInstance& e : c.effects)
        if (movable(e.effectId)) out << e.effectId;
    return out;
}

QVector<const EffectInstance*> renderOrder(const Clip& c)
{
    // Place of a fixed stage in the chain (TimelineBuilder applyEffects); the movable effects come after all of them
    static const QStringList stages{"stabilize", "grade", "chromakey"};
    auto rank = [](const EffectInstance& e) {
        if (movable(e.effectId)) return int(stages.size()) + 1;
        const int i = int(stages.indexOf(e.effectId));
        return i >= 0 ? i : int(stages.size());
    };
    QVector<const EffectInstance*> out;
    for (const EffectInstance& e : c.effects) out << &e;
    std::stable_sort(out.begin(), out.end(), [&](const EffectInstance* a, const EffectInstance* b) {
        return rank(*a) < rank(*b);
    });
    return out;
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
