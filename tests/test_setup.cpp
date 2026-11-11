// SPDX-License-Identifier: AGPL-3.0-only
// The editing guide built into the program (cli/Guide.h, the repo's Claude skill) and `schneidi-cli setup claude`
// (cli/Setup.h): Claude Code gets MCP server + skill, Claude Desktop the MCP server in its config, other entries of
// that config stay, a broken config is left alone, --remove takes everything out again. Runs the real schneidi-cli
// in a fake home with a fake `claude` command that only logs its arguments.
#include "cli/Commands.h"
#include "cli/Guide.h"

#include "check.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>

namespace {

struct Run {
    int exit = -1;
    QJsonObject out;
};

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(data);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // --- The guide: main text without front matter, topics with titles, no way out of the resources
    {
        const QStringList topics = Cli::Guide::topics();
        CHECK(topics.contains("ad"));
        CHECK(topics.contains("highlights"));
        CHECK(topics.contains("check"));
        CHECK(Cli::Guide::text().startsWith("# Editing video with schneidi"));
        CHECK(!Cli::Guide::text().contains("description:"));
        CHECK_EQ(Cli::Guide::title("ad"), QString("Ad, product or marketing clip"));
        for (const QString& t : topics) CHECK(!Cli::Guide::text(t).isEmpty());
        CHECK(Cli::Guide::text("../SKILL").isEmpty());
        CHECK(Cli::Guide::text("nope").isEmpty());

        // The `guide` command: list of topics, one topic, unknown topic = NOT_FOUND with the list
        const Cli::Command* guide = Cli::find("guide");
        if (CHECK(guide)) {
            Cli::Context ctx;
            const QJsonObject all = Cli::execute(*guide, Cli::normalize(*guide, {}), ctx);
            CHECK_EQ(all["topics"].toArray().size(), topics.size());
            CHECK(all["guide"].toString().contains("## How to work"));
            const QJsonObject one = Cli::execute(*guide, Cli::normalize(*guide, {{"topic", "references/ad.md"}}), ctx);
            CHECK_EQ(one["topic"].toString(), QString("ad"));
            CHECK(one["guide"].toString().startsWith("# Ad,"));
            QString code;
            try {
                Cli::execute(*guide, Cli::normalize(*guide, {{"topic", "nope"}}), ctx);
            } catch (const Cli::Error& e) {
                code = e.code;
                CHECK(e.message.contains("highlights"));
            }
            CHECK_EQ(code, QString("NOT_FOUND"));
        }

        // As files: the skill with front matter and its references
        QTemporaryDir dir;
        QString error;
        CHECK(Cli::Guide::installSkill(dir.filePath("schneidi"), &error));
        CHECK(readFile(dir.filePath("schneidi/SKILL.md")).startsWith("---\nname: schneidi"));
        CHECK(QFile::exists(dir.filePath("schneidi/references/ad.md")));
    }

    // --- Option names are normalised (--dry-run = --dry_run), their values are not: `new --media=<path>` turned the
    // hyphens of the path into underscores (report 9.11.), the project file too
    {
        QTemporaryDir dir;
        const QString media = Check::makeMedia(dir.filePath("my-clip-2026-11-03.mkv"),
                                               {"-f", "lavfi", "-i", "color=c=gray:s=160x90:d=1:r=25", "-c:v", "ffv1"});
        const QString project = dir.filePath("my-cut.schneidi");
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("QT_QPA_PLATFORM", "offscreen");
        p.setProcessEnvironment(env);
        p.start(Check::cliPath(), {"new", "--project=" + project, "--media=" + media});
        CHECK(p.waitForFinished(60000));
        CHECK_EQ(p.exitCode(), 0);
        CHECK(QFile::exists(project));
        CHECK(!QFile::exists(dir.filePath("my_cut.schneidi")));
        CHECK(readFile(project).contains(QFileInfo(media).fileName().toUtf8()));
    }

