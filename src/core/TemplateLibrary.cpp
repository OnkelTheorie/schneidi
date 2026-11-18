// SPDX-License-Identifier: AGPL-3.0-only
#include "core/TemplateLibrary.h"

#include "core/AppPaths.h"
#include "core/CodeTrust.h"
#include "core/I18n.h"
#include "core/PathKey.h"
#include "core/ProjectRefs.h"
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
#include <QLockFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>

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
    return QDir(AppPaths::writable(QStandardPaths::CacheLocation)).filePath("templates-builtin");
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

// All files a project uses (forEachFile)
QSet<QString> usedFiles(ProjectData& d)
{
    QSet<QString> used;
    forEachFile(d, [&](QString& path) { used.insert(path); });
    return used;
}

void redirect(ProjectData& d, const QHash<QString, QString>& moved)
{
    for (MediaInfo& m : d.media) m.path = moved.value(m.path, m.path);
    forEachFile(d, [&](QString& path) { path = moved.value(path, path); });
}

// Same size and the same bytes
bool sameBytes(const QString& a, const QString& b)
{
    const QFileInfo fa(a), fb(b);
    if (!fa.isFile() || !fb.isFile() || fa.size() != fb.size()) return false;
    QFile x(a), y(b);
    if (!x.open(QIODevice::ReadOnly) || !y.open(QIODevice::ReadOnly)) return false;
    while (!x.atEnd())
        if (x.read(1 << 20) != y.read(1 << 20)) return false;
    return true;
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
        if (here != abs && PathKey::isInside(here, inside) && QFileInfo::exists(here)) moved[abs] = here;
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
            // Built-in names/descriptions are German keys of the dictionaries (tools/i18n_check.py reads them from
            // assets/templates/*/template.schneidi); own templates keep the user's text
            t->name = T(t->name);
            if (!t->description.isEmpty()) t->description = T(t->description);
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

std::optional<ContentTemplate> save(const ProjectData& project, const QString& name, const SaveOptions& o, QString* error,
                                    QString* code)
{
    QString tmp; // the staging folder, removed on every failure
    const auto fail = [&](const char* c, const QString& why) {
        if (!tmp.isEmpty()) QDir(tmp).removeRecursively();
        if (error) *error = why;
        if (code) *code = QString::fromLatin1(c);
        return std::nullopt;
    };
    const QString shown = name.trimmed();
    if (shown.isEmpty()) return fail("BAD_ARGUMENT", T("Die Vorlage braucht einen Namen."));
    const QString id = idForName(shown);
    const QDir base(dir());
    const QString final = base.filePath(id);
    if (QFileInfo::exists(final) && !o.replace) return fail("EXISTS", QString("a template '%1' exists already").arg(id));
    // Everything that takes long (copying the media) is written into a hidden folder of its own first, without the
    // library lock: another schneidi (the app on its UI thread) waits only for the move into place. Media of the old
    // template may be the ones copied (saving a filled template again): they are still there meanwhile.
    QDir().mkpath(base.absolutePath());
    tmp = base.filePath(QString(".%1.%2.saving").arg(id, QUuid::createUuid().toString(QUuid::Id128).left(8)));
    if (!QDir().mkpath(tmp)) return fail("SAVE_FAILED", "cannot create " + tmp);

    ProjectData d = project;
    for (Sequence& s : d.sequences)
        if (s.id == d.currentSequence) s.timeline = d.timeline;
    d.templateInfo = TemplateInfo{shown, o.description.trimmed(), o.tags, o.aspect};
    d.renderQueue.clear(); // jobs point at this machine's folders
    d.watchFolders.clear();
    d.watchLog.clear();
    d.watchRevision = 0;
    if (o.copyMedia) {
        // Clips' media, LUTs and wipe images of effects, the project brand kit's files: the template brings them all
        const QSet<QString> used = usedFiles(d);
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
            if (!QFile::copy(src, dst)) return fail("SAVE_FAILED", QString("cannot copy %1 into the template").arg(src));
            moved[src] = dst;
        }
        redirect(d, moved);
    }
    QString err;
    const bool saved = ProjectFile::save(d, QDir(tmp).filePath(ProjectFileName), &err);
    // The save remembers trusted custom code for the temporary file; the template gets its own record once in place
    CodeTrust::forget(QDir(tmp).filePath(ProjectFileName));
    if (!saved) return fail("SAVE_FAILED", err);
    if (!o.preview.isEmpty()) {
        const QString suffix = QFileInfo(o.preview).suffix().toLower();
        if (!QFile::copy(o.preview, QDir(tmp).filePath("preview." + (suffix.isEmpty() ? QString("png") : suffix))))
            return fail("SAVE_FAILED", "cannot copy the preview " + o.preview);
    }

    // From the name check to the template in place: no other save/import/delete in between
    const std::unique_ptr<QLockFile> held = lockLibrary(o.lockWaitMs);
    if (!held) return fail("BUSY", "the own templates are being changed by another schneidi, try again");
    if (QFileInfo::exists(final) && !o.replace) return fail("EXISTS", QString("a template '%1' exists already").arg(id));
    if (o.preview.isEmpty()) // replacing a template keeps its preview
        if (const QString oldPreview = previewIn(final); !oldPreview.isEmpty())
            QFile::copy(oldPreview, QDir(tmp).filePath(QFileInfo(oldPreview).fileName()));
    // The old one moves aside and is removed only once the new one is in place (back on a failure)
    const QString aside = tmp + ".old";
    if (QFileInfo::exists(final) && !QDir().rename(final, aside)) return fail("SAVE_FAILED", "cannot replace " + final);
    if (!QDir().rename(tmp, final)) {
        if (QFileInfo::exists(aside)) QDir().rename(aside, final);
        return fail("SAVE_FAILED", "cannot create " + final);
    }
    tmp.clear();
    QDir(aside).removeRecursively();
    // Once more from its place: the absolute media paths point into the final folder (relative ones did already)
    ProjectData placed;
    const QString file = QDir(final).filePath(ProjectFileName);
    if (ProjectFile::load(file, &placed, &err)) ProjectFile::save(placed, file, &err);
    return open(final, error);
}

