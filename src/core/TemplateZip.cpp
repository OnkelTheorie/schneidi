// SPDX-License-Identifier: AGPL-3.0-only
#include "core/TemplateZip.h"

#include "core/ProjectFile.h"
#include "core/Zip.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRandomGenerator>
#include <QSet>
#include <QStorageInfo>
#include <QTemporaryDir>

#include <algorithm>

namespace TemplateLibrary {
namespace {

bool failWith(QString* error, QString* code, const QString& c, const QString& message)
{
    if (error) *error = message;
    if (code) *code = c;
    return false;
}

// One part of a file name valid on every system (what Zip::safeName accepts): characters Windows refuses become '_',
// no dot or space at the end, no device name ("nul.mp4" -> "nul_.mp4")
QString portablePart(const QString& part)
{
    QString s;
    for (const QChar c : part)
        s += c.unicode() < 0x20 || QStringLiteral("<>:\"|?*\\").contains(c) ? QChar('_') : c;
    while (!s.isEmpty() && (s.endsWith('.') || s.endsWith(' '))) s.chop(1);
    if (s.isEmpty()) s = QStringLiteral("_");
    if (Zip::safeName(s).isEmpty()) s.insert(s.section('.', 0, 0).size(), '_'); // a device name
    return s;
}

// A path for `rel` (a file in the template's folder) that every system unpacks as a file of its own: its parts
// portable, not another file's name by case ("Clip.mp4" next to "clip.mp4" -> "clip-2.mp4")
QString portablePath(const QString& rel, QSet<QString>& used)
{
    QStringList parts = rel.split('/', Qt::SkipEmptyParts);
    for (QString& p : parts) p = portablePart(p);
    QString out = parts.join('/');
    const QString last = parts.takeLast();
    const QString folder = parts.isEmpty() ? QString() : parts.join('/') + '/';
    const qsizetype dot = last.indexOf('.', 1);
    const QString stem = dot > 0 ? last.left(dot) : last, suffix = dot > 0 ? last.mid(dot) : QString();
    for (int n = 2; used.contains(out.toCaseFolded()); ++n) out = QString("%1%2-%3%4").arg(folder, stem).arg(n).arg(suffix);
    used.insert(out.toCaseFolded());
    return out;
}

// Media references of a project moved to other files (old absolute path -> new)
void redirectMedia(ProjectData& d, const QHash<QString, QString>& moved)
{
    const auto fix = [&](QString& path) {
        if (!path.isEmpty()) path = moved.value(QDir::cleanPath(path), path);
    };
    for (MediaInfo& m : d.media) fix(m.path);
    forEachFile(d, fix); // clips, LUTs, wipe images, the brand kit's files
}

constexpr quint64 kMiB = quint64(1) << 20;

} // namespace

quint64 importLimit(qint64 zipSize)
{
    return std::min<quint64>(quint64(4) << 30, quint64(std::max<qint64>(zipSize, 0)) * 20 + 64 * kMiB);
}

bool exportZip(const QString& idOrName, const QString& zip, bool overwrite, QString* error, QString* code, int* fileCount)
{
    QString err;
    const std::optional<ContentTemplate> t = find(idOrName, &err);
    if (!t) return failWith(error, code, "NOT_FOUND", err);
    if (zip.isEmpty()) return failWith(error, code, "BAD_ARGUMENT", "export needs zip (the file to write)");
    if (!zip.endsWith(".zip", Qt::CaseInsensitive)) return failWith(error, code, "BAD_ARGUMENT", "zip must end in .zip");
    if (QFileInfo::exists(zip) && !overwrite)
        return failWith(error, code, "EXISTS", "file exists (pass overwrite to replace it): " + zip);
    const QDir root(t->dir);
    QStringList rels;
    QDirIterator it(t->dir, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext()) rels << root.relativeFilePath(it.next());
    rels.sort();
    // Names legal here but not everywhere ("a:b.mp4", "x\y.png" on Linux) get a portable one in the archive (import
    // refuses the others), the project in it points at those
    QVector<Zip::Source> files;
    QSet<QString> used{QString(ProjectFileName).toCaseFolded()};
    QHash<QString, QString> moved;
    for (const QString& rel : rels) {
        const QString name = rel == ProjectFileName ? rel : portablePath(rel, used);
        if (name != rel) moved.insert(QDir::cleanPath(root.absoluteFilePath(rel)), QDir::cleanPath(root.absoluteFilePath(name)));
        files << Zip::Source{t->id + "/" + name, root.absoluteFilePath(rel)};
    }
    QTemporaryDir tmp;
    // A built-in template is stored with its German source name; the archive carries the name the list shows (the
    // user's language), as for an own template
    if (!moved.isEmpty() || t->builtIn) {
        ProjectData d;
        if (!load(*t, &d, &err)) return failWith(error, code, "BAD_TEMPLATE", err);
        redirectMedia(d, moved);
        if (t->builtIn && d.templateInfo) {
            d.templateInfo->name = t->name;
            d.templateInfo->description = t->description;
        }
        const QString project = tmp.filePath(ProjectFileName);
        QFile f(project);
        // Written as if in the template's folder: relative paths as they are in the archive
        if (!tmp.isValid() || !f.open(QIODevice::WriteOnly) || f.write(ProjectFile::toJson(d, t->project)) < 0)
            return failWith(error, code, "SAVE_FAILED", "cannot write a temporary file");
        f.close();
        for (Zip::Source& s : files)
            if (s.name == t->id + "/" + ProjectFileName) s.file = project;
    }
    std::sort(files.begin(), files.end(), [](const Zip::Source& x, const Zip::Source& y) { return x.name < y.name; });
    QDir().mkpath(QFileInfo(zip).absolutePath());
    const QString part = zip + ".part";
    if (!Zip::write(part, files, &err)) {
        QFile::remove(part);
        return failWith(error, code, "SAVE_FAILED", err);
    }
    QFile::remove(zip);
    if (!QFile::rename(part, zip)) {
        QFile::remove(part);
        return failWith(error, code, "SAVE_FAILED", "cannot write " + zip);
    }
    if (fileCount) *fileCount = int(files.size());
    return true;
}

std::optional<ContentTemplate> importZip(const QString& zip, ZipClash clash, QString* error, QString* code)
{
    if (!QFileInfo::exists(zip)) return failWith(error, code, "NOT_FOUND", "no file " + zip), std::nullopt;
    QString err;
    const std::optional<QVector<Zip::Entry>> list = Zip::entries(zip, &err);
    if (!list) return failWith(error, code, "BAD_ZIP", err), std::nullopt;
    // The template's folder in the archive: its root, or the one folder everything is in (what macOS and Windows add,
    // "__MACOSX/", ".DS_Store", "Thumbs.db", does not count)
    QString prefix;
    bool atRoot = false;
    QSet<QString> tops;
    for (const Zip::Entry& e : *list) {
        const QString n = Zip::safeName(e.name);
        if (n.isEmpty() || Zip::junk(n)) continue;
        if (n == ProjectFileName) atRoot = true;
        tops.insert(n.section('/', 0, 0));
    }
    if (!atRoot) {
        if (tops.size() == 1) prefix = *tops.begin();
        bool found = false;
        for (const Zip::Entry& e : *list)
            found |= !prefix.isEmpty() && Zip::safeName(e.name) == prefix + "/" + ProjectFileName;
        if (!found)
            return failWith(error, code, "BAD_TEMPLATE", QString("no %1 in %2 (a template is a folder with it)")
                                                             .arg(ProjectFileName, QFileInfo(zip).fileName())),
                   std::nullopt;
    }
    // A "zip bomb" stops before anything is written (importLimit), and so does a full disk
    quint64 total = 0;
    for (const Zip::Entry& e : *list)
        if (!e.dir) total += e.size;
    const quint64 limit = importLimit(QFileInfo(zip).size());
    if (total > limit)
        return failWith(error, code, "BAD_ZIP", QString("%1: too big when unpacked (%2 MB)")
                                                    .arg(QFileInfo(zip).fileName()).arg(total / kMiB)),
               std::nullopt;
    const QDir base(dir());
    QDir().mkpath(base.absolutePath());
    const QStorageInfo disk(base.absolutePath());
    if (disk.isValid() && disk.bytesAvailable() >= 0 && total + 64 * kMiB > quint64(disk.bytesAvailable()))
        return failWith(error, code, "SAVE_FAILED", QString("not enough free space for %1 MB in %2")
                                                        .arg(total / kMiB).arg(QDir::toNativeSeparators(base.absolutePath()))),
               std::nullopt;
    const QString staging = base.filePath(QString(".import-%1").arg(QRandomGenerator::global()->generate(), 8, 16, QChar('0')));
    const auto cleanUp = [&](const QString& c, const QString& message) {
        QDir(staging).removeRecursively();
        failWith(error, code, c, message);
        return std::nullopt;
    };
    if (!Zip::extract(zip, staging, prefix, &err, limit)) return cleanUp("BAD_ZIP", err);
    // Checked and named while still hidden: a template that does not open never shows up among the own ones
    std::optional<ContentTemplate> t = open(staging, &err);
    if (!t) return cleanUp("BAD_TEMPLATE", err);
    ProjectData d;
    if (!load(*t, &d, &err)) return cleanUp("BAD_TEMPLATE", err);
    QString name = d.templateInfo && !d.templateInfo->name.trimmed().isEmpty() ? d.templateInfo->name.trimmed()
                   : !prefix.isEmpty()                                       ? prefix
                                                                             : QFileInfo(zip).completeBaseName();
    QString id = idForName(name);
    if (QFileInfo::exists(base.filePath(id))) {
        if (clash == ZipClash::Fail) return cleanUp("EXISTS", QString("a template named %1 exists").arg(name));
        if (clash == ZipClash::KeepBoth) { // a name of its own: "Name 2"
            const QString shown = name;
            for (int n = 2; QFileInfo::exists(base.filePath(id)); ++n) {
                name = QString("%1 %2").arg(shown).arg(n);
                id = idForName(name);
            }
        }
    }
    TemplateInfo info = d.templateInfo.value_or(TemplateInfo{});
    info.name = name;
    d.templateInfo = info;
    if (!ProjectFile::save(d, t->project, &err)) return cleanUp("SAVE_FAILED", err);
    // Replace: the old one moves aside and is removed only once the new one is in place (back on any failure)
    const QString final = base.filePath(id);
    QString replaced;
    if (QFileInfo::exists(final)) {
        replaced = staging + ".old";
        if (!QDir().rename(final, replaced)) return cleanUp("SAVE_FAILED", "cannot replace " + final);
    }
    const auto restore = [&] {
        if (!replaced.isEmpty()) QDir().rename(replaced, final);
    };
    if (!QDir().rename(staging, final)) {
        restore();
        return cleanUp("SAVE_FAILED", "cannot create " + final);
    }
    // From its place: absolute media paths into the new folder (the relative ones were right already)
    std::optional<ContentTemplate> placed = open(final, &err);
    ProjectData pd;
    if (placed && load(*placed, &pd, &err) && ProjectFile::save(pd, placed->project, &err)) placed = open(final, &err);
    else placed.reset();
    if (!placed) {
        QDir(final).removeRecursively();
        restore();
        failWith(error, code, "SAVE_FAILED", err);
        return std::nullopt;
    }
    if (!replaced.isEmpty()) QDir(replaced).removeRecursively();
    return placed;
}

} // namespace TemplateLibrary
