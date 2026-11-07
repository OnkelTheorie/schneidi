// SPDX-License-Identifier: AGPL-3.0-only
// Test runs never use the user's settings (core/SettingsIsolation.h): SCHNEIDI_TEST_SETTINGS sends the settings of
// this test and of every schneidi-cli it starts to an INI file in the test's folder. A default start template and
// import presets written there reach a spawned `schneidi-cli new` + `info`; a fresh folder gives the neutral default;
// the user's real settings file is never written.

#include "check.h"

#include "core/SettingsIsolation.h"
#include "core/StartTemplates.h"
#include "core/StreamPresets.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>

namespace {

QJsonObject cli(const QStringList& args, const QProcessEnvironment& env)
{
    QProcess p;
    p.setProcessEnvironment(env);
    p.start(Check::cliPath(), args);
    p.closeWriteChannel();
    if (!p.waitForFinished(60000)) std::printf("       timeout: schneidi-cli %s\n", qPrintable(args.join(' ')));
    const QByteArray out = p.readAllStandardOutput();
    const QJsonObject o = QJsonDocument::fromJson(out).object();
    if (!o["ok"].toBool()) std::printf("       schneidi-cli %s: %s %s\n", qPrintable(args.join(' ')), out.constData(),
                                       p.readAllStandardError().constData());
    return o;
}

// The project a spawned schneidi-cli makes with the default start template, as `info` shows it
QJsonObject newProject(const QString& path, const QProcessEnvironment& env)
{
    cli({"new", path}, env);
    return cli({"info", path}, env);
}

QString inside(const QString& path, const QString& folder)
{
    return QDir::cleanPath(path).startsWith(QDir::cleanPath(folder) + '/') ? QString("yes") : path;
}

} // namespace

int main(int argc, char* argv[])
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    // The real program's names: without the isolation this would be the user's own settings
    QGuiApplication::setApplicationName("schneidi");
#ifdef Q_OS_WIN
    QGuiApplication::setOrganizationName("schneidi");
#endif
    if (Check::cliPath().isEmpty()) return Check::skip("schneidi-cli unknown");

    const QString own = qEnvironmentVariable(SettingsIsolation::kVariable);
    if (!CHECK(!own.isEmpty())) return Check::result();
    CHECK_EQ(QSettings().format(), QSettings::IniFormat);
    CHECK_EQ(inside(QSettings().fileName(), own), QString("yes"));
#if !defined(Q_OS_WIN) && !defined(Q_OS_DARWIN)
    CHECK_EQ(inside(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation), own), QString("yes"));
    CHECK_EQ(inside(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation), own), QString("yes"));
    CHECK_EQ(inside(QStandardPaths::writableLocation(QStandardPaths::CacheLocation), own), QString("yes"));
#else
    CHECK(QStandardPaths::isTestModeEnabled()); // XDG does nothing there: Qt's test folders, never the real AppData
#endif
#ifdef Q_OS_LINUX
    // The user's real settings file (src/main.cpp: no organization on Linux): only its metadata, never its content
    const QFileInfo real(QDir::homePath() + "/.config/Unknown Organization/schneidi.conf");
    const bool realExisted = real.exists();
    const QDateTime realTime = real.lastModified();
    const qint64 realSize = real.size();
    CHECK(QSettings().fileName() != real.absoluteFilePath());
#endif

    // Settings of this test: the "game" start template by default (it takes the import presets), a rule muting the
    // stream "Voice"
    StartTemplates::setDefaultId("game");
    StreamPresets presets;
    presets.rules << StreamRule{"Voice", StreamRule::Action::Mute, 0.0, {}};
    presets.save();
    CHECK(QFileInfo::exists(QSettings().fileName()));

    QTemporaryDir tmp;
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    {
        const QJsonObject info = newProject(tmp.filePath("with.schneidi"), env);
        const QJsonObject import = info["import_presets"].toObject();
        CHECK(import["mute_mix"].toBool());
        const QJsonArray rules = import["rules"].toArray();
        CHECK_EQ(rules.size(), 1);
        CHECK_EQ(rules.first()["title"].toString(), QString("Voice"));
        CHECK_EQ(rules.first()["action"].toString(), QString("mute"));
    }
    // Another (empty) folder: the neutral default, nothing of the settings above
    {
        QProcessEnvironment fresh = env;
        fresh.insert(SettingsIsolation::kVariable, QDir::toNativeSeparators(tmp.filePath("fresh")));
        const QJsonObject info = newProject(tmp.filePath("fresh.schneidi"), fresh);
        CHECK(!info.isEmpty());
        CHECK(!info.contains("import_presets"));
    }

#ifdef Q_OS_LINUX
    const QFileInfo after(real.absoluteFilePath());
    CHECK_EQ(after.exists(), realExisted);
    CHECK_EQ(after.lastModified(), realTime);
    CHECK_EQ(after.size(), realSize);
#endif
    return Check::result();
}
