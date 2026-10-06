#include "engine/Frei0r.h"

#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "engine/Bundle.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSet>

#include <framework/mlt.h>

namespace Frei0r {
namespace {

#ifdef Q_OS_WIN
constexpr char kListSep = ';';
#else
constexpr char kListSep = ':';
// MLT's default when FREI0R_PATH is unset (plus Debian's multiarch folders)
const char* const kSystemDirs[] = {"/usr/lib/frei0r-1", "/usr/lib/x86_64-linux-gnu/frei0r-1",
                                   "/usr/lib/aarch64-linux-gnu/frei0r-1", "/usr/lib64/frei0r-1",
                                   "/usr/local/lib/frei0r-1", "/opt/local/lib/frei0r-1"};
#endif

// Not offered: Green Screen has its own section; face detection needs paths to OpenCV models
const QSet<QString> kHidden{"frei0r.bluescreen0r", "frei0r.facebl0r", "frei0r.facedetect"};

// Old names from MLT's aliases.yaml ("  - frei0r.pr0be"): same plugins as the new names, show them only once
QSet<QString> aliases()
{
    QSet<QString> out;
    const char* data = mlt_environment("MLT_DATA");
    if (!data) return out;
    QFile f(QDir(QString::fromLocal8Bit(data)).filePath("frei0r/aliases.yaml"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.startsWith("- ")) out << line.mid(2).trimmed();
    }
    return out;
}

// Upper-case first letter ("triplevel" -> "Triplevel")
QString capitalized(QString s)
{
    s = s.trimmed();
    if (!s.isEmpty()) s[0] = s[0].toUpper();
    return s;
}

// String parameter with a fixed set of values, listed in its description ("Accepted values: 'a', 'b'" or "One of: a, b")
QStringList choicesOf(const QString& description)
{
    QStringList out;
    if (const int i = description.indexOf("Accepted values", 0, Qt::CaseInsensitive); i >= 0) {
        static const QRegularExpression quoted("'([^']+)'");
        for (auto it = quoted.globalMatch(description.mid(i)); it.hasNext();) out << it.next().captured(1);
    } else if (const int j = description.indexOf("One of:", 0, Qt::CaseInsensitive); j >= 0) {
        for (const QString& part : description.mid(j + 7).split(',')) {
            static const QRegularExpression word("^[A-Za-z0-9_-]+$");
            if (const QString w = part.trimmed(); word.match(w).hasMatch()) out << w;
        }
    }
    return out;
}

QString str(mlt_properties p, const char* name) { return QString::fromUtf8(mlt_properties_get(p, name)); }

bool describe(mlt_repository repo, const QString& service, EffectDescriptor* d)
{
    const QByteArray id = service.toUtf8();
    auto md = static_cast<mlt_properties>(mlt_repository_metadata(repo, mlt_service_filter_type, id.constData()));
    if (!md) return false;
    d->id = service;
    d->name = capitalized(str(md, "title"));
    if (d->name.isEmpty()) d->name = service.mid(7);
    d->mltService = service;
    d->library = true;
    d->category = EffectRegistry::Frei0rCategory;
    d->description = str(md, "description");
    auto params = static_cast<mlt_properties>(mlt_properties_get_data(md, "parameters", nullptr));
    for (int i = 0; params && i < mlt_properties_count(params); ++i) {
        auto pm = static_cast<mlt_properties>(mlt_properties_get_data_at(params, i, nullptr));
        if (!pm) continue;
        EffectParam p;
        p.key = p.mltProperty = str(pm, "identifier");
        p.mltName = str(pm, "title");
        p.label = capitalized(p.mltName);
        p.description = str(pm, "description");
        if (p.label.isEmpty()) p.label = p.key;
        const QString type = str(pm, "type");
        const QString def = str(pm, "default");
        if (type == "float") {
            p.type = EffectParam::Double;
            p.min = mlt_properties_get(pm, "minimum") ? mlt_properties_get_double(pm, "minimum") : 0.0;
            p.max = mlt_properties_get(pm, "maximum") ? mlt_properties_get_double(pm, "maximum") : 1.0;
            if (p.max <= p.min) p.max = p.min + 1;
            p.defaultValue = std::clamp(def.isEmpty() ? p.min : mlt_properties_get_double(pm, "default"), p.min, p.max);
            p.step = (p.max - p.min) / 500.0;
            p.decimals = 3;
            if (str(pm, "animation") != "no") // keyframes like the built-in parameters
                p.anim = Keys::effectParam(service, p.key);
        } else if (type == "boolean") {
            p.type = EffectParam::Bool;
            p.defaultValue = def.toDouble() > 0.5;
        } else if (type == "color") {
            p.type = EffectParam::Color;
            const QColor c(def);
            p.defaultValue = c.isValid() ? c : QColor(Qt::black);
        } else if (type == "string") {
            p.choices = choicesOf(p.description);
            if (p.choices.isEmpty()) continue; // free text (paths, curve lists): keep MLT's default
            p.type = EffectParam::Choice;
            p.defaultValue = p.choices.contains(def) ? def : p.choices.first();
        } else {
            continue;
        }
        d->params << p;
    }
    return true;
}

} // namespace

void prepareEnvironment()
{
    const QString own = EffectFolders::frei0rDir();
    QDir().mkpath(own);
    QByteArray list = qgetenv("FREI0R_PATH");
#ifndef Q_OS_WIN
    if (list.isEmpty()) { // system MLT: keep its default folders
        QByteArrayList dirs;
        for (const char* d : kSystemDirs)
            if (QDir(d).exists()) dirs << d;
        dirs << QDir::home().filePath(".frei0r-1/lib").toUtf8();
        list = dirs.join(kListSep);
    }
#else
    if (list.isEmpty()) return; // no bundled folder (development build): leave MLT's default
#endif
    qputenv("FREI0R_PATH", list + kListSep + Bundle::pathForMlt(own));
}

int registerEffects()
{
    mlt_repository repo = mlt_factory_repository();
    if (!repo) return 0;
    const QSet<QString> old = aliases();
    QVector<EffectDescriptor> out;
    mlt_properties filters = mlt_repository_filters(repo);
    for (int i = 0; i < mlt_properties_count(filters); ++i) {
        const QString name = QString::fromUtf8(mlt_properties_get_name(filters, i));
        if (!name.startsWith("frei0r.") || kHidden.contains(name) || old.contains(name)) continue;
        EffectDescriptor d;
        if (describe(repo, name, &d)) out << d;
    }
    std::sort(out.begin(), out.end(), [](const EffectDescriptor& a, const EffectDescriptor& b) {
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });
    EffectRegistry::registerEffects(out);
    return int(out.size());
}

} // namespace Frei0r
