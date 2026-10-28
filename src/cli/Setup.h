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

// Command line `schneidi-cli setup <target> [--only code|desktop] [--remove]`; prints JSON, returns the exit code
int run(QStringList args, bool pretty);

} // namespace Cli::Setup
