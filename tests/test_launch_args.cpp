// SPDX-License-Identifier: AGPL-3.0-only
// The app's command line: options never end up imported as files. Qt's X11 -geometry stayed in arguments() and was
// imported as two unreadable files; the modal "not readable" box blocked a test run (screenshot never taken).
// The restart after a design/language change reopens its project in a window of its own: it was handed over to
// another schneidi that runs (that one replaced its project, the restarted window was gone).

#include "check.h"

#include "app/LaunchArgs.h"

#include <QCoreApplication>

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    using LaunchArgs::files;
    CHECK_EQ(files({"-geometry", "1600x900+160+90", "--demo", "/v/a.mkv", "--actions", "play_pause", "--screenshot",
                    "/tmp/x.png", "--wait", "6000", "--size", "1600x900", "--verbose"}),
             QStringList{"/v/a.mkv"});
    CHECK_EQ(files({"/p/x.schneidi"}), QStringList{"/p/x.schneidi"});
    CHECK_EQ(files({"--display", ":1", "-reverse", "a.mp4", "-style=fusion", "b.mp4", "-qwindowgeometry", "10x10"}),
             (QStringList{"a.mp4", "b.mp4"}));
    CHECK_EQ(files({"--lang", "en", "--format", "1080x1920@30", "--storage", "/s", "--design", "dark", "c.mov"}),
             QStringList{"c.mov"});
    // Which launches offer their files to a running app first
    using LaunchArgs::handsOver;
    CHECK(handsOver({"/v/a.mkv"}));
    CHECK(handsOver({"--open", "/p/x.schneidi"}));
    CHECK(!handsOver({}));
    CHECK(!handsOver({"--verbose"}));
    CHECK(!handsOver({"--new-instance", "/v/a.mkv"}));
    CHECK(!handsOver({"--demo", "/v/a.mkv"}));
    CHECK(!handsOver({"--actions", "play_pause", "/v/a.mkv"}));
    // The restart: its project again, never handed over
    CHECK_EQ(files(LaunchArgs::restart("/p/x.schneidi")), QStringList{"/p/x.schneidi"});
    CHECK(!handsOver(LaunchArgs::restart("/p/x.schneidi")));
    CHECK(files(LaunchArgs::restart({})).isEmpty());
    return Check::result();
}
