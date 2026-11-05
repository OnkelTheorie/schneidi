// SPDX-License-Identifier: AGPL-3.0-only
#include "core/TemplateZip.h"

#include "core/ProjectFile.h"
#include "core/Zip.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QSet>

#include <algorithm>

namespace TemplateLibrary {
namespace {

bool failWith(QString* error, QString* code, const QString& c, const QString& message)
{
    if (error) *error = message;
    if (code) *code = c;
    return false;
}

} // namespace

bool exportZip(const QString& idOrName, const QString& zip, bool overwrite, QString* error, QString* code, int* fileCount)
{
    QString err;
    const std::optional<ContentTemplate> t = find(idOrName, &err);
    if (!t) return failWith(error, code, "NOT_FOUND", err);
    if (zip.isEmpty()) return failWith(error, code, "BAD_ARGUMENT", "export needs zip (the file to write)");
    if (!zip.endsWith(".zip", Qt::CaseInsensitive)) return failWith(error, code, "BAD_ARGUMENT", "zip must end in .zip");
    if (QFileInfo::exists(zip) && !overwrite)
        return failWith(error, code, "EXISTS", "file exists (pass overwrite to replace it): " + zip);
    QVector<Zip::Source> files;
    const QDir root(t->dir);
    QDirIterator it(t->dir, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        files << Zip::Source{t->id + "/" + root.relativeFilePath(file), file};
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
    // The template's folder in the archive: its root, or the one folder everything is in
    QString prefix;
    bool atRoot = false;
    QSet<QString> tops;
    for (const Zip::Entry& e : *list) {
        const QString n = Zip::safeName(e.name);
        if (n.isEmpty()) continue;
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
    const QDir base(dir());
    QDir().mkpath(base.absolutePath());
    const QString staging = base.filePath(QString(".import-%1").arg(QRandomGenerator::global()->generate(), 8, 16, QChar('0')));
    const auto cleanUp = [&](const QString& c, const QString& message) {
        QDir(staging).removeRecursively();
        failWith(error, code, c, message);
        return std::nullopt;
    };
    if (!Zip::extract(zip, staging, prefix, &err)) return cleanUp("BAD_ZIP", err);
    std::optional<ContentTemplate> t = open(staging, &err);
    if (!t) return cleanUp("BAD_TEMPLATE", err);
    ProjectData d;
    load(*t, &d, &err);
    QString name = d.templateInfo && !d.templateInfo->name.trimmed().isEmpty() ? d.templateInfo->name.trimmed()
                   : !prefix.isEmpty()                                       ? prefix
                                                                             : QFileInfo(zip).completeBaseName();
    QString id = idForName(name);
    if (QFileInfo::exists(base.filePath(id))) {
        if (clash == ZipClash::Fail) return cleanUp("EXISTS", QString("a template named %1 exists").arg(name));
        if (clash == ZipClash::Replace) {
            if (!QDir(base.filePath(id)).removeRecursively()) return cleanUp("SAVE_FAILED", "cannot replace " + base.filePath(id));
        } else { // a name of its own: "Name 2"
            const QString shown = name;
            for (int n = 2; QFileInfo::exists(base.filePath(id)); ++n) {
                name = QString("%1 %2").arg(shown).arg(n);
                id = idForName(name);
            }
        }
    }
    const QString final = base.filePath(id);
    if (!QDir().rename(staging, final)) return cleanUp("SAVE_FAILED", "cannot create " + final);
    // From its place: media paths into the new folder, the (unique) name in the project
    std::optional<ContentTemplate> placed = open(final, &err);
    if (placed) {
        ProjectData pd;
        if (load(*placed, &pd, &err)) {
            TemplateInfo info = pd.templateInfo.value_or(TemplateInfo{});
            info.name = name;
            pd.templateInfo = info;
            ProjectFile::save(pd, placed->project, &err);
        }
        placed = open(final, &err);
    }
    if (!placed) failWith(error, code, "BAD_TEMPLATE", err);
    return placed;
}

} // namespace TemplateLibrary