void forEachFile(ProjectData& d, const std::function<void(QString& path)>& fn)
{
    // The Media Pool goes its own way here (only the files in use are copied), watch folders stay where they are
    ProjectRefs::forEach(d, [&](QString& path, ProjectRefs::Kind kind) {
        if (kind != ProjectRefs::Kind::PoolMedia && kind != ProjectRefs::Kind::WatchFolder) fn(path);
    });
}

bool copyTemplateFiles(ProjectData& d, const QString& templateDir, const QString& targetDir, FileCopies* out,
                       QString* error, QHash<QString, QString>* known, bool keepPool)
{
    FileCopies result;
    const QString inside = QDir::cleanPath(QDir(templateDir).absolutePath()) + '/';
    // Judged by the clean path: "<template>/media/../../x" (a hand-written or downloaded template) is no file of the
    // template - it would be read from outside it and its copy written outside the target folder
    const auto inTemplate = [&inside](const QString& path) { return PathKey::isInside(path, inside); };
    QSet<QString> used;
    forEachFile(d, [&](QString& path) {
        if (inTemplate(path)) used.insert(path);
    });
    if (keepPool)
        for (const MediaInfo& m : d.media)
            if (inTemplate(m.path)) used.insert(m.path);
    QVector<MediaInfo> kept;
    for (const MediaInfo& m : d.media)
        if (!inTemplate(m.path) || used.contains(m.path)) kept << m;
    d.media = kept;
    QStringList sources(used.begin(), used.end());
    sources.sort();
    const QDir target(targetDir);
    // Copies of earlier calls count only in this call's folder (a project saved again elsewhere gets copies there)
    const QString targetInside = QDir::cleanPath(target.absolutePath()) + '/';
    const QHash<QString, QString> knownBefore = known ? *known : QHash<QString, QString>{};
    for (const QString& src : sources) {
        if (!QFileInfo(src).isFile()) continue; // offline: the reference stays
        const qint64 size = QFileInfo(src).size();
        if (known && known->contains(src) && PathKey::isInside(known->value(src), targetInside)
            && QFileInfo(known->value(src)).size() == size) {
            result.moved[src] = known->value(src);
            continue;
        }
        // Its place in the template; a different file there keeps it, the copy takes the next free name
        const QFileInfo want(QDir::cleanPath(target.absoluteFilePath(QDir::cleanPath(src).mid(inside.size()))));
        const QString suffix = want.suffix().isEmpty() ? QString() : "." + want.suffix();
        QString dst = want.absoluteFilePath();
        bool same = false;
        for (int n = 2; QFileInfo::exists(dst); ++n) {
            if ((same = sameBytes(src, dst))) break;
            dst = want.dir().filePath(QString("%1-%2%3").arg(want.completeBaseName()).arg(n).arg(suffix));
        }
        if (!same) {
            QDir().mkpath(QFileInfo(dst).absolutePath());
            if (!QFile::copy(src, dst)) {
                for (const QString& f : std::as_const(result.written)) QFile::remove(f);
                if (known) *known = knownBefore; // the removed copies are not known
                if (error) *error = QString("cannot copy %1 to %2").arg(src, dst);
                return false;
            }
            // Copies out of the resources are read-only
            QFile::setPermissions(dst, QFile::permissions(dst) | QFileDevice::WriteOwner | QFileDevice::ReadOwner);
            result.written << dst;
        }
        result.moved[src] = dst;
        if (known) known->insert(src, dst);
    }
    redirect(d, result.moved);
    if (out) *out = result;
    return true;
}

