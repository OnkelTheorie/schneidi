// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Content templates (0.18): a whole schneidi project with placeholders (ClipSlot, core/TemplateFill.h), one folder per
// template:
//   <templates>/<id>/template.schneidi   the project (media inside the folder referenced relative to it)
//   <templates>/<id>/preview.mp4|png     optional preview for browsers
//   <templates>/<id>/media/              optional bundled media
// Own templates live in AppDataLocation/templates (next to the start templates' *.json files, core/StartTemplates.h,
// which stay as they are). Built-in ones come with the program as Qt resources (assets/templates/<id>/, like
// assets/luts) and are copied into the cache folder before use, so the engine can read their media; they are listed
// before the own ones. Sharing a template = a ZIP of its folder.

#include "core/ProjectFile.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>

struct ContentTemplate {
    QString id;          // folder name
    QString name;        // TemplateInfo::name, else the folder name
    QString description;
    QStringList tags;
    QString aspect;      // TemplateInfo::aspect, else from the project format ("9:16", "16:9", "1:1", "4:5" or "W:H")
    bool builtIn = false;
    QString dir;         // the folder (for built-in ones: their copy in the cache)
    QString project;     // dir/template.schneidi
    QString preview;     // dir/preview.mp4 or preview.png, empty = none
    int width = 0, height = 0;   // project format
    double fps = 0;
    int length = 0;      // frames of the open timeline
    QStringList slotIds; // slot ids (TemplateFill::slotIds)
};

class QLockFile;

namespace TemplateLibrary {

inline constexpr const char* ProjectFileName = "template.schneidi";
inline constexpr const char* MediaDirName = "media";

QString dir();        // AppDataLocation/templates (own templates)
QString builtInRoot(); // ":/templates" (resources), or what setBuiltInRoot set
// Tests: another folder with built-in templates (empty = the resources again)
void setBuiltInRoot(const QString& root);

QVector<ContentTemplate> builtIn(); // by name
QVector<ContentTemplate> own();     // the folders in dir() with a template.schneidi, by name
QVector<ContentTemplate> all();     // built-in, then own
// By id (built-in ones first), then by name, any case; also a path to a template folder or its .schneidi file
std::optional<ContentTemplate> find(const QString& idOrNameOrPath, QString* error = nullptr);
// A template folder or its template.schneidi (any place, e.g. an unpacked ZIP)
std::optional<ContentTemplate> open(const QString& path, QString* error = nullptr);
// The template's project (media resolved in its folder); the project path for saving stays the caller's
bool load(const ContentTemplate& t, ProjectData* data, QString* error = nullptr);

// Folder name of an own template of this name (safe on every system, StartTemplates::idForName rules)
QString idForName(const QString& name);

struct SaveOptions {
    QString description;
    QStringList tags;
    QString aspect;          // empty = from the project format
    bool copyMedia = true;   // copy the used media into media/ (paths relative to the folder); false = they stay
                             // where they are (absolute; the template works only on this machine)
    QString preview;         // a file to copy as preview.mp4/png (empty = none)
    bool replace = false;    // replace an own template of the same id (else an error)
};
// Saves `project` as own template `name` (templateInfo set from name + options). Only used media are copied (all
// timelines); two files of the same name get distinct names in media/. Returns the saved template.
std::optional<ContentTemplate> save(const ProjectData& project, const QString& name, const SaveOptions& options = {},
                                    QString* error = nullptr);
// Own templates only: delete the folder; rename (the name in its project; the folder follows when it is free)
bool remove(const QString& id, QString* error = nullptr);
// Held while own templates change (save, remove, rename, import): two at once (two AIs saving the same name, a save
// and a delete) work one after the other instead of on each other's half-written folders. A lock file in dir(),
// stale only when its process is gone; nullptr = still taken after timeoutMs.
std::unique_ptr<QLockFile> lockLibrary(int timeoutMs = 60000);
bool rename(const QString& id, const QString& name, QString* error = nullptr);

// Every file a project refers to: the media of clips, path parameters of clip effects (LUTs), wipe images of
// transitions and the project brand kit's logo/intro/outro (built-in LUTs and wipes left out). `fn` may change the
// path in place.
void forEachFile(ProjectData& d, const std::function<void(QString& path)>& fn);

struct FileCopies {
    QHash<QString, QString> moved; // template file -> its copy
    QStringList written;           // copies written by this call (the caller removes them when it gives up)
};
// Makes a project made from the template in `templateDir` independent of that folder (a built-in one lives in the
// cache, an own one may be deleted or renamed): the files inside the folder the project uses (forEachFile; with
// `keepPool` also the Media Pool's) are copied into `targetDir` (their place in the template kept: "media/a.mp4" ->
// "<targetDir>/media/a.mp4") and the project points at the copies. Without `keepPool` files of the template that
// nothing uses leave the Media Pool. Never overwrites: a file of that name with other content stays and the copy
// gets a free name ("a-2.mp4"); one with the same content (size, then bytes) is used as it is. `known` (optional):
// copies of earlier calls (template file -> copy), used again without comparing while they lie in `targetDir` and
// their size matches (the rows of a batch share one folder). False + error when a copy fails; the copies written by
// this call are removed then (and `known` is as before the call).
bool copyTemplateFiles(ProjectData& d, const QString& templateDir, const QString& targetDir, FileCopies* out,
                       QString* error = nullptr, QHash<QString, QString>* known = nullptr, bool keepPool = false);
// The template folders (own ones in dir(), built-in ones in the cache) holding files the project uses (forEachFile) or
// has in its Media Pool, sorted: a project made from a template still points into them until a save copies the files
// (copyTemplateFiles), also when nothing remembers which template it came from (restored from an automatic backup)
QStringList foldersUsed(const ProjectData& d);

// "9:16" etc. for a size
QString aspectLabel(int width, int height);

} // namespace TemplateLibrary
