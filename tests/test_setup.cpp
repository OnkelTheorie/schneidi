// The editing guide built into the program (cli/Guide.h, the repo's Claude skill) and `schneidi-cli setup claude`
// (cli/Setup.h): Claude Code gets MCP server + skill, Claude Desktop the MCP server in its config, other entries of
// that config stay, a broken config is left alone, --remove takes everything out again. Runs the real schneidi-cli
// in a fake home with a fake `claude` command that only logs its arguments.
#include "cli/Commands.h"
#include "cli/Guide.h"

#include "check.h"

#include <QCoreApplication>
#include <QDir>
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
        p.start(SCHNEIDI_CLI, QStringList{"setup"} + args);
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
        CHECK(calls.contains(QString("mcp add --scope user schneidi -- %1 mcp").arg(SCHNEIDI_CLI)));
        CHECK(QFile::exists(home + "/.claude/skills/schneidi/SKILL.md"));
        const QJsonObject config = QJsonDocument::fromJson(readFile(desktopConfig)).object();
        CHECK_EQ(config["globalShortcut"].toString(), QString("Ctrl+Space"));
        CHECK_EQ(config["mcpServers"]["other"]["command"].toString(), QString("x"));
        CHECK_EQ(config["mcpServers"]["schneidi"]["command"].toString(), QString(SCHNEIDI_CLI));
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
