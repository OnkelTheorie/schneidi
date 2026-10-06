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

// Mitgelieferte Verlaufsblenden (Bild erzeugt engine/Lumas nach id), Reihenfolge = Anzeige
constexpr const char* kLumaDir = ":/lumas/";
constexpr Builtin kLumas[] = {
    {"kreis", N_("Kreis auf")},
    {"kreis-zu", N_("Kreis zu")},
    {"raute", N_("Raute")},
    {"rechteck", N_("Rechteck auf")},
    {"tuer-h", N_("Tür horizontal")},
    {"tuer-v", N_("Tür vertikal")},
    {"diagonal", N_("Diagonale")},
    {"uhr", N_("Uhrzeiger")},
    {"faecher", N_("Fächer")},
    {"spirale", N_("Spirale")},
    {"jalousie-h", N_("Jalousie horizontal")},
    {"jalousie-v", N_("Jalousie vertikal")},
    {"schachbrett", N_("Schachbrett")},
    {"welle", N_("Welle")},
    {"pixel", N_("Pixel")},
    {"wolken", N_("Wolken")},
};

QString lumaPath(const char* id) { return QString(kLumaDir) + QString::fromLatin1(id); }

QVector<LutEntry> scan(const QString& dir, bool (*accept)(const QString&), bool keepSuffix)
{
    QVector<LutEntry> out;
    const QDir base(dir);
    QDirIterator it(base.path(), QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo fi(it.next());
        if (!accept(fi.fileName())) continue;
        QString group = base.relativeFilePath(fi.absolutePath());
        if (group == ".") group.clear();
        out << LutEntry{fi.absoluteFilePath(), keepSuffix ? fi.fileName() : fi.completeBaseName(), group};
    }
    std::sort(out.begin(), out.end(), [](const LutEntry& a, const LutEntry& b) {
        const int g = QString::localeAwareCompare(a.group, b.group);
        return g != 0 ? g < 0 : QString::localeAwareCompare(a.name, b.name) < 0;
    });
    return out;
}

void addDirs(const QString& dir, QStringList* out)
{
    *out << dir;
    QDirIterator it(dir, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) *out << it.next();
}

QString findIn(const QString& dir, const QString& fileName)
{
    if (fileName.isEmpty()) return {};
    QDirIterator it(dir, {fileName}, QDir::Files, QDirIterator::Subdirectories);
    return it.hasNext() ? it.next() : QString();
}

} // namespace

QString root() { return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("effects"); }

QString lutDir() { return QDir(root()).filePath("LUTs"); }

QString transitionDir() { return QDir(root()).filePath("Transitions"); }

QString frei0rDir() { return QDir(root()).filePath("frei0r"); }

void ensure()
{
    QDir().mkpath(lutDir());
    QDir().mkpath(transitionDir());
    QDir().mkpath(frei0rDir());
    // New content (frei0r) -> replace an older readme
    const QString readme = QDir(root()).filePath(T("Liesmich.txt"));
    QFile old(readme);
    if (old.open(QIODevice::ReadOnly) && old.readAll().contains("frei0r")) return;
    old.close();
    QFile f(readme);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    f.write(T("Effekte für schneidi\n"
              "\n"
              "LUTs: Dateien (.cube, .3dl, .csp oder Hald-CLUT-Bilder .png/.tif) in den Ordner „LUTs“ legen.\n"
              "Unterordner werden zu eigenen Kategorien in der Effects Library. Neue Dateien erscheinen sofort.\n"
              "Eine LUT auf einen Clip ziehen legt sie in dessen Farbkorrektur (Color-Seite) ab.\n"
              "\n"
              "Übergänge: Graustufenbilder (.png, .jpg, .pgm, .bmp, .tif) in den Ordner „Transitions“ legen\n"
              "(z. B. Luma-Übergänge aus Kdenlive, Shotcut oder OpenShot). Dunkle Stellen wechseln zuerst.\n"
              "Auf einen Schnitt ziehen wie die anderen Übergänge.\n"
              "\n"
              "frei0r: zusätzliche frei0r-Plugins (.so unter Linux, .dll unter Windows) in den Ordner „frei0r“ legen.\n"
              "Sie erscheinen nach einem Neustart unter Open FX → frei0r.\n")
                .toUtf8());
}

const QStringList& lutSuffixes()
{
    static const QStringList s{"cube", "3dl", "csp", "png", "tif", "tiff", "bmp"};
    return s;
}

bool isLutFile(const QString& path) { return lutSuffixes().contains(QFileInfo(path).suffix().toLower()); }

const QStringList& transitionSuffixes()
{
    static const QStringList s{"png", "jpg", "jpeg", "pgm", "bmp", "tif", "tiff"};
    return s;
}

bool isTransitionFile(const QString& path)
{
    return transitionSuffixes().contains(QFileInfo(path).suffix().toLower());
}

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

QVector<LutEntry> userLuts() { return scan(lutDir(), &isLutFile, false); }

QVector<LutEntry> builtinTransitions()
{
    QVector<LutEntry> out;
    for (const Builtin& b : kLumas) out << LutEntry{lumaPath(b.id), T(b.name), {}};
    return out;
}

QVector<LutEntry> userTransitions() { return scan(transitionDir(), &isTransitionFile, false); }

QStringList watchDirs()
{
    QStringList dirs;
    addDirs(lutDir(), &dirs);
    addDirs(transitionDir(), &dirs);
    return dirs;
}

bool isBuiltin(const QString& path) { return path.startsWith(kBuiltinDir) || path.startsWith(kLumaDir); }

QString builtinId(const QString& path)
{
    if (!isBuiltin(path)) return {};
    return QFileInfo(path).completeBaseName();
}

QString displayName(const QString& path)
{
    for (const Builtin& b : kBuiltins)
        if (path == builtinPath(b.id)) return T(b.name);
    for (const Builtin& b : kLumas)
        if (path == lumaPath(b.id)) return T(b.name);
    return isTransitionFile(path) && !isLutFile(path) ? QFileInfo(path).completeBaseName() : QFileInfo(path).fileName();
}

QString findUserLut(const QString& fileName) { return findIn(lutDir(), fileName); }

QString findUserTransition(const QString& fileName) { return findIn(transitionDir(), fileName); }

} // namespace EffectFolders