QStringList foldersUsed(const ProjectData& project)
{
    ProjectData d = project; // forEachFile may change paths, this only reads them
    QStringList roots;
    for (const QString& r : {dir(), builtInCache()}) roots << QDir::cleanPath(QDir(r).absolutePath()) + '/';
    QHash<QString, QString> found; // by PathKey: one folder in two spellings (Windows) is one
    const auto look = [&](const QString& given) {
        const QString path = QDir::cleanPath(given);
        for (const QString& root : std::as_const(roots)) {
            if (!PathKey::isInside(path, root)) continue;
            const int slash = path.indexOf('/', root.size()); // <root>/<id>/...: a file directly in the root is none
            if (slash > root.size()) found.insert(PathKey::of(path.left(slash)), path.left(slash));
        }
    };
    forEachFile(d, [&](QString& path) { look(path); });
    for (const MediaInfo& m : d.media) look(m.path);
    QStringList list = found.values();
    list.sort();
    return list;
}

std::unique_ptr<QLockFile> lockLibrary(int timeoutMs)
{
    if (timeoutMs < 0) {
        bool ok = false;
        const int ms = qEnvironmentVariableIntValue("SCHNEIDI_TEMPLATE_LOCK_MS", &ok); // tests: not a minute
        timeoutMs = ok && ms >= 0 ? ms : 60000;
    }
    QDir().mkpath(dir());
    auto l = std::make_unique<QLockFile>(QDir(dir()).filePath(".templates.lock"));
    l->setStaleLockTime(0);
    if (!l->tryLock(timeoutMs)) return nullptr;
    return l;
}

namespace {

// The folder of an own template: the id as given, else one whose id (folder name) differs only in case - on a
// case-sensitive file system "My-Short" and "my-short" must not be two answers to an AI, and Windows sees one
// folder anyway -, else one of that name (as find()). Empty = none. Called with the library lock held.
QString ownFolder(const QString& key)
{
    const QString k = key.trimmed();
    if (k.isEmpty() || k.contains('/') || k.contains('\\') || k.startsWith('.')) return {};
    const QDir base(dir());
    if (QFileInfo::exists(QDir(base.filePath(k)).filePath(ProjectFileName))) return base.filePath(k);
    const QVector<ContentTemplate> list = own();
    for (const ContentTemplate& t : list)
        if (t.id.compare(k, Qt::CaseInsensitive) == 0) return t.dir;
    for (const ContentTemplate& t : list)
        if (t.name.compare(k, Qt::CaseInsensitive) == 0) return t.dir;
    return {};
}

bool failed(QString* error, QString* code, const char* c, const QString& why)
{
    if (error) *error = why;
    if (code) *code = QString::fromLatin1(c);
    return false;
}

} // namespace

bool remove(const QString& id, QString* error, QString* code, int waitMs)
{
    const std::unique_ptr<QLockFile> held = lockLibrary(waitMs);
    if (!held) return failed(error, code, "BUSY", "the own templates are being changed by another schneidi, try again");
    const QString folder = ownFolder(id);
    if (folder.isEmpty()) return failed(error, code, "NOT_FOUND", QString("no own content template '%1'").arg(id));
    if (!QDir(folder).removeRecursively()) return failed(error, code, "FAILED", "cannot delete " + folder);
    return true;
}

bool rename(const QString& id, const QString& name, QString* error, QString* code, int waitMs)
{
    const QString shown = name.trimmed();
    if (shown.isEmpty()) return failed(error, code, "BAD_ARGUMENT", T("Die Vorlage braucht einen Namen."));
    const std::unique_ptr<QLockFile> held = lockLibrary(waitMs);
    if (!held) return failed(error, code, "BUSY", "the own templates are being changed by another schneidi, try again");
    QString folder = ownFolder(id);
    if (folder.isEmpty()) return failed(error, code, "NOT_FOUND", QString("no own content template '%1'").arg(id));
    QString file = QDir(folder).filePath(ProjectFileName);
    ProjectData d;
    QString why;
    if (!ProjectFile::load(file, &d, &why)) return failed(error, code, "FAILED", why);
    TemplateInfo info = d.templateInfo.value_or(TemplateInfo{});
    info.name = shown;
    d.templateInfo = info;
    if (!ProjectFile::save(d, file, &why)) return failed(error, code, "FAILED", why);
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
