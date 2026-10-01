#include "core/EffectFolders.h"

#include "core/I18n.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <algorithm>

namespace EffectFolders {

namespace {

constexpr const char* kBuiltinDir = ":/luts/";

// Mitgelieferte Looks (Dateien aus tools/gen_luts.py), Reihenfolge = Anzeige
struct Builtin {
    const char* id;
    const char* name;
};
constexpr Builtin kBuiltins[] = {
    {"teal-orange", N_("Teal & Orange")},
    {"kino", N_("Kino-Kontrast")},
    {"bleach-bypass", N_("Bleach Bypass")},
    {"verblasst", N_("Verblasst")},
    {"retro", N_("Retro")},
    {"cross-process", N_("Cross-Process")},
    {"pastell", N_("Pastell")},
    {"lebendig", N_("Lebendig")},
    {"warm", N_("Warm")},
    {"kuehl", N_("Kühl")},
    {"nacht", N_("Day for Night")},
    {"schwarzweiss", N_("Schwarzweiß")},
    {"schwarzweiss-kontrast", N_("Schwarzweiß kontrastreich")},
    {"sepia", N_("Sepia")},
};

QString builtinPath(const char* id) { return QString(kBuiltinDir) + QString::fromLatin1(id) + ".cube"; }

} // namespace

QString root() { return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("effects"); }

QString lutDir() { return QDir(root()).filePath("LUTs"); }

void ensure()
{
    QDir().mkpath(lutDir());
    const QString readme = QDir(root()).filePath(T("Liesmich.txt"));
    if (QFileInfo::exists(readme)) return;
    QFile f(readme);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    f.write(T("Effekte für schneidi\n"
              "\n"
              "LUTs: Dateien (.cube, .3dl, .csp oder Hald-CLUT-Bilder .png/.tif) in den Ordner „LUTs“ legen.\n"
              "Unterordner werden zu eigenen Kategorien in der Effects Library. Neue Dateien erscheinen sofort.\n"
              "Eine LUT auf einen Clip ziehen legt sie in dessen Farbkorrektur (Color-Seite) ab.\n")
                .toUtf8());
}

const QStringList& lutSuffixes()
{
    static const QStringList s{"cube", "3dl", "csp", "png", "tif", "tiff", "bmp"};
    return s;
}

bool isLutFile(const QString& path) { return lutSuffixes().contains(QFileInfo(path).suffix().toLower()); }

QString lutFileFilter()
{
    QStringList patterns;
    for (const QString& s : lutSuffixes()) patterns << "*." + s;
    return T("LUT-Dateien (%1)").arg(patterns.join(' '));
}

QVector<LutEntry> builtinLuts()
{
    QVector<LutEntry> out;
    for (const Builtin& b : kBuiltins) out << LutEntry{builtinPath(b.id), T(b.name), {}};
    return out;
}

QVector<LutEntry> userLuts()
{
    QVector<LutEntry> out;
    const QDir base(lutDir());
    QDirIterator it(base.path(), QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo fi(it.next());
        if (!isLutFile(fi.fileName())) continue;
        QString group = base.relativeFilePath(fi.absolutePath());
        if (group == ".") group.clear();
        out << LutEntry{fi.absoluteFilePath(), fi.completeBaseName(), group};
    }
    std::sort(out.begin(), out.end(), [](const LutEntry& a, const LutEntry& b) {
        const int g = QString::localeAwareCompare(a.group, b.group);
        return g != 0 ? g < 0 : QString::localeAwareCompare(a.name, b.name) < 0;
    });
    return out;
}

QStringList userLutDirs()
{
    QStringList dirs{lutDir()};
    QDirIterator it(lutDir(), QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) dirs << it.next();
    return dirs;
}

bool isBuiltin(const QString& path) { return path.startsWith(kBuiltinDir); }

QString displayName(const QString& lutPath)
{
    for (const Builtin& b : kBuiltins)
        if (lutPath == builtinPath(b.id)) return T(b.name);
    return QFileInfo(lutPath).fileName();
}

QString findUserLut(const QString& fileName)
{
    if (fileName.isEmpty()) return {};
    QDirIterator it(lutDir(), {fileName}, QDir::Files, QDirIterator::Subdirectories);
    return it.hasNext() ? it.next() : QString();
}

} // namespace EffectFolders
