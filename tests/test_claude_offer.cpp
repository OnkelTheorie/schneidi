// SPDX-License-Identifier: AGPL-3.0-only
// The offer to connect Claude (app start question, terminal tip; cli/Setup.h): only when Claude Code or Desktop is
// there and schneidi is connected to neither; at most kOfferTimes times, kOfferDays apart; never after "don't ask
// again" (`setup claude --never`); not once the skill is installed or Claude Desktop has the MCP server.
#include "check.h"

#include "cli/Setup.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace {

void resetOffer()
{
    QSettings s;
    s.remove("claude/offerNever");
    s.remove("claude/offerCount");
    s.remove("claude/offerLast");
}

void writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(data);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("claude_offer");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    // Claude Code's folder (CLAUDE_CONFIG_DIR) in the temp dir; Claude Desktop's folder is in QStandardPaths' test paths
    const QString code = tmp.filePath("claude");
    QDir().mkpath(code);
    qputenv("CLAUDE_CONFIG_DIR", code.toUtf8());
    const QString desktop = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)).filePath("Claude");
    QDir(desktop).removeRecursively();
    resetOffer();

    CHECK(Cli::Setup::claudeFound().contains("Claude Code"));
    CHECK(!Cli::Setup::claudeFound().contains("Claude Desktop"));
    CHECK(!Cli::Setup::connected());
    CHECK(Cli::Setup::offerDue());

    // Asked: not again before two weeks
    Cli::Setup::offerShown();
    CHECK(!Cli::Setup::offerDue());
    const qint64 day = 24LL * 3600 * 1000;
    QSettings().setValue("claude/offerLast", QDateTime::currentMSecsSinceEpoch() - (Cli::Setup::kOfferDays - 1) * day);
    CHECK(!Cli::Setup::offerDue());
    QSettings().setValue("claude/offerLast", QDateTime::currentMSecsSinceEpoch() - (Cli::Setup::kOfferDays + 1) * day);
    CHECK(Cli::Setup::offerDue());
    // At most kOfferTimes times
    QSettings().setValue("claude/offerCount", Cli::Setup::kOfferTimes);
    CHECK(!Cli::Setup::offerDue());
    resetOffer();
    CHECK(Cli::Setup::offerDue());

    // "Don't ask again" (also `setup claude --never`)
    Cli::Setup::offerNever();
    CHECK(!Cli::Setup::offerDue());
    resetOffer();
    CHECK_EQ(Cli::Setup::run({"claude", "--never"}, false), 0);
    CHECK(!Cli::Setup::offerDue());
    CHECK_EQ(Cli::Setup::run({"claude", "--never", "--remove"}, false), 2); // --never stands alone
    resetOffer();

    // Connected through Claude Code (the skill is there): no offer
    writeFile(code + "/skills/schneidi/SKILL.md", "# schneidi\n");
    CHECK(Cli::Setup::connected());
    CHECK(!Cli::Setup::offerDue());
    QDir(code + "/skills").removeRecursively();
    CHECK(Cli::Setup::offerDue());

    // Claude Desktop with schneidi among its MCP servers: connected; with other servers only: not
    writeFile(desktop + "/claude_desktop_config.json", R"({"mcpServers":{"other":{"command":"x"}}})");
    CHECK(Cli::Setup::claudeFound().contains("Claude Desktop"));
    CHECK(!Cli::Setup::connected());
    writeFile(desktop + "/claude_desktop_config.json", R"({"mcpServers":{"schneidi":{"command":"x"}}})");
    CHECK(Cli::Setup::connected());
    CHECK(!Cli::Setup::offerDue());
    QDir(desktop).removeRecursively();
    resetOffer();
    return Check::result();
}