    // --- The stdio MCP server of a running chat after an update: the program file on disk is not the one running.
    // Every tool result then says so (restart the MCP server / the chat) instead of answering silently with the old
    // build (report 9.11.)
    {
        QTemporaryDir dir;
        const QString exe = dir.filePath(QFileInfo(Check::cliPath()).fileName());
        CHECK(QFile::copy(Check::cliPath(), exe));
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("QT_QPA_PLATFORM", "offscreen");
        env.remove("APPIMAGE");
        // The copy finds its libraries where the build put them (Windows looks for DLLs along PATH)
        env.insert("PATH", QFileInfo(Check::cliPath()).absolutePath() + QDir::listSeparator() + env.value("PATH"));
        p.setProcessEnvironment(env);
        p.start(exe, {"mcp"});
        CHECK(p.waitForStarted(10000));
        int id = 0;
        const auto call = [&](const QJsonObject& msg) {
            QJsonObject m = msg;
            m["jsonrpc"] = "2.0";
            m["id"] = ++id;
            p.write(QJsonDocument(m).toJson(QJsonDocument::Compact) + '\n');
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < 30000) {
                if (!p.canReadLine() && !p.waitForReadyRead(1000)) continue;
                while (p.canReadLine()) {
                    const QJsonObject o = QJsonDocument::fromJson(p.readLine()).object();
                    if (o["id"].toInt() == id) return o;
                }
            }
            return QJsonObject();
        };
        const auto texts = [](const QJsonObject& answer) {
            QString all;
            for (const QJsonValue& c : answer["result"]["content"].toArray()) all += c["text"].toString() + "\n";
            return all;
        };
        call({{"method", "initialize"},
              {"params", QJsonObject{{"protocolVersion", "2025-03-26"}, {"capabilities", QJsonObject{}},
                                     {"clientInfo", QJsonObject{{"name", "test"}, {"version", "1"}}}}}});
        const QJsonObject guideCall{{"method", "tools/call"},
                                    {"params", QJsonObject{{"name", "guide"}, {"arguments", QJsonObject{}}}}};
        const QString before = texts(call(guideCall));
        CHECK(before.contains("topics"));
        CHECK(!before.contains("was updated"));
        // An update replaces the file (the new one renamed into its place, as installers and package managers do)
        const QString fresh = exe + ".new";
        CHECK(QFile::copy(Check::cliPath(), fresh));
        QFile f(fresh);
        if (CHECK(f.open(QIODevice::Append))) f.write("x"); // another build: another size
        f.close();
        // (the running file is moved aside, not removed: Windows cannot delete a running program, only rename it)
        CHECK(QFile::rename(exe, exe + ".old"));
        CHECK(QFile::rename(fresh, exe));
        const QString after = texts(call(guideCall));
        CHECK(after.contains("topics")); // the tool still answers
        CHECK(after.contains("schneidi was updated"));
        CHECK(after.contains("restart"));
        p.closeWriteChannel();
        if (!p.waitForFinished(10000)) p.kill();
    }

#ifdef Q_OS_WIN
    return Check::skip("the fake `claude` below is a shell script");
