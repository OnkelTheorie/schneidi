// SPDX-License-Identifier: AGPL-3.0-only
#include "cli/Setup.h"

#include "cli/Guide.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>

#include <cstdio>

namespace Cli::Setup {

namespace {

const QString kName = QStringLiteral("schneidi");

QJsonObject state(const QString& s, const QString& message) { return {{"state", s}, {"message", message}}; }

QString claudeCodeDir()
{
    const QString env = qEnvironmentVariable("CLAUDE_CONFIG_DIR");
    return env.isEmpty() ? QDir::home().filePath(".claude") : env;
}

QString claudeDesktopDir()
{
#if defined(Q_OS_WIN)
    return QDir(qEnvironmentVariable("APPDATA")).filePath("Claude");
#elif defined(Q_OS_MACOS)
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath("Claude");
#else
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)).filePath("Claude");
#endif
}

// Claude Code's installer puts `claude` into ~/.local/bin (older ones: ~/.claude/local), which the PATH of an app
// started from the desktop often lacks
QString findClaude()
{
    if (const QString exe = QStandardPaths::findExecutable("claude"); !exe.isEmpty()) return exe;
    return QStandardPaths::findExecutable("claude", {QDir::home().filePath(".local/bin"), QDir::home().filePath(".claude/local")});
}

// Runs `claude …`; false with the output as error when it fails
bool runClaude(const QString& exe, const QStringList& args, QString* output)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(exe, args);
    const bool finished = p.waitForStarted(10000) && p.waitForFinished(60000);
    if (output) *output = QString::fromUtf8(p.readAll()).trimmed();
    if (!finished) p.kill();
    return finished && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

QJsonObject setupCode(bool remove)
{
    const QString exe = findClaude();
    const QString dir = claudeCodeDir();
    if (exe.isEmpty() && !QFileInfo(dir).isDir()) return state("skipped", "Claude Code not found");
    const QString skillDir = QDir(dir).filePath("skills/" + kName);
    QJsonObject out;
    QStringList notes;
    bool ok = true;

    if (remove) {
        if (QFileInfo(skillDir).isDir() && !QDir(skillDir).removeRecursively()) {
            ok = false;
            notes << "cannot delete " + QDir::toNativeSeparators(skillDir);
        }
    } else {
        QString error;
        if (Guide::installSkill(skillDir, &error)) {
            out["skill"] = QDir::toNativeSeparators(skillDir);
        } else {
            ok = false;
            notes << error;
        }
    }

    const QStringList server = serverCommand();
    if (exe.isEmpty()) {
        // Without the claude command the user's MCP list cannot be changed safely (it lives in a big shared file)
        notes << (remove ? QString("remove the MCP server yourself: claude mcp remove --scope user %1").arg(kName)
                         : QString("claude command not found; add the MCP server yourself: claude mcp add --scope user "
                                   "%1 -- %2").arg(kName, server.join(' ')));
        return {{"state", ok ? "partly" : "failed"}, {"message", notes.join("; ")}, {"skill", out["skill"]}};
    }
    QString output;
    // Replace an older entry (another installation or version), so the server is always this one
    runClaude(exe, {"mcp", "remove", "--scope", "user", kName}, &output);
    if (!remove) {
        if (runClaude(exe, QStringList{"mcp", "add", "--scope", "user", kName, "--"} + server, &output)) {
            out["mcp"] = server.join(' ');
        } else {
            ok = false;
            notes << "claude mcp add failed: " + output;
        }
    }
    out["state"] = ok ? "done" : "failed";
    out["message"] = ok ? (remove ? QString("removed (MCP server and skill)")
                                  : QString("MCP server and skill installed; new Claude Code sessions have them"))
                        : notes.join("; ");
    return out;
}

