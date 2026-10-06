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
};

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

// Kdenlive <effect>: service from tag / mlt_service property / kdenlive_id / id; children <parameter>, <property>
Filter readKdenliveEffect(QXmlStreamReader& x, QString* name, QString* description)
{
    Filter f;
    const auto a = x.attributes();
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
            if (const double factor = pa.value("factor").toDouble(); factor > 0 && factor != 1) f.factors[n] = factor;
            addProp(&f, n, v);
            x.skipCurrentElement();
        } else if (x.name() == u"name" && name) {
            *name = x.readElementText().trimmed();
        } else if (x.name() == u"description" && description) {
            *description = x.readElementText().trimmed();
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
        p.filters << readKdenliveEffect(x, &name, &p.description);
        if (!name.isEmpty()) p.name = name;
    } else if (x.name() == u"effectgroup") {
        p.source = QStringLiteral("Kdenlive");
        if (const QString id = x.attributes().value("id").toString(); !id.isEmpty()) p.name = id;
        p.parentIn = std::max(0, x.attributes().value("parentIn").toInt());
        while (x.readNextStartElement()) {
            if (x.name() == u"effect") p.filters << readKdenliveEffect(x, nullptr, nullptr);
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

// Kdenlive names frei0r parameters by their title ("Gamma"), Shotcut and MLT by index ("3")
const EffectParam* paramFor(const EffectDescriptor& d, const QString& name)
{
    for (const EffectParam& p : d.params)
        if (p.mltProperty == name || p.key == name) return &p;
    for (const EffectParam& p : d.params)
        if (p.label.compare(name, Qt::CaseInsensitive) == 0) return &p;
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

} // namespace

QByteArray toXml(const Clip& c, const QString& name, QStringList* skipped)
{
    QByteArray out;
    QXmlStreamWriter x(&out);
    x.setAutoFormatting(true);
    x.setAutoFormattingIndent(4);
    x.writeStartDocument();
    x.writeStartElement("effectgroup");
    x.writeAttribute("id", name);
    x.writeAttribute("parentIn", "0"); // keyframes count from the clip start
    for (const EffectInstance& e : c.effects) {
        const EffectDescriptor* d = EffectRegistry::find(e.effectId);
        if (!d || e.effectId == kGrade || !d->video) {
            if (skipped) *skipped << (d ? d->name : e.effectId);
            continue;
        }
        x.writeStartElement("effect");
        x.writeAttribute("id", d->mltService.isEmpty() ? d->id : d->mltService);
        if (!d->mltService.isEmpty()) {
            x.writeStartElement("property");
            x.writeAttribute("name", "mlt_service");
            x.writeCharacters(d->mltService);
            x.writeEndElement();
        }
        auto prop = [&](const QString& n, const QString& v) {
            x.writeStartElement("property");
            x.writeAttribute("name", n);
            x.writeCharacters(v);
            x.writeEndElement();
        };
        if (!e.enabled) prop("disable", "1");
        for (const EffectParam& p : d->params) {
            const QString n = p.mltProperty.isEmpty() ? p.key : p.mltProperty;
            if (p.anim != AnimParam::Count && Keys::animated(c, p.anim)) {
                prop(n, animationText(c, p.anim));
                continue;
            }
            const QString v = valueText(p, e.params.value(p.key, p.defaultValue));
            if (!v.isEmpty() || p.type == EffectParam::Choice) prop(n, v);
        }
        x.writeEndElement();
    }
    x.writeEndElement();
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
    if (descriptorFor(folderName) || isLut3d(folderName)) return folderName;
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
        for (const EffectParam& p : d->params) m.keys.remove(p.anim);
        m.effects << inst;
        m.keys.insert(keys);
    }
    return m;
}

void apply(Clip& c, const Mapped& m)
{
    for (const EffectInstance& e : m.effects) {
        if (EffectInstance* old = EffectRegistry::instance(c, e.effectId)) *old = e;
        else c.effects << e;
        // the preset's values replace the old animation of this effect
        if (const EffectDescriptor* d = EffectRegistry::find(e.effectId))
            for (const EffectParam& p : d->params)
                if (p.anim != AnimParam::Count) c.keys.remove(p.anim);
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
