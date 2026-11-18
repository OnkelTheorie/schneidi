// SPDX-License-Identifier: AGPL-3.0-only
// Out-of-line parts of check.h (linked into every test): keeps the engine headers out of check.h
#include "check.h"

#include "core/SettingsIsolation.h"
#include "engine/Engine.h"

#include <QDir>
#include <QFile>

#include <utility>

namespace Check {

void initEngine() { Engine::initHeadless(); }

namespace {

// Marks a settings folder the tests made: only such a folder is ever emptied
constexpr const char* kMarker = ".schneidi-test-settings";
// Set by the first process of a test: processes it starts (itself again, schneidi-cli) keep the folder and its
// XDG variables as they are
constexpr const char* kOwned = "SCHNEIDI_TEST_SETTINGS_READY";

// A temporary folder of a test started by hand: removed when the test ends
struct TempFolder {
    QString path;
    ~TempFolder()
    {
        if (!path.isEmpty() && QFileInfo::exists(QDir(path).filePath(kMarker))) QDir(path).removeRecursively();
    }
};

} // namespace

void isolateSettings()
{
    static bool done = false;
    if (done) return;
    done = true;
    if (!qEnvironmentVariableIsSet(kOwned)) {
        QString folder = qEnvironmentVariable(SettingsIsolation::kVariable);
        if (folder.isEmpty()) {
            static TempFolder temp;
            temp.path = QDir::temp().filePath(
                QString("schneidi-test-settings-%1").arg(QCoreApplication::applicationPid()));
            folder = temp.path;
            qputenv(SettingsIsolation::kVariable, QDir::toNativeSeparators(folder).toLocal8Bit());
        }
        QDir dir(folder);
        // Fresh every run: what an earlier run left (a default template, presets) must not change this one. Only the
        // subfolders the isolation itself makes are removed, never the given folder (a hand-set
        // SCHNEIDI_TEST_SETTINGS=$HOME must not lose anything else)
        if (dir.exists(kMarker))
            for (const char* sub : {"settings", "config", "data", "cache"}) QDir(dir.filePath(sub)).removeRecursively();
        dir.mkpath(".");
        QFile marker(dir.filePath(kMarker));
        if (marker.open(QIODevice::WriteOnly)) marker.close();
        // Set here, not only where unset: the user's own XDG variables (ctest inherits them) must not leak in. All
        // platforms: on Windows/macOS AppPaths::writable follows them in a test run (core/AppPaths.h)
        for (const auto& [name, sub] : {std::pair{"XDG_CONFIG_HOME", "config"}, std::pair{"XDG_DATA_HOME", "data"},
                                        std::pair{"XDG_CACHE_HOME", "cache"}})
            qputenv(name, QDir::toNativeSeparators(dir.absoluteFilePath(sub)).toLocal8Bit());
        qputenv(kOwned, "1");
    }
    SettingsIsolation::apply();
}

} // namespace Check
