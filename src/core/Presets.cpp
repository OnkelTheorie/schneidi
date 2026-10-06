#include "core/Presets.h"

#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "core/Keyframes.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QXmlStreamWriter>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <algorithm>
#include <cmath>

namespace Presets {
namespace {

constexpr const char* kGrade = "grade";

// Shotcut filters whose QML objectName is not the MLT service (from Shotcut's qml/filters/*/meta*.qml); presets of
// the others lie in a folder named like the service or the service without "frei0r."
struct ShotcutName {
    const char* objectName;
    const char* service;
};
constexpr ShotcutName kShotcutNames[] = {
    {"alphaChannelAdjust", "frei0r.alpha0ps"}, {"alphaChannelView", "frei0r.alpha0ps"},
    {"blur_exponential", "frei0r.IIRblur"},   {"blur_gaussian", "frei0r.IIRblur"},
    {"blur_lowpass", "frei0r.IIRblur"},       {"Frei0rColoradjWidget", "frei0r.coloradj_RGB"},
    {"gradient", "frei0r.cairogradient"},     {"halftone", "frei0r.colorhalftone"},
    {"noise_keyframes", "frei0r.rgbnoise"},   {"nosync", "frei0r.nosync0r"},
    {"scanlines", "frei0r.scanline0r"},       {"lut3d", "avfilter.lut3d"},
    {"blur_gaussian_av", "avfilter.gblur"},   {"contrast", "lift_gamma_gain"},
};

// ---- MLT filters rebuilt with schneidi's own effects (color correction, blur, Color page grade) ----
// A target parameter = f(a, b) of up to two filter properties (alternative names separated by '|', fallback = the
// filter's default when missing). One of them may be animated: its keys become keys of the target parameter.

// Blur radii are pixels; presets come from projects of unknown size -> assumed 1080 lines. schneidi's blur:
// sigma = strength * height / 2000 (TimelineBuilder applyBlur).
double sigmaToStrength(double sigma) { return std::max(0.0, sigma) * 2000.0 / 1080.0; }
// Box blur of radius r (width 2r+1) has the variance r(r+1)/3; `passes` boxes in a row add up
double boxToStrength(double r, double passes = 1)
{
    r = std::max(0.0, r);
    return sigmaToStrength(std::sqrt(std::max(0.0, passes) * r * (r + 1) / 3.0));
}
// MLT lift_gamma_gain works on the picture taken as linear and converted to gamma 2.2: lift l moves black to
// l^2.2, gamma g gives x^(1/g), gain multiplies. schneidi's grade: lift moves black to l, gamma g gives
// x^(2^(-2g)).
double liftOf(double l) { return l >= 0 ? std::pow(l, 2.2) : -std::pow(-l, 2.2); }
double gammaOf(double g) { return g > 0 ? std::log2(g) / 2.0 : -1.0; }

struct Factor {
    const char* names = nullptr; // nullptr = not used (value 1)
    double fallback = 0;
};
struct Conversion {
    const char* service; // MLT service (also Kdenlive's effect id)
    const char* effect;  // schneidi effect id
    const char* param;   // its parameter key
    Factor a, b;
    double (*f)(double a, double b);
};
// The same look with schneidi's effects (TimelineBuilder applyColor: out = 0.5 + (in - 0.5)(1 + contrast/100) +
// brightness/200 per channel, saturation -100 = gray, +100 = double)
const Conversion kConversions[] = {
    // MLT brightness: out = in * level -> contrast and brightness 100(level - 1) give exactly that
    {"brightness", "color", "brightness", {"level", 1}, {}, [](double v, double) { return 100 * (v - 1); }},
    {"brightness", "color", "contrast", {"level", 1}, {}, [](double v, double) { return 100 * (v - 1); }},
    // FFmpeg eq: out = (in - 0.5) * contrast + 0.5 + brightness (luma), saturation 1 = unchanged
    {"avfilter.eq", "color", "brightness", {"av.brightness", 0}, {}, [](double v, double) { return 200 * v; }},
    {"avfilter.eq", "color", "contrast", {"av.contrast", 1}, {}, [](double v, double) { return 100 * (v - 1); }},
    {"avfilter.eq", "color", "saturation", {"av.saturation", 1}, {}, [](double v, double) { return 100 * (v - 1); }},
    {"avfilter.hue", "color", "saturation", {"av.s", 1}, {}, [](double v, double) { return 100 * (v - 1); }},
    {"greyscale", "color", "saturation", {}, {}, [](double, double) { return -100.0; }},
    {"avfilter.gblur", "blur", "strength", {"av.sigma", 0}, {}, [](double v, double) { return sigmaToStrength(v); }},
    {"avfilter.avgblur", "blur", "strength", {"av.sizeX", 1}, {}, [](double v, double) { return boxToStrength(v); }},
    {"avfilter.boxblur", "blur", "strength", {"av.luma_radius|av.lr", 2}, {"av.luma_power|av.lp", 2},
     [](double r, double passes) { return boxToStrength(r, passes); }},
    // MLT boxblur: radius = blur (or start) * hori
    {"boxblur", "blur", "strength", {"blur|start", 2}, {"hori", 1}, [](double v, double h) { return boxToStrength(v * h); }},
    {"box_blur", "blur", "strength", {"hradius", 1}, {}, [](double v, double) { return boxToStrength(v); }},
    {"lift_gamma_gain", "grade", "liftR", {"lift_r", 0}, {}, [](double v, double) { return liftOf(v); }},
    {"lift_gamma_gain", "grade", "liftG", {"lift_g", 0}, {}, [](double v, double) { return liftOf(v); }},
    {"lift_gamma_gain", "grade", "liftB", {"lift_b", 0}, {}, [](double v, double) { return liftOf(v); }},
    {"lift_gamma_gain", "grade", "gammaR", {"gamma_r", 1}, {}, [](double v, double) { return gammaOf(v); }},
    {"lift_gamma_gain", "grade", "gammaG", {"gamma_g", 1}, {}, [](double v, double) { return gammaOf(v); }},
    {"lift_gamma_gain", "grade", "gammaB", {"gamma_b", 1}, {}, [](double v, double) { return gammaOf(v); }},
    {"lift_gamma_gain", "grade", "gainR", {"gain_r", 1}, {}, [](double v, double) { return v; }},
    {"lift_gamma_gain", "grade", "gainG", {"gain_g", 1}, {}, [](double v, double) { return v; }},
    {"lift_gamma_gain", "grade", "gainB", {"gain_b", 1}, {}, [](double v, double) { return v; }},
};

bool converted(const QString& service)
{
    return std::any_of(std::begin(kConversions), std::end(kConversions),
                       [&](const Conversion& c) { return service == QLatin1String(c.service); });
}

// frei0r plugins whose Kdenlive effect file (/usr/share/kdenlive/effects/frei0r_*.xml, Kdenlive 24.12) names the
// parameters by the plugin's parameter name ("Blur"). Kdenlive sets unknown names on the filter as well, and MLT
// prefers the index -> an own preset must use exactly Kdenlive's name, else its slider would have no effect. All
// other frei0r plugins (no Kdenlive file: generated from the MLT metadata, or files with "0", "1", …) use the index.
constexpr const char* kKdenliveNamed[] = {
    "alpha0ps", "alpha0ps_alpha0ps", "alpha0ps_alphagrad", "alpha0ps_alphaspot", "alphagrad", "alphaspot",
    "balanc0r", "bgsubtract0r", "bigsh0t_eq_mask", "bigsh0t_eq_to_rect", "bigsh0t_hemi_to_eq", "bigsh0t_rect_to_eq",
    "bigsh0t_stabilize_360", "bigsh0t_transform_360", "brightness", "cairoimagegrid", "cartoon", "cluster", "colgate",
    "coloradj_RGB", "colordistance", "colorize", "colortap", "contrast0r", "defish0r", "delay0r", "distort0r",
    "dither", "edgeglow", "emboss", "facebl0r", "facedetect", "flippo", "glow", "hqdn3d", "hueshift0r", "IIRblur",
    "kaleid0sc0pe", "keyspillm0pup", "lenscorrection", "letterb0xed", "levels", "lightgraffiti", "mask0mate",
    "medians", "nosync0r", "pixeliz0r", "pixs0r", "pr0be", "pr0file", "primaries", "rgbnoise", "saturat0r",
    "scale0tilt", "select0r", "sharpness", "sigmoidaltransfer", "softglow", "sopsat", "squareblur",
    "three_point_balance", "threshold0r", "timeout", "tint0r", "vertigo", "vignette",
};

// frei0r plugins Kdenlive hides (data/excluded_effects.txt, 24.12): an <effect> of them would make Kdenlive refuse
// the whole effect group -> written like schneidi's own effects
constexpr const char* kKdenliveExcluded[] = {
    "3dflippo", "baltan", "bgsubtract0r", "bigsh0t_zenith_correction", "colorhalftone", "delay0r", "delaygrab",
    "lightgraffiti", "perspective", "tehroxx0r", "tehRoxx0r", "water",
};

// Old frei0r parameter names (MLT frei0r/param_name_map.yaml; Kdenlive's files still use them) -> index.
// kdenlive = Kdenlive's current file uses this name (written into own presets).
struct ParamAlias {
    const char* plugin; // without "frei0r."
    const char* name;
    const char* index;
    bool kdenlive;
};
constexpr ParamAlias kParamAliases[] = {
    {"lenscorrection", "xcenter", "0", true},
    {"lenscorrection", "ycenter", "1", true},
    {"lenscorrection", "correctionnearcenter", "2", true},
    {"lenscorrection", "correctionnearedges", "3", true},
    {"lenscorrection", "brightness", "4", true},
    {"pixeliz0r", "BlockSizeX", "0", true},
    {"pixeliz0r", "BlockSizeY", "1", true},
    {"alpha0ps", "Shrink/grow amount", "4", false}, // older Kdenlive file of alpha0ps
};

QString frei0rPlugin(const QString& service)
{
    return service.startsWith(QLatin1String("frei0r.")) ? service.mid(7) : QString();
}

// Parameter name Kdenlive uses for this parameter (see kKdenliveNamed)
QString kdenliveName(const EffectDescriptor& d, const EffectParam& p)
{
    const QString index = p.mltProperty.isEmpty() ? p.key : p.mltProperty;
    const QString plugin = frei0rPlugin(d.mltService);
    if (plugin.isEmpty() || p.mltName.isEmpty()) return index;
    for (const ParamAlias& a : kParamAliases)
        if (a.kdenlive && plugin == QLatin1String(a.plugin) && index == QLatin1String(a.index))
            return QString::fromLatin1(a.name);
    for (const char* named : kKdenliveNamed)
        if (plugin == QLatin1String(named)) return p.mltName;
    return index;
}

// An effect Kdenlive has under the same id (MLT filter it does not hide)
bool kdenliveKnows(const EffectDescriptor& d)
{
    if (d.mltService.isEmpty()) return false;
    const QString plugin = frei0rPlugin(d.mltService);
    for (const char* excluded : kKdenliveExcluded)
        if (plugin == QLatin1String(excluded)) return false;
    return true;
}

bool isLut3d(const QString& service) { return service == "avfilter.lut3d" || service == "lut3d"; }

// Properties that only describe the filter, not its look
bool ignored(const QString& name)
{
    return name == "mlt_service" || name == "version" || name == "in" || name == "out" ||
           name.startsWith("shotcut:") || name.startsWith("kdenlive:") || name.startsWith("kdenlive_");
}

void addProp(Filter* f, const QString& name, const QString& value)
{
    if (name == "mlt_service") {
        if (f->service.isEmpty()) f->service = value.trimmed();
        return;
    }
    if (!name.isEmpty() && !ignored(name)) f->props.append({name, value.trimmed()});
}

// Element of schneidi's own effects in an own preset (Kdenlive does not know them and would refuse the whole file
// for an unknown <effect id>; other elements it ignores)
constexpr const char* kOwnEffect = "schneidi-effect";

// Kdenlive <effect>: service from tag / mlt_service property / kdenlive_id / id; children <parameter>, <property>.
// <parameter value>/<default> are MLT values (Kdenlive 19.04+: factor only scales the slider). Only custom effects
// from before 19.04 (attribute kdenlive_info) stored shown values = MLT value * factor.
Filter readKdenliveEffect(QXmlStreamReader& x, QString* name, QString* description, QVector<Filter>* own = nullptr)
{
    Filter f;
    const auto a = x.attributes();
    const bool legacy = a.hasAttribute("kdenlive_info");
    f.service = a.value("tag").toString();
    QString fallback = a.value("kdenlive_id").toString();
    if (fallback.isEmpty()) fallback = a.value("id").toString();
    while (x.readNextStartElement()) {
        if (x.name() == u"property") {
            const QString n = x.attributes().value("name").toString();
            addProp(&f, n, x.readElementText());
        } else if (x.name() == u"parameter") {
            const auto pa = x.attributes();
            const QString n = pa.value("name").toString();
            const QString v = pa.hasAttribute("value") ? pa.value("value").toString() : pa.value("default").toString();
            if (const double factor = pa.value("factor").toDouble(); legacy && factor > 0 && factor != 1)
                f.factors[n] = factor;
            addProp(&f, n, v);
            x.skipCurrentElement();
        } else if (x.name() == u"name" && name) {
            *name = x.readElementText().trimmed();
        } else if (x.name() == u"description" && description) {
            *description = x.readElementText().trimmed();
        } else if (x.name() == QLatin1String(kOwnEffect) && own) {
            *own << readKdenliveEffect(x, nullptr, nullptr);
        } else {
            x.skipCurrentElement();
        }
    }
    if (f.service.isEmpty()) f.service = fallback;
    return f;
}

// Shotcut filter set: <mlt><producer><filter><property name=…>…
void readMltFilters(QXmlStreamReader& x, Preset* p)
{
    while (!x.atEnd()) {
        x.readNext();
        if (!x.isStartElement() || x.name() != u"filter") continue;
        Filter f;
        while (x.readNextStartElement()) {
            if (x.name() == u"property") {
                const QString n = x.attributes().value("name").toString();
                addProp(&f, n, x.readElementText());
            } else {
                x.skipCurrentElement();
            }
        }
        p->filters << f;
    }
}

Preset parseXml(const QByteArray& data, Preset p)
{
    QXmlStreamReader x(data);
    if (!x.readNextStartElement()) {
        p.error = x.errorString();
        return p;
    }
    if (x.name() == u"effect") {
        p.source = QStringLiteral("Kdenlive");
        QString name;
        QVector<Filter> own;
        p.filters << readKdenliveEffect(x, &name, &p.description, &own);
        p.filters << own;
        if (!name.isEmpty()) p.name = name;
    } else if (x.name() == u"effectgroup") {
        p.source = QStringLiteral("Kdenlive");
        if (const QString id = x.attributes().value("id").toString(); !id.isEmpty()) p.name = id;
        p.parentIn = std::max(0, x.attributes().value("parentIn").toInt());
        while (x.readNextStartElement()) {
            if (x.name() == u"effect" || x.name() == QLatin1String(kOwnEffect))
                p.filters << readKdenliveEffect(x, nullptr, nullptr);
            else if (x.name() == u"description") p.description = x.readElementText().trimmed();
            else x.skipCurrentElement();
        }
    } else if (x.name() == u"mlt") {
        p.source = QStringLiteral("Shotcut");
        readMltFilters(x, &p);
    } else {
        p.error = T("Unbekanntes Format (<%1>)").arg(x.name().toString());
        return p;
    }
    if (x.hasError()) p.error = x.errorString();
    else if (p.filters.isEmpty()) p.error = T("Enthält keine Filter");
    return p;
}

// One key time of an MLT animation: frames ("10", "-1" = last frame), clock ("hh:mm:ss.zzz", "mm:ss.zzz") or
// SMPTE ("hh:mm:ss:ff")
bool parseTime(const QString& text, double fps, int* frame)
{
    if (text.isEmpty()) return false;
    bool ok = false;
    if (!text.contains(':')) {
        *frame = text.toInt(&ok);
        return ok;
    }
    const bool negative = text.startsWith('-');
    const QStringList parts = (negative ? text.mid(1) : text).split(':');
    if (parts.size() < 2 || parts.size() > 4) return false;
    QVector<double> n;
    for (const QString& part : parts) {
        n << part.toDouble(&ok); // QString::toDouble: always '.', independent of LC_NUMERIC
        if (!ok || n.last() < 0) return false;
    }
    double f = 0;
    if (parts.size() == 4) { // SMPTE: frames in the last field
        f = ((n[0] * 60 + n[1]) * 60 + n[2]) * std::round(fps) + n[3];
    } else {
        double seconds = 0;
        for (double x : n) seconds = seconds * 60 + x;
        f = seconds * fps;
    }
    *frame = int(std::lround(negative ? -f : f));
    return true;
}

// Interpolation of a segment from the MLT keyframe type character (mlt_keyframe_type, MLT 7.22+ letters for eases)
enum class Segment { Linear, Discrete, Smooth, SlowStart, SlowEnd, SlowBoth };
Segment segmentOf(QChar type)
{
    if (type.isNull()) return Segment::Linear;
    if (type == '|' || type == '!') return Segment::Discrete;
    if (type == '~' || type == '$') return Segment::Smooth;
    if (type == '-') return Segment::SlowBoth; // smooth tight: slope 0 at the keys
    int index = -1;
    if (type >= 'a' && type <= 'z') index = type.unicode() - 'a';
    else if (type >= 'A' && type <= 'D') index = 26 + type.unicode() - 'A';
    if (index < 0) return Segment::Linear;
    switch (index % 3) { // sinusoidal_in, _out, _in_out, quadratic_in, …
    case 0: return Segment::SlowStart;
    case 1: return Segment::SlowEnd;
    default: return Segment::SlowBoth;
    }
}

void slowStart(Keyframe& k)
{
    if (k.ease == KeyEase::Linear) k.ease = KeyEase::EaseOut;
    else if (k.ease == KeyEase::EaseIn) k.ease = KeyEase::EaseInOut;
}

void slowEnd(Keyframe& k)
{
    if (k.ease == KeyEase::Linear) k.ease = KeyEase::EaseIn;
    else if (k.ease == KeyEase::EaseOut) k.ease = KeyEase::EaseInOut;
}

// MLT colors: "#rrggbb", "#aarrggbb", "0xrrggbbaa", decimal 0xrrggbbaa, color names
QColor parseColor(const QString& s)
{
    bool ok = false;
    quint32 v = 0;
    if (s.startsWith("0x", Qt::CaseInsensitive)) v = s.mid(2).toUInt(&ok, 16);
    else if (!s.isEmpty() && s[0].isDigit()) v = s.toUInt(&ok, 10);
    if (ok) return QColor(int(v >> 24), int((v >> 16) & 0xff), int((v >> 8) & 0xff), int(v & 0xff));
    return QColor(s);
}

const EffectDescriptor* descriptorFor(const QString& service)
{
    if (service.isEmpty()) return nullptr;
    if (const EffectDescriptor* d = EffectRegistry::find(service)) return d;
    for (const EffectDescriptor& d : EffectRegistry::all())
        if (d.mltService == service) return &d;
    return nullptr;
}

// Kdenlive names frei0r parameters by their name ("Gamma", some by old names), Shotcut and MLT by index ("3")
const EffectParam* paramFor(const EffectDescriptor& d, const QString& name)
{
    for (const EffectParam& p : d.params)
        if (p.mltProperty == name || p.key == name) return &p;
    for (const EffectParam& p : d.params)
        if (!p.mltName.isEmpty() && p.mltName == name) return &p;
    const QString plugin = frei0rPlugin(d.mltService);
    for (const ParamAlias& a : kParamAliases)
        if (plugin == QLatin1String(a.plugin) && name == QLatin1String(a.name))
            for (const EffectParam& p : d.params)
                if (p.mltProperty == QLatin1String(a.index)) return &p;
    for (const EffectParam& p : d.params)
        if (p.label.compare(name, Qt::CaseInsensitive) == 0 || p.mltName.compare(name, Qt::CaseInsensitive) == 0)
            return &p;
    return nullptr;
}

// Value in the registry's type, invalid = not usable
QVariant convert(const EffectParam& p, const QString& v, double factor)
{
    switch (p.type) {
    case EffectParam::Double: {
        bool ok = false;
        const double d = v.toDouble(&ok);
        if (!ok) return {};
        return std::clamp(d / factor, p.min, p.max);
    }
    case EffectParam::Bool: {
        if (v.compare("true", Qt::CaseInsensitive) == 0) return true;
        bool ok = false;
        const double d = v.toDouble(&ok);
        return ok ? QVariant(d > 0.5) : QVariant();
    }
    case EffectParam::Color: {
        const QColor c = parseColor(v);
        return c.isValid() ? QVariant(c) : QVariant();
    }
    case EffectParam::Choice:
        for (const QString& c : p.choices)
            if (c.compare(v, Qt::CaseInsensitive) == 0) return c;
        return {};
    case EffectParam::Path: return {};
    }
    return {};
}

QString lutFile(const Filter& f)
{
    for (const auto& [name, value] : f.props) {
        if (name != "av.file" && name != "resource" && name != "filter.av.file") continue;
        if (QFileInfo::exists(value)) return value;
        // Preset from another computer: same file name in the own LUTs folder
        return EffectFolders::findUserLut(QFileInfo(QString(value).replace('\\', '/')).fileName());
    }
    return {};
}

QString number(double v) { return QString::number(v, 'g', 12); } // always '.', like MLT XML with LC_NUMERIC=C

// Parameter value as MLT property text; empty = not saved
QString valueText(const EffectParam& p, const QVariant& v)
{
    switch (p.type) {
    case EffectParam::Double: return number(v.toDouble());
    case EffectParam::Bool: return v.toBool() ? QStringLiteral("1") : QStringLiteral("0");
    case EffectParam::Color: {
        const QColor c = v.value<QColor>();
        return QString::asprintf("0x%02x%02x%02x%02x", c.red(), c.green(), c.blue(), c.alpha()); // MLT 0xrrggbbaa
    }
    case EffectParam::Choice: return v.toString();
    case EffectParam::Path: return {};
    }
    return {};
}

// MLT keyframe type of the segment a -> b (inverse of segmentOf/toKeyTrack)
QChar segmentChar(const Keyframe& a, const Keyframe& b)
{
    if (a.ease == KeyEase::Bezier || b.ease == KeyEase::Bezier) return '~';
    const bool start = a.ease == KeyEase::EaseOut || a.ease == KeyEase::EaseInOut;
    const bool end = b.ease == KeyEase::EaseIn || b.ease == KeyEase::EaseInOut;
    if (start && end) return 'c'; // sinusoidal in/out
    if (start) return 'a';        // sinusoidal in
    if (end) return 'b';          // sinusoidal out
    return {};
}

// Keyframes as MLT animation string, frames from the clip start. Keys outside the clip (after trimming) would be
// negative (= from the end in MLT) -> replaced by keys with the value at the clip edge.
QString animationText(const Clip& c, AnimParam p)
{
    const KeyTrack& k = c.keys.value(p);
    const int len = c.length();
    KeyTrack inside;
    for (Keyframe x : k) {
        x.frame -= c.in;
        if (x.frame >= 0 && x.frame < len) inside << x;
    }
    if (k.first().frame - c.in < 0 && (inside.isEmpty() || inside.first().frame != 0))
        inside.prepend(Keyframe{0, Keys::valueAt(c, p, 0), KeyEase::Linear});
    if (k.last().frame - c.in > len - 1 && inside.last().frame != len - 1)
        inside << Keyframe{len - 1, Keys::valueAt(c, p, len - 1), KeyEase::Linear};
    QStringList items;
    for (int i = 0; i < inside.size(); ++i) {
        QString t = QString::number(inside[i].frame);
        if (i + 1 < inside.size())
            if (const QChar type = segmentChar(inside[i], inside[i + 1]); !type.isNull()) t += type;
        items << t + "=" + number(inside[i].value);
    }
    return items.join(';');
}

QString safeFileName(QString name)
{
    static const QRegularExpression bad(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]"));
    name.replace(bad, QStringLiteral("_"));
    name = name.trimmed();
    while (name.startsWith('.')) name.remove(0, 1); // no hidden files
    return name.isEmpty() ? QStringLiteral("Preset") : name;
}


// Property of a filter by one of its names ('|'), empty = missing
QString propValue(const Filter& f, const char* names)
{
    if (!names) return {};
    const QStringList list = QString::fromLatin1(names).split('|');
    for (const auto& [name, value] : f.props)
        if (list.contains(name)) return value;
    return {};
}

// One factor of a conversion: static value and keys (frame -> value, empty = not animated)
struct FactorValue {
    double value = 1;
    QVector<RawKey> keys;
};
FactorValue factorOf(const Filter& f, const Factor& factor, double fps)
{
    FactorValue out;
    if (!factor.names) return out;
    out.value = factor.fallback;
    const QString text = propValue(f, factor.names);
    if (text.isEmpty()) return out;
    bool ok = false;
    if (parseAnimation(text, fps, &out.keys)) {
        out.value = out.keys.first().value.toDouble(&ok);
        if (!ok) out.value = factor.fallback;
        if (out.keys.size() < 2) out.keys.clear();
    } else if (const double v = text.toDouble(&ok); ok) {
        out.value = v;
    }
    return out;
}

void convertFilter(const Filter& f, int parentIn, double fps, Mapped* m)
{
    bool enabled = true;
    for (const auto& [name, value] : f.props)
        if (name == "disable") enabled = value.trimmed().isEmpty() || value.trimmed() == "0";
    for (const Conversion& c : kConversions) {
        if (f.service != QLatin1String(c.service)) continue;
        const EffectDescriptor* d = EffectRegistry::find(c.effect);
        if (!d) continue;
        const EffectParam* p = nullptr;
        for (const EffectParam& x : d->params)
            if (x.key == QLatin1String(c.param)) p = &x;
        if (!p) continue;
        EffectInstance* inst = nullptr;
        for (EffectInstance& e : m->effects)
            if (e.effectId == d->id) inst = &e;
        if (!inst) { // several filters may build one effect (brightness + saturation): they add up
            EffectInstance e;
            e.effectId = d->id;
            for (const EffectParam& x : d->params) e.params[x.key] = x.defaultValue;
            m->effects << e;
            inst = &m->effects.last();
        }
        inst->enabled = enabled;
        m->touched[d->id] << p->key;
        const FactorValue a = factorOf(f, c.a, fps), b = factorOf(f, c.b, fps);
        auto value = [&](double x, double y) { return std::clamp(c.f(x, y), p->min, p->max); };
        inst->params[p->key] = value(a.value, b.value);
        if (p->anim == AnimParam::Count) continue;
        m->keys.remove(p->anim);
        const bool first = !a.keys.isEmpty();
        const QVector<RawKey>& raw = first ? a.keys : b.keys;
        if (raw.isEmpty()) continue;
        QVector<AnimKey> list;
        for (const RawKey& r : raw) {
            bool ok = false;
            const double v = r.value.toDouble(&ok);
            if (!ok) continue;
            const int frame = r.frame >= 0 ? std::max(0, r.frame - parentIn) : r.frame;
            list << AnimKey{frame, first ? value(v, b.value) : value(a.value, v), r.type};
        }
        if (list.size() >= 2) m->keys.insert(p->anim, list);
    }
}

} // namespace

QByteArray toXml(const Clip& c, const QString& name, QStringList* skipped)
{
    // Kdenlive reads an <effectgroup> only with at least two <effect>s (each id an effect it knows), a single effect
    // only as a custom effect (<effect tag=…> with its <parameter> definitions). schneidi's own effects go into
    // <schneidi-effect> elements, which Kdenlive ignores.
    QVector<QPair<const EffectInstance*, const EffectDescriptor*>> mlt, own;
    for (const EffectInstance& e : c.effects) {
        const EffectDescriptor* d = EffectRegistry::find(e.effectId);
        if (!d || e.effectId == kGrade || !d->video) {
            if (skipped) *skipped << (d ? d->name : e.effectId);
            continue;
        }
        (kdenliveKnows(*d) ? mlt : own) << qMakePair(&e, d);
    }
    const bool single = mlt.size() == 1;

    QByteArray out;
    QXmlStreamWriter x(&out);
    x.setAutoFormatting(true);
    x.setAutoFormattingIndent(4);
    x.writeStartDocument();
    auto prop = [&](const QString& n, const QString& v) {
        x.writeStartElement("property");
        x.writeAttribute("name", n);
        x.writeCharacters(v);
        x.writeEndElement();
    };
    // Value of a parameter as MLT text (animation string when animated); empty + false = not saved
    auto text = [&](const EffectInstance& e, const EffectParam& p, bool* animated) {
        *animated = p.anim != AnimParam::Count && Keys::animated(c, p.anim);
        if (*animated) return animationText(c, p.anim);
        return valueText(p, e.params.value(p.key, p.defaultValue));
    };
    auto properties = [&](const EffectInstance& e, const EffectDescriptor& d, bool own) {
        if (!e.enabled) prop("disable", "1");
        for (const EffectParam& p : d.params) {
            bool animated = false;
            const QString v = text(e, p, &animated);
            if (!v.isEmpty() || p.type == EffectParam::Choice) prop(own ? p.key : kdenliveName(d, p), v);
        }
    };
    auto ownEffects = [&] {
        for (const auto& [e, d] : own) {
            x.writeStartElement(kOwnEffect);
            x.writeAttribute("id", d->id);
            properties(*e, *d, true);
            x.writeEndElement();
        }
    };

    if (single) {
        // Kdenlive custom effect (like its "Save effect"): the effect's definition with the values in value=
        const auto [e, d] = mlt.first();
        x.writeStartElement("effect");
        x.writeAttribute("tag", d->mltService);
        x.writeAttribute("id", name);
        x.writeAttribute("type", "customVideo");
        x.writeTextElement("name", name);
        for (const EffectParam& p : d->params) {
            bool animated = false;
            const QString v = text(*e, p, &animated);
            if (v.isEmpty() && p.type != EffectParam::Choice) continue;
            x.writeStartElement("parameter");
            switch (p.type) {
            case EffectParam::Double: x.writeAttribute("type", p.anim != AnimParam::Count ? "animated" : "constant"); break;
            case EffectParam::Bool: x.writeAttribute("type", "bool"); break;
            case EffectParam::Color: x.writeAttribute("type", "color"); break;
            case EffectParam::Choice: x.writeAttribute("type", "list"); break;
            case EffectParam::Path: break;
            }
            x.writeAttribute("name", kdenliveName(*d, p));
            x.writeAttribute("default", valueText(p, p.defaultValue));
            if (p.type == EffectParam::Double) {
                x.writeAttribute("min", number(p.min));
                x.writeAttribute("max", number(p.max));
                x.writeAttribute("decimals", QString::number(std::max(p.decimals, 3)));
            }
            if (p.type == EffectParam::Choice) x.writeAttribute("paramlist", p.choices.join(';'));
            x.writeAttribute("value", v);
            x.writeTextElement("name", p.label);
            x.writeEndElement();
        }
        if (!e->enabled) prop("disable", "1"); // not part of a Kdenlive definition: only schneidi reads it
        ownEffects();
        x.writeEndElement();
    } else {
        x.writeStartElement("effectgroup");
        x.writeAttribute("id", name);
        x.writeAttribute("parentIn", "0"); // keyframes count from the clip start
        for (const EffectInstance& e : c.effects) { // clip order, own effects in between
            const EffectDescriptor* d = EffectRegistry::find(e.effectId);
            if (!d || e.effectId == kGrade || !d->video) continue;
            const bool isOwn = !kdenliveKnows(*d);
            x.writeStartElement(isOwn ? QString::fromLatin1(kOwnEffect) : QStringLiteral("effect"));
            x.writeAttribute("id", isOwn ? d->id : d->mltService);
            properties(e, *d, isOwn);
            x.writeEndElement();
        }
        x.writeEndElement();
    }
    x.writeEndDocument();
    return out;
}

QString userPresetPath(const QString& name)
{
    return QDir(EffectFolders::presetDir()).filePath(safeFileName(name) + ".xml");
}

bool save(const Clip& c, const QString& name, const QString& path, QString* error, QStringList* skipped)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(toXml(c, name, skipped));
    if (!f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

bool parseAnimation(const QString& v, double fps, QVector<RawKey>* keys)
{
    keys->clear();
    if (v.startsWith('#') || !v.contains('=')) return false;
    static const QRegularExpression keyRe(QStringLiteral("^(-?[0-9][0-9:.]*)([|!~$a-zA-D-]?)$"));
    for (const QString& item : v.split(';', Qt::SkipEmptyParts)) {
        const qsizetype eq = item.indexOf('=');
        if (eq <= 0) return false;
        const QRegularExpressionMatch match = keyRe.match(item.left(eq).trimmed());
        RawKey k;
        if (!match.hasMatch() || !parseTime(match.captured(1), fps, &k.frame)) return false;
        if (const QString type = match.captured(2); !type.isEmpty()) k.type = type[0];
        k.value = item.mid(eq + 1).trimmed();
        *keys << k;
    }
    return !keys->isEmpty();
}

KeyTrack toKeyTrack(QVector<AnimKey> keys, int length)
{
    for (AnimKey& k : keys)
        if (k.frame < 0) k.frame += length;
    std::stable_sort(keys.begin(), keys.end(), [](const AnimKey& a, const AnimKey& b) { return a.frame < b.frame; });
    for (int i = int(keys.size()) - 2; i >= 0; --i) // the same frame twice: the later key wins
        if (keys[i].frame == keys[i + 1].frame) keys.removeAt(i);
    KeyTrack out;
    QVector<int> at; // index of keys[i] in out (holds insert keys in between)
    for (int i = 0; i < keys.size(); ++i) {
        at << int(out.size());
        out << Keyframe{keys[i].frame, keys[i].value, KeyEase::Linear};
        // discrete: value holds until the frame before the next key
        if (i + 1 < keys.size() && segmentOf(keys[i].type) == Segment::Discrete && keys[i + 1].frame - keys[i].frame > 1)
            out << Keyframe{keys[i + 1].frame - 1, keys[i].value, KeyEase::Linear};
    }
    QVector<int> smooth;
    for (int i = 0; i + 1 < keys.size(); ++i) {
        Keyframe& a = out[at[i]];
        Keyframe& b = out[at[i + 1]];
        switch (segmentOf(keys[i].type)) {
        case Segment::SlowStart: slowStart(a); break;
        case Segment::SlowEnd: slowEnd(b); break;
        case Segment::SlowBoth: slowStart(a); slowEnd(b); break;
        case Segment::Smooth: smooth << at[i] << at[i + 1]; break;
        default: break;
        }
    }
    // smooth (Catmull-Rom in MLT) -> Bezier with soft handles (same slope rule)
    for (int i : smooth) out[i].ease = KeyEase::Bezier;
    for (int i : smooth) Keys::autoHandles(out, i);
    return out;
}

QString shotcutService(const QString& folderName)
{
    for (const ShotcutName& n : kShotcutNames)
        if (folderName == QLatin1String(n.objectName)) return QString::fromLatin1(n.service);
    if (descriptorFor(folderName) || isLut3d(folderName) || converted(folderName)) return folderName;
    if (descriptorFor("frei0r." + folderName)) return "frei0r." + folderName;
    return {};
}

Preset parse(const QByteArray& data, const QString& fileName, const QString& folderName)
{
    Preset p;
    const QFileInfo fi(fileName);
    p.name = fi.suffix().compare("xml", Qt::CaseInsensitive) == 0 ? fi.completeBaseName() : fi.fileName();
    const QByteArray trimmed = data.trimmed();
    if (trimmed.isEmpty()) {
        p.error = T("Leere Datei");
        return p;
    }
    if (trimmed.startsWith('<')) return parseXml(trimmed, p);

    // Shotcut filter preset: "property=value" lines, the filter is the folder name
    p.source = QStringLiteral("Shotcut");
    Filter f;
    f.service = shotcutService(folderName);
    for (const QByteArray& raw : trimmed.split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        const qsizetype eq = line.indexOf('=');
        if (line.isEmpty() || line.startsWith('#')) continue;
        if (eq <= 0) {
            p.error = T("Unbekanntes Format");
            return p;
        }
        addProp(&f, line.left(eq), line.mid(eq + 1));
    }
    if (f.service.isEmpty()) {
        p.error = T("Unbekannter Shotcut-Filter „%1“ (Ordnername)").arg(folderName);
        return p;
    }
    p.filters << f;
    return p;
}

Preset load(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        Preset p;
        p.name = QFileInfo(path).fileName();
        p.error = f.errorString();
        return p;
    }
    return parse(f.read(4 << 20), path, QFileInfo(path).absoluteDir().dirName());
}

Mapped map(const Preset& preset, double fps)
{
    Mapped m;
    for (const Filter& f : preset.filters) {
        if (isLut3d(f.service)) {
            if (const QString lut = lutFile(f); !lut.isEmpty()) m.lut = lut;
            else m.skipped << f.service;
            continue;
        }
        if (converted(f.service)) {
            convertFilter(f, preset.parentIn, fps, &m);
            continue;
        }
        const EffectDescriptor* d = descriptorFor(f.service);
        if (!d || !d->video) {
            m.skipped << (f.service.isEmpty() ? QStringLiteral("?") : f.service);
            continue;
        }
        EffectInstance inst;
        inst.effectId = d->id;
        for (const EffectParam& p : d->params) inst.params[p.key] = p.defaultValue;
        QMap<AnimParam, QVector<AnimKey>> keys;
        for (const auto& [name, value] : f.props) {
            if (name == "disable") { // MLT: filter switched off
                inst.enabled = value.trimmed().isEmpty() || value.trimmed() == "0";
                continue;
            }
            const EffectParam* p = paramFor(*d, name);
            if (!p) continue;
            const double factor = f.factors.value(name, 1.0);
            QVector<RawKey> raw;
            const bool animation = parseAnimation(value, fps, &raw);
            if (const QVariant v = convert(*p, animation ? raw.first().value : value, factor); v.isValid())
                inst.params[p->key] = v; // static value = first key (stays when the keyframes are removed)
            if (!animation || raw.size() < 2) continue;
            if (p->type != EffectParam::Double || p->anim == AnimParam::Count) {
                m.keyframes = true;
                continue;
            }
            QVector<AnimKey> list;
            for (const RawKey& r : raw)
                if (const QVariant v = convert(*p, r.value, factor); v.isValid())
                    list << AnimKey{r.frame >= 0 ? std::max(0, r.frame - preset.parentIn) : r.frame, v.toDouble(), r.type};
            keys.remove(p->anim);
            if (list.size() >= 2) keys.insert(p->anim, list);
        }
        // The same effect twice (Shotcut: several blurs from one plugin): the later one wins, like in the stack
        m.effects.removeIf([&](const EffectInstance& e) { return e.effectId == inst.effectId; });
        m.touched.remove(inst.effectId);
        for (const EffectParam& p : d->params) m.keys.remove(p.anim);
        m.effects << inst;
        m.keys.insert(keys);
    }
    return m;
}

void apply(Clip& c, const Mapped& m)
{
    for (const EffectInstance& e : m.effects) {
        const QSet<QString> touched = m.touched.value(e.effectId);
        EffectInstance* old = EffectRegistry::instance(c, e.effectId);
        if (old && !touched.isEmpty()) { // rebuilt from other filters: only their parameters change
            for (const QString& key : touched) old->params[key] = e.params.value(key);
            old->enabled = e.enabled;
        } else if (old) {
            *old = e;
        } else {
            c.effects << e;
        }
        // the preset's values replace the old animation of this effect
        if (const EffectDescriptor* d = EffectRegistry::find(e.effectId))
            for (const EffectParam& p : d->params)
                if (p.anim != AnimParam::Count && (touched.isEmpty() || touched.contains(p.key))) c.keys.remove(p.anim);
    }
    for (auto it = m.keys.cbegin(); it != m.keys.cend(); ++it) {
        KeyTrack k = toKeyTrack(it.value(), c.length());
        for (Keyframe& x : k) x.frame += c.in; // keys count in source frames
        c.keys.insert(it.key(), k);
    }
    if (!m.lut.isEmpty()) {
        EffectRegistry::add(c, kGrade);
        EffectRegistry::instance(c, kGrade)->params["lut"] = m.lut;
    }
}

} // namespace Presets
