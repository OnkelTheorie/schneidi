// SPDX-License-Identifier: AGPL-3.0-only
#include "core/TemplateLibrary.h"

#include "core/I18n.h"
#include "core/StartTemplates.h"
#include "core/TemplateFill.h"
#include "core/TimelineOps.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace TemplateLibrary {
namespace {

QString& builtInOverride()
{
    static QString root;
    return root;
}

// Built-in templates are copied here before use (the engine cannot read media out of the resources)
QString builtInCache()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath("templates-builtin");
}

// Whether the copy `dst` still is `src`: same size and the modification time of `src` it was stamped with on copying
// (resources keep their source file's time, so a changed template of the same size in a program update counts as
// new; no file is read). Without a time of `src` the contents are compared.
bool sameFile(const QFileInfo& src, const QFileInfo& dst)
{
    if (!dst.exists() || dst.size() != src.size()) return false;
    const QDateTime t = src.lastModified();
    if (t.isValid()) return dst.lastModified() == t;
    QFile a(src.filePath()), b(dst.filePath());
    if (!a.open(QIODevice::ReadOnly) || !b.open(QIODevice::ReadOnly)) return false;
    while (!a.atEnd())
        if (a.read(1 << 20) != b.read(1 << 20)) return false;
    return true;
}

// Copies the files of `from` (resources or a folder) into `to` where they are missing or changed (sameFile)
bool copyTree(const QString& from, const QString& to)
{
    bool ok = true;
    QDirIterator it(from, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString src = it.next();
        const QString dst = QDir(to).filePath(QDir(from).relativeFilePath(src));
        const QFileInfo si(src), di(dst);
        if (sameFile(si, di)) continue;
        QDir().mkpath(di.absolutePath());
        QFile::remove(dst);
        if (!QFile::copy(src, dst)) {
            ok = false;
            continue;
        }
        // Copies out of the resources are read-only: a newer version must be able to replace them
        QFile::setPermissions(dst, QFile::permissions(dst) | QFileDevice::WriteOwner | QFileDevice::ReadOwner);
        // Stamped with the source's time for sameFile
        if (si.lastModified().isValid()) {
            QFile f(dst);
            if (f.open(QIODevice::ReadWrite)) f.setFileTime(si.lastModified(), QFileDevice::FileModificationTime);
        }
    }
    return ok;
}

// All media paths a project's timelines use (files of clips; titles and nested clips have none)
QSet<QString> usedMedia(const ProjectData& d)
{
    QSet<QString> used;
    const auto scan = [&](const Timeline& tl) {
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (const Track& t : tl.tracks(k))
                for (const Clip& c : t.clips)
                    if (c.kind == ClipKind::Media && !c.mediaPath.isEmpty()) used.insert(c.mediaPath);
    };
    scan(d.timeline);
    for (const Sequence& s : d.sequences)
        if (s.id != d.currentSequence) scan(s.timeline);
    return used;
}

void redirect(ProjectData& d, const QHash<QString, QString>& moved)
{
    for (MediaInfo& m : d.media) m.path = moved.value(m.path, m.path);
    const auto fix = [&](Timeline& tl) {
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (Track& t : tl.tracks(k))
                for (Clip& c : t.clips)
                    if (c.kind == ClipKind::Media) c.mediaPath = moved.value(c.mediaPath, c.mediaPath);
    };
    fix(d.timeline);
    for (Sequence& s : d.sequences) fix(s.timeline);
}

// A template's project: media inside its folder are taken from there even when the absolute path they were saved
// with still exists (a copy of the folder, an unpacked ZIP of it on the same machine)
bool loadProject(const QString& file, ProjectData* d, QString* error)
{
    if (!ProjectFile::load(file, d, error)) return false;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return true;
    const QDir folder = QFileInfo(file).absoluteDir();
    const QString inside = folder.absolutePath() + '/';
    QHash<QString, QString> moved;
    for (const QJsonValue& v : QJsonDocument::fromJson(f.readAll()).object().value("media").toArray()) {
        const QJsonObject o = v.toObject();
        const QString rel = o.value("relPath").toString(), abs = o.value("path").toString();
        if (rel.isEmpty() || abs.isEmpty()) continue;
        const QString here = QDir::cleanPath(folder.absoluteFilePath(rel));
        if (here != abs && here.startsWith(inside) && QFileInfo::exists(here)) moved[abs] = here;
    }
    if (!moved.isEmpty()) redirect(*d, moved);
    return true;
}

QString previewIn(const QString& dir)
{
    for (const char* name : {"preview.mp4", "preview.webm", "preview.png", "preview.jpg"})
        if (QFileInfo::exists(QDir(dir).filePath(name))) return QDir(dir).filePath(name);
    return {};
}

QVector<ContentTemplate> sorted(QVector<ContentTemplate> list)
{
    std::stable_sort(list.begin(), list.end(), [](const ContentTemplate& a, const ContentTemplate& b) {
        return I18n::compareNames(a.name, b.name) < 0;
    });
    return list;
}

} // namespace

