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

} // namespace LaunchArgs
