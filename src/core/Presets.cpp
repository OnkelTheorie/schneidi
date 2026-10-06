#include "core/Presets.h"

#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/I18n.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QXmlStreamReader>
#include <algorithm>

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
    return name == "mlt_service" || name == "version" || name == "in" || name == "out" || name == "disable" ||
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

// First value of an MLT animation string ("1=0;10=15", "00:00:00.000=0.5", "1~=0"); plain values stay as they are
QString firstValue(const QString& v, bool* animated)
{
    const qsizetype eq = v.indexOf('=');
    if (eq < 0 || v.startsWith('#')) return v;
    const QString key = v.left(eq);
    static const QString keyChars = QStringLiteral("0123456789:.-~|!$<>()[]{}");
    if (std::any_of(key.begin(), key.end(), [](QChar c) { return !keyChars.contains(c); })) return v;
    const QStringList frames = v.split(';', Qt::SkipEmptyParts);
    if (frames.size() > 1) *animated = true;
    return frames.value(0).mid(frames.value(0).indexOf('=') + 1).trimmed();
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

} // namespace

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

Mapped map(const Preset& preset)
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
        for (const auto& [name, value] : f.props) {
            const EffectParam* p = paramFor(*d, name);
            if (!p) continue;
            const QVariant v = convert(*p, firstValue(value, &m.keyframes), f.factors.value(name, 1.0));
            if (v.isValid()) inst.params[p->key] = v;
        }
        // The same effect twice (Shotcut: several blurs from one plugin): the later one wins, like in the stack
        m.effects.removeIf([&](const EffectInstance& e) { return e.effectId == inst.effectId; });
        m.effects << inst;
    }
    return m;
}

void apply(Clip& c, const Mapped& m)
{
    for (const EffectInstance& e : m.effects) {
        if (EffectInstance* old = EffectRegistry::instance(c, e.effectId)) *old = e;
        else c.effects << e;
    }
    if (!m.lut.isEmpty()) {
        EffectRegistry::add(c, kGrade);
        EffectRegistry::instance(c, kGrade)->params["lut"] = m.lut;
    }
}

} // namespace Presets