QString dir() { return StartTemplates::dir(); }

QString builtInRoot() { return builtInOverride().isEmpty() ? QStringLiteral(":/templates") : builtInOverride(); }

void setBuiltInRoot(const QString& root) { builtInOverride() = root; }

QString aspectLabel(int width, int height)
{
    if (width <= 0 || height <= 0) return {};
    const double r = double(width) / height;
    for (const auto& [w, h] : {std::pair{16, 9}, {9, 16}, {1, 1}, {4, 5}, {4, 3}, {3, 4}, {21, 9}, {2, 3}, {3, 2}})
        if (std::abs(r - double(w) / h) < 0.01 * r) return QString("%1:%2").arg(w).arg(h);
    const int g = std::gcd(width, height);
    return QString("%1:%2").arg(width / g).arg(height / g);
}

std::optional<ContentTemplate> open(const QString& path, QString* error)
{
    const QFileInfo fi(path);
    const QString project = fi.isDir() ? QDir(fi.absoluteFilePath()).filePath(ProjectFileName) : fi.absoluteFilePath();
    ProjectData d;
    if (!loadProject(project, &d, error)) return std::nullopt;
    ContentTemplate t;
    t.dir = QFileInfo(project).absolutePath();
    t.id = QFileInfo(t.dir).fileName();
    t.project = project;
    t.preview = previewIn(t.dir);
    t.width = d.format.width;
    t.height = d.format.height;
    t.fps = d.format.rate.fps();
    t.length = TimelineOps::endFrame(d.timeline);
    t.slotIds = TemplateFill::slotIds(d);
    if (d.templateInfo) {
        t.name = d.templateInfo->name.trimmed();
        t.description = d.templateInfo->description;
        t.tags = d.templateInfo->tags;
        t.aspect = d.templateInfo->aspect;
    }
    if (t.name.isEmpty()) t.name = t.id;
    if (t.aspect.isEmpty()) {
        // The open timeline's own size counts (a 9:16 short in a 16:9 project)
        int w = d.format.width, h = d.format.height;
        for (const Sequence& s : d.sequences)
            if (s.id == d.currentSequence && s.hasOwnSize()) w = s.width, h = s.height;
        t.aspect = aspectLabel(w, h);
    }
    return t;
}

QVector<ContentTemplate> builtIn()
{
    QVector<ContentTemplate> list;
    const QString root = builtInRoot();
    for (const QString& id : QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (!QFileInfo::exists(QDir(root).filePath(id + "/" + ProjectFileName))) continue;
        const QString target = QDir(builtInCache()).filePath(id);
        copyTree(QDir(root).filePath(id), target);
        if (std::optional<ContentTemplate> t = open(target)) {
            t->builtIn = true;
            list << *t;
        }
    }
    return sorted(list);
}