#endif

    QTemporaryDir tmp;
    const QString home = tmp.filePath("home"), bin = tmp.filePath("bin");
    const QString log = bin + "/calls.log";
    const QString desktopConfig = home + "/.config/Claude/claude_desktop_config.json";
    writeFile(bin + "/claude", "#!/bin/sh\necho \"$@\" >> \"$(dirname \"$0\")/calls.log\"\n");
    QFile(bin + "/claude").setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QDir().mkpath(home);

    const auto setup = [&](const QStringList& args, bool withClaude) {
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("HOME", home);
        env.insert("XDG_CONFIG_HOME", home + "/.config");
        env.insert("XDG_DATA_HOME", home + "/.local/share");
        env.insert("PATH", withClaude ? bin + ":/usr/bin:/bin" : QString("/usr/bin:/bin"));
        env.remove("CLAUDE_CONFIG_DIR");
        env.remove("APPIMAGE");
        p.setProcessEnvironment(env);
        p.start(Check::cliPath(), QStringList{"setup"} + args);
        Run r;
        if (!p.waitForFinished(30000)) return r;
        r.exit = p.exitCode();
        r.out = QJsonDocument::fromJson(p.readAllStandardOutput()).object();
        return r;
    };

    // Nothing of Claude on this computer: nothing written, a clear message
    {
        const Run r = setup({"claude"}, false);
        CHECK_EQ(r.exit, 1);
        CHECK_EQ(r.out["claude_code"]["state"].toString(), QString("skipped"));
        CHECK_EQ(r.out["claude_desktop"]["state"].toString(), QString("skipped"));
        CHECK(!QFile::exists(home + "/.claude"));
    }

    // Both there: MCP server via `claude mcp add` (an older entry removed first), skill, Desktop config merged
    writeFile(desktopConfig, R"({"globalShortcut":"Ctrl+Space","mcpServers":{"other":{"command":"x"}}})");
    {
        const Run r = setup({"claude"}, true);
        CHECK_EQ(r.exit, 0);
        CHECK_EQ(r.out["claude_code"]["state"].toString(), QString("done"));
        CHECK_EQ(r.out["claude_desktop"]["state"].toString(), QString("done"));
        const QString calls = QString::fromUtf8(readFile(log));
        CHECK(calls.contains("mcp remove --scope user schneidi"));
        CHECK(calls.contains(QString("mcp add --scope user schneidi -- %1 mcp").arg(Check::cliPath())));
        CHECK(QFile::exists(home + "/.claude/skills/schneidi/SKILL.md"));
        const QJsonObject config = QJsonDocument::fromJson(readFile(desktopConfig)).object();
        CHECK_EQ(config["globalShortcut"].toString(), QString("Ctrl+Space"));
        CHECK_EQ(config["mcpServers"]["other"]["command"].toString(), QString("x"));
        CHECK_EQ(config["mcpServers"]["schneidi"]["command"].toString(), Check::cliPath());
        CHECK_EQ(config["mcpServers"]["schneidi"]["args"].toArray(), QJsonArray({"mcp"}));
    }

    // --remove: entry, skill and Desktop entry gone, the rest of the Desktop config stays
    {
        QFile::remove(log);
        const Run r = setup({"claude", "--remove"}, true);
        CHECK_EQ(r.exit, 0);
        CHECK(QString::fromUtf8(readFile(log)).contains("mcp remove --scope user schneidi"));
        CHECK(!QString::fromUtf8(readFile(log)).contains("mcp add"));
        CHECK(!QFile::exists(home + "/.claude/skills/schneidi"));
        const QJsonObject config = QJsonDocument::fromJson(readFile(desktopConfig)).object();
        CHECK(!config["mcpServers"].toObject().contains("schneidi"));
        CHECK(config["mcpServers"].toObject().contains("other"));
    }

    // Claude Code without its command (only ~/.claude): skill yes, the MCP command to run is in the message
    {
        const Run r = setup({"claude", "--only", "code"}, false);
        CHECK_EQ(r.exit, 0);
        CHECK_EQ(r.out["claude_code"]["state"].toString(), QString("partly"));
        CHECK(r.out["claude_code"]["message"].toString().contains("claude mcp add --scope user schneidi --"));
        CHECK(QFile::exists(home + "/.claude/skills/schneidi/SKILL.md"));
        CHECK(!r.out.contains("claude_desktop"));
    }

    // `claude` only in ~/.local/bin, where its installer puts it (an app started from the desktop lacks it in PATH)
    {
        QDir().mkpath(home + "/.local/bin");
        writeFile(home + "/.local/bin/claude", "#!/bin/sh\necho \"$@\" >> \"$(dirname \"$0\")/calls.log\"\n");
        QFile(home + "/.local/bin/claude").setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        const Run r = setup({"claude", "--only", "code"}, false);
        CHECK_EQ(r.exit, 0);
        CHECK_EQ(r.out["claude_code"]["state"].toString(), QString("done"));
        CHECK(QString::fromUtf8(readFile(home + "/.local/bin/calls.log")).contains("mcp add --scope user schneidi"));
        QFile::remove(home + "/.local/bin/claude");
    }

    // A broken Desktop config is never overwritten
    {
        writeFile(desktopConfig, "{ \"mcpServers\": ");
        const Run r = setup({"claude", "--only", "desktop"}, true);
        CHECK_EQ(r.exit, 1);
        CHECK_EQ(r.out["claude_desktop"]["state"].toString(), QString("failed"));
        CHECK_EQ(readFile(desktopConfig), QByteArray("{ \"mcpServers\": "));
    }

    // Usage errors
    CHECK_EQ(setup({"chatgpt"}, true).exit, 2);
    CHECK_EQ(setup({"claude", "--only", "web"}, true).exit, 2);

    return Check::result();
}
