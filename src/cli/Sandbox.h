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
// directory). Throws OUTSIDE_ROOT if it leaves the root, also through a symlink; "@app" stays (APP_OFF if not allowed)
QString resolve(const QString& path);
// Path relative to the root ("" = the root itself); null if outside (or no root)
QString relative(const QString& absolutePath);
// Answers: paths of this computer back to the client's (reverse of the maps); strings in objects/arrays too
QString mapBack(const QString& path);
QJsonValue mapBack(const QJsonValue& v);
// A project file names media, LUTs and transition images: refuse existing ones outside the root (a project put
// into the work folder must not read other files of the server). Missing files are fine (offline media).
void checkProject(const Project& p);

} // namespace Cli::Sandbox
