// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Work folder of schneidi-cli (`serve`, `mcp --root`): every path a client gives goes through resolve() (the
// path parameters of the command table and the files inside edit ops, both via detail::absolute()). Relative paths
// start in the work folder, anything outside it is refused (OUTSIDE_ROOT), symlinks included. Path mapping
// (`--map 'Z:\Clips=/data/clips'`) rewrites paths of another computer to this one's and back in the answers.
// Without a root (and maps) nothing changes: relative paths start in the working directory, as before.

#include <QJsonValue>
#include <QString>
#include <QStringList>

class Project;
struct ProjectData;

namespace Cli::Sandbox {

struct Settings {
    QString root;      // work folder; empty = the whole file system
    QStringList maps;  // "client prefix=server prefix"
    bool app = true;   // live connection to the desktop app ("@app", `app`); off with a root (the app does not
                       // know the work folder)
};

// Throws Error (BAD_ARGUMENT) for a root that is not a folder or a map without '='
void configure(const Settings& s);
bool active();       // a root is set
QString root();      // absolute, clean
bool appAllowed();

// A path from a client -> absolute path on this computer: mapped, relative ones in the root (or the working
// directory). Throws OUTSIDE_ROOT if it leaves the root, also through a symlink, HIDDEN for a hidden file or folder in
// it (or something inside one); "@app" stays (APP_OFF if not allowed)
QString resolve(const QString& path);
// Only the path mapping (--map) of a client's path, nothing resolved or checked; unmapped paths stay as given
QString mapPath(const QString& path);
// Path relative to the root ("" = the root itself); null if outside (or no root)
QString relative(const QString& absolutePath);
// A hidden file or folder in the work folder, or something in one (".ssh/id_rsa", ".config/…"): the server's own file
// access (/files, `files`, `upload`) leaves them out, as its listings do
bool hidden(const QString& absolutePath);
// With a root: an existing file that makes MLT or FFmpeg read other files or addresses when opened as media (MLT XML,
// .kdenlive, m3u/HLS playlists, ffconcat, SDP, DASH manifests), by name or content; a path inside the root would
// otherwise read anything. checkMedia() throws BAD_MEDIA for one. Only a clear early answer for the known kinds: the
// engine itself never opens a demuxer that follows names in the file, whatever it is called (engine/OpenMedia.h).
bool namesOtherFiles(const QString& absolutePath);
void checkMedia(const QString& absolutePath);
// Answers: paths of this computer back to the client's (reverse of the maps); strings in objects/arrays too
QString mapBack(const QString& path);
QJsonValue mapBack(const QJsonValue& v);
// A project file names media, LUTs, transition images, files of compositing nodes, texture pictures, brand kit files
// and watch folders (core/ProjectRefs.h, the one list): refuse any outside the root (or the server's own effects
// folder), also missing ones (a project put into the work folder must not read other files of the server; the engine
// reads more than plain paths). Also the server's own global brand kit's files: a template saved from the project
// would copy them. No root: nothing to check.
void checkProject(const Project& p);
void checkProject(const ProjectData& d);
// A file a project may name here (what checkProject accepts): always true without a root
bool projectFileAllowed(const QString& path);

} // namespace Cli::Sandbox