QVector<ContentTemplate> own()
{
    QVector<ContentTemplate> list;
    for (const QFileInfo& fi : QDir(dir()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (fi.fileName().startsWith('.')) continue; // a save in progress
        if (!QFileInfo::exists(QDir(fi.absoluteFilePath()).filePath(ProjectFileName))) continue;
        if (std::optional<ContentTemplate> t = open(fi.absoluteFilePath())) list << *t;
    }
    return sorted(list);
}

QVector<ContentTemplate> all() { return builtIn() + own(); }

std::optional<ContentTemplate> find(const QString& key, QString* error)
{
    const QString k = key.trimmed();
    if (k.isEmpty()) {
        if (error) *error = "no template given";
        return std::nullopt;
    }
    const QFileInfo fi(k);
    if (fi.isAbsolute() && fi.exists()) return open(k, error);
    const QVector<ContentTemplate> list = all();
    const auto same = [&k](const QString& s) { return s.compare(k, Qt::CaseInsensitive) == 0; };
    for (const ContentTemplate& t : list)
        if (same(t.id)) return t;
    for (const ContentTemplate& t : list)
        if (same(t.name)) return t;
    if (error) *error = QString("no content template '%1'").arg(k);
    return std::nullopt;
}

bool load(const ContentTemplate& t, ProjectData* data, QString* error) { return loadProject(t.project, data, error); }

QString idForName(const QString& name) { return StartTemplates::idForName(name.trimmed()); }

std::optional<ContentTemplate> save(const ProjectData& project, const QString& name, const SaveOptions& o, QString* error)
{
    const auto fail = [&](const QString& why) {
        if (error) *error = why;
        return std::nullopt;
    };
    const QString shown = name.trimmed();
    if (shown.isEmpty()) return fail(T("Die Vorlage braucht einen Namen."));
    const QString id = idForName(shown);
    const QDir base(dir());
    const QString final = base.filePath(id);
    if (QFileInfo::exists(final) && !o.replace) return fail(QString("a template '%1' exists already").arg(id));
    // Written next to it first: media of the old template may be the ones copied (saving a filled template again)
    const QString tmp = base.filePath("." + id + ".saving");
    QDir(tmp).removeRecursively();
    if (!QDir().mkpath(tmp)) return fail("cannot create " + tmp);

    ProjectData d = project;
    for (Sequence& s : d.sequences)
        if (s.id == d.currentSequence) s.timeline = d.timeline;
    d.templateInfo = TemplateInfo{shown, o.description.trimmed(), o.tags, o.aspect};
    d.renderQueue.clear(); // jobs point at this machine's folders
    d.watchFolders.clear();
    d.watchLog.clear();
    d.watchRevision = 0;
    if (o.copyMedia) {
        const QSet<QString> used = usedMedia(d);
        QVector<MediaInfo> kept;
        for (const MediaInfo& m : d.media)
            if (used.contains(m.path)) kept << m;
        d.media = kept;
        QHash<QString, QString> moved;
        QSet<QString> names;
        QDir().mkpath(QDir(tmp).filePath(MediaDirName));
        QStringList paths(used.begin(), used.end());
        paths.sort();
        for (const QString& src : paths) {
            if (!QFileInfo::exists(src)) continue; // offline: the reference stays
            const QFileInfo sf(src);
            QString fileName = sf.fileName();
            for (int n = 2; names.contains(fileName.toLower()); ++n)
                fileName = QString("%1-%2%3").arg(sf.completeBaseName()).arg(n)
                               .arg(sf.suffix().isEmpty() ? QString() : "." + sf.suffix());
            names.insert(fileName.toLower());
            const QString dst = QDir(tmp).filePath(QString(MediaDirName) + "/" + fileName);
            if (!QFile::copy(src, dst)) {
                QDir(tmp).removeRecursively();
                return fail(QString("cannot copy %1 into the template").arg(src));
            }
            moved[src] = dst;
        }
        redirect(d, moved);
    }
    QString err;
    if (!ProjectFile::save(d, QDir(tmp).filePath(ProjectFileName), &err)) {
        QDir(tmp).removeRecursively();
        return fail(err);
    }
    QString oldPreview = previewIn(final);
    if (!o.preview.isEmpty()) {
        const QString suffix = QFileInfo(o.preview).suffix().toLower();
        if (!QFile::copy(o.preview, QDir(tmp).filePath("preview." + (suffix.isEmpty() ? QString("png") : suffix)))) {
            QDir(tmp).removeRecursively();
            return fail("cannot copy the preview " + o.preview);
        }
    } else if (!oldPreview.isEmpty()) { // replacing a template keeps its preview
        QFile::copy(oldPreview, QDir(tmp).filePath(QFileInfo(oldPreview).fileName()));
    }
    if (QFileInfo::exists(final) && !QDir(final).removeRecursively()) {
        QDir(tmp).removeRecursively();
        return fail("cannot replace " + final);
    }
    if (!QDir().rename(tmp, final)) {
        QDir(tmp).removeRecursively();
        return fail("cannot create " + final);
    }
    // Once more from its place: the absolute media paths point into the final folder (relative ones did already)
    ProjectData placed;
    const QString file = QDir(final).filePath(ProjectFileName);
    if (ProjectFile::load(file, &placed, &err)) ProjectFile::save(placed, file, &err);
    return open(final, error);
}

bool remove(const QString& id, QString* error)
{
    const QString folder = QDir(dir()).filePath(id);
    if (id.isEmpty() || id.contains('/') || id.contains('\\') || id.startsWith('.')
        || !QFileInfo::exists(QDir(folder).filePath(ProjectFileName))) {
        if (error) *error = QString("no own content template '%1'").arg(id);
        return false;
    }
    if (!QDir(folder).removeRecursively()) {
        if (error) *error = "cannot delete " + folder;
        return false;
    }
    return true;
}

bool rename(const QString& id, const QString& name, QString* error)
{
    const QString shown = name.trimmed();
    if (shown.isEmpty()) {
        if (error) *error = T("Die Vorlage braucht einen Namen.");
        return false;
    }
    QString folder = QDir(dir()).filePath(id);
    QString file = QDir(folder).filePath(ProjectFileName);
    if (id.isEmpty() || id.contains('/') || id.contains('\\') || !QFileInfo::exists(file)) {
        if (error) *error = QString("no own content template '%1'").arg(id);
        return false;
    }
    ProjectData d;
    if (!ProjectFile::load(file, &d, error)) return false;
    TemplateInfo info = d.templateInfo.value_or(TemplateInfo{});
    info.name = shown;
    d.templateInfo = info;
    if (!ProjectFile::save(d, file, error)) return false;
    const QString newFolder = QDir(dir()).filePath(idForName(shown));
    // The folder follows the name when that one is free (a template of that name keeps its own)
    if (newFolder != folder && !QFileInfo::exists(newFolder) && QDir().rename(folder, newFolder)) {
        file = QDir(newFolder).filePath(ProjectFileName);
        ProjectData placed; // absolute media paths into the new folder
        if (ProjectFile::load(file, &placed, nullptr)) ProjectFile::save(placed, file, nullptr);
    }
    return true;
}

} // namespace TemplateLibrary