QJsonObject setupDesktop(bool remove)
{
    const QString dir = claudeDesktopDir();
    if (!QFileInfo(dir).isDir()) return state("skipped", "Claude Desktop not found");
    const QString file = QDir(dir).filePath("claude_desktop_config.json");
    QJsonObject config;
    if (QFile f(file); f.exists()) {
        if (!f.open(QIODevice::ReadOnly)) return state("failed", "cannot read " + QDir::toNativeSeparators(file));
        const QByteArray data = f.readAll();
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
        if (!data.trimmed().isEmpty() && !doc.isObject())
            return state("failed", QDir::toNativeSeparators(file) + " is not valid JSON (" + err.errorString()
                                       + "); left unchanged");
        config = doc.object();
    }
    QJsonObject servers = config["mcpServers"].toObject();
    if (remove) {
        if (!servers.contains(kName)) return state("done", "was not set up");
        servers.remove(kName);
    } else {
        QStringList server = serverCommand();
        const QString program = server.takeFirst();
        servers[kName] = QJsonObject{{"command", program}, {"args", QJsonArray::fromStringList(server)}};
    }
    config["mcpServers"] = servers;
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly) || out.write(QJsonDocument(config).toJson(QJsonDocument::Indented)) < 0
        || !out.commit())
        return state("failed", "cannot write " + QDir::toNativeSeparators(file));
    QJsonObject result = state("done", remove ? "removed; restart Claude Desktop"
                                              : "MCP server added; restart Claude Desktop (the AI reads the "
                                                "editing guide through the `guide` tool)");
    result["config"] = QDir::toNativeSeparators(file);
    return result;
}

void print(const QJsonObject& o, bool pretty)
{
    const QByteArray out = QJsonDocument(o).toJson(pretty ? QJsonDocument::Indented : QJsonDocument::Compact);
    std::fwrite(out.constData(), 1, size_t(out.size()), stdout);
    if (!pretty) std::fputc('\n', stdout);
    std::fflush(stdout);
}

} // namespace

QStringList serverCommand()
{
    // An AppImage runs from a temporary mount: only the image itself stays valid
    if (const QString image = qEnvironmentVariable("APPIMAGE"); !image.isEmpty()) return {image, "cli", "mcp"};
    QString exe = QCoreApplication::applicationFilePath();
    if (QFileInfo(exe).completeBaseName() != "schneidi-cli") { // called from the app: its schneidi-cli next to it
#ifdef Q_OS_WIN
        exe = QDir(QCoreApplication::applicationDirPath()).filePath("schneidi-cli.exe");
#else
        exe = QDir(QCoreApplication::applicationDirPath()).filePath("schneidi-cli");
#endif
    }
    return {QDir::toNativeSeparators(exe), "mcp"};
}

QJsonObject claude(const Options& options)
{
    QJsonObject out;
    if (options.code) out["claude_code"] = setupCode(options.remove);
    if (options.desktop) out["claude_desktop"] = setupDesktop(options.remove);
    return out;
}

int run(QStringList args, bool pretty)
{
    const auto usage = [pretty](const QString& message) {
        print({{"ok", false}, {"error", QJsonObject{{"code", "USAGE"}, {"message", message}}}}, pretty);
        return 2;
    };
    if (args.isEmpty() || args.takeFirst() != "claude")
        return usage("schneidi-cli setup claude [--only code|desktop] [--remove]: connects Claude Code and Claude "
                     "Desktop to schneidi (MCP server + editing guide)");
    Options o;
    o.remove = args.removeAll("--remove") > 0;
    if (const qsizetype i = args.indexOf("--only"); i >= 0) {
        const QString only = args.value(i + 1);
        if (only != "code" && only != "desktop") return usage("--only takes code or desktop");
        o.code = only == "code";
        o.desktop = only == "desktop";
        args.remove(i, 2);
    }
    if (!args.isEmpty()) return usage("unexpected argument: " + args.first());
    QJsonObject result = claude(o);
    bool failed = false, any = false;
    for (const QJsonValue& v : result) {
        failed |= v["state"].toString() == "failed";
        any |= v["state"].toString() != "skipped";
    }
    if (!any) {
        result["message"] = "neither Claude Code nor Claude Desktop was found on this computer";
        failed = true;
    }
    result["ok"] = !failed;
    print(result, pretty);
    return failed ? 1 : 0;
}

} // namespace Cli::Setup
