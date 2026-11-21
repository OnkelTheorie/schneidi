// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Help › Report a bug…: opens a new GitHub issue (form .github/ISSUE_TEMPLATE/bug_report.yml) in the browser with
// version, system, GPU, UI language and where the log file is already filled in. Nothing is sent by schneidi: the user
// sees every field on GitHub and submits it there. No user data beyond that (the home folder in the log path becomes
// ~ or %USERPROFILE%, no project paths, no user name).

#include <QString>
#include <QUrl>

namespace BugReport {

struct Facts {
    QString version;  // "0.25.0 (abc1234)" (QCoreApplication::applicationVersion)
    QString os;       // "Debian GNU/Linux 13 (trixie)"
    QString kernel;   // "linux 6.12.117"
    QString cpuArch;  // "x86_64"
    QString gpu;      // driver's device name, empty if unknown
    QString gpuApi;   // "Vulkan", "Direct3D 11", ...
    QString language; // UI language code "de"
    QString logDir;   // log folder with the home folder replaced (shortHome)
};

// The facts of this machine (GPU only if the picture engine probed it already or may: Picture::Gpu::probe is cached)
Facts collect();
// path with the home folder replaced by "~" (Windows: "%USERPROFILE%"); separators as the system shows them
QString shortHome(const QString& path, const QString& home);
// https://github.com/OnkelTheorie/schneidi/issues/new?template=bug_report.yml&version=…&system=…&log=… (under 6000
// characters: long values are cut)
QUrl issueUrl(const Facts& f);

} // namespace BugReport
