// SPDX-License-Identifier: AGPL-3.0-only
#include "app/LaunchArgs.h"

namespace LaunchArgs {

QStringList files(const QStringList& args)
{
    // Qt's (and X11 toolkit) options with a value; a single or double dash both work there
    static const QStringList qtValue = {"geometry", "display", "title", "name", "platform", "platformpluginpath",
                                        "platformtheme", "plugin", "qwindowgeometry", "qwindowtitle", "qwindowicon",
                                        "style", "stylesheet", "session", "visual", "ncols", "cmap", "fn", "font",
                                        "bg", "background", "fg", "foreground", "btn", "button", "im", "inputstyle",
                                        "qmljsdebugger"};
    QStringList out;
    for (int i = 0; i < args.size(); ++i) {
        const QString& a = args[i];
        if (valueOptions().contains(a)) {
            ++i;
            continue;
        }
        if (a.startsWith('-') && a.size() > 1) {
            QString name = a.mid(a.startsWith(QLatin1String("--")) ? 2 : 1);
            if (!name.contains('=') && qtValue.contains(name)) ++i; // its value too
            continue; // flags (--demo, --verbose, -reverse, -sync ...) are no files
        }
        out << a;
    }
    return out;
}

bool handsOver(const QStringList& args)
{
    return !files(args).isEmpty() && !args.contains("--demo") && !args.contains("--actions")
           && !args.contains("--new-instance");
}

QStringList restart(const QString& projectPath)
{
    QStringList args{"--new-instance"};
    if (!projectPath.isEmpty()) args << projectPath;
    return args;
}

Info info(const QStringList& args)
{
    for (const QString& a : args) {
        if (a == QLatin1String("--")) break; // file names after it
        if (a == QLatin1String("--help") || a == QLatin1String("-h") || a == QLatin1String("-?")) return Info::Help;
        if (a == QLatin1String("--version") || a == QLatin1String("-v")) return Info::Version;
    }
    return Info::None;
}

QString helpText(const QString& program, const QString& version)
{
    return QString("schneidi %1 - video editor\n"
                   "\n"
                   "Usage: %2 [options] [files or folders ...]\n"
                   "\n"
                   "Files are imported (a .schneidi project is opened); with schneidi already running they go to it.\n"
                   "\n"
                   "Options:\n"
                   "  -h, --help          show this help and quit\n"
                   "  -v, --version       show the version and quit\n"
                   "  --open <file>       open or import a file (same as giving it alone)\n"
                   "  --new-instance      open a window of its own even when schneidi already runs\n"
                   "  --lang <code>       user interface language for this run (en, de, fr, ...)\n"
                   "  --design <name>     colour design for this run\n"
                   "  --verbose           also log FFmpeg warnings\n"
                   "\n"
                   "Editing from scripts and AIs: schneidi-cli --help\n")
        .arg(version, program);
}

} // namespace LaunchArgs
