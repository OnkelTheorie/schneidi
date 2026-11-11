// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// `schneidi-cli setup claude`: one command connects this installation to Claude. Claude Code gets the MCP server
// (user scope, via its own `claude mcp add`) and the skill (cli/Guide.h) in its skills folder; Claude Desktop gets
// the MCP server in claude_desktop_config.json (it reads the guide through the `guide` tool). Always the absolute
// path of this installation, so it works without schneidi-cli on PATH (AppImage, Windows). Not an MCP tool: an AI
// must not change which tools it has.

#include <QJsonObject>
#include <QStringList>

namespace Cli::Setup {

struct Options {
    bool code = true;    // Claude Code
    bool desktop = true; // Claude Desktop
    bool remove = false; // take it out again
};

// Per target {"state": "done" | "partly" | "skipped" | "failed", "message": …}: {"claude_code": …, "claude_desktop": …}
QJsonObject claude(const Options& options);

// Program and arguments that start this installation's MCP server (AppImage: the image with "cli mcp")
QStringList serverCommand();

// Command line `schneidi-cli setup <target> [--only code|desktop] [--remove] [--never]`; prints JSON, returns the
// exit code. --never only stops the offer below.
int run(QStringList args, bool pretty);

// The offer to connect (asked once on app start, a tip in an interactive terminal): Claude Code or Claude Desktop is
// on this computer, but schneidi is connected to neither. Settings "claude/offer*" (shared by app and CLI): never
// again after "don't ask again", at most kOfferTimes times, at least kOfferDays days apart.
inline constexpr int kOfferTimes = 3, kOfferDays = 14;
QStringList claudeFound(); // "Claude Code", "Claude Desktop" (those found)
bool connected();          // the skill in Claude Code's skills folder or schneidi in Claude Desktop's MCP servers
bool offerDue();           // found, not connected, not stopped, not asked too often or too recently
void offerShown();         // counts the offer (the next one at the earliest kOfferDays later)
void offerNever();         // "don't ask again"

} // namespace Cli::Setup
