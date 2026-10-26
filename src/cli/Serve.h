#pragma once
// `schneidi-cli serve`: the MCP server over HTTP ("Streamable HTTP", MCP 2025-06-18, 2025-03-26 clients too), so AIs
// on other devices and automation (Docker) edit videos on a server. One endpoint POST /mcp (answers as JSON, or as a
// Server-Sent Events stream when a call reports progress), DELETE /mcp ends a session; files in and out with
// PUT/GET /files/<path>. Everything happens in a work folder (--root, cli/Sandbox.h); tool calls run as jobs in
// worker processes (ServeJobs.h). Listens on 127.0.0.1 by default; on any other address a token is required
// (Authorization: Bearer). TLS: a reverse proxy in front (packaging/docker/compose.yml).

#include <QHash>
#include <QJsonObject>
#include <QStringList>

namespace Cli::Serve {

// Options of `serve` and `mcp`: "--name value" or "--name=value"; `valued` take a value (may repeat), `flags` none.
// Throws Error (USAGE) for anything else.
QHash<QString, QStringList> parseOptions(const QStringList& args, const QStringList& valued, const QStringList& flags);

// `schneidi-cli help`: the two servers (mcp, serve) with their options
QJsonObject help();

// Runs until SIGINT/SIGTERM; exit code
int run(const QStringList& args);

} // namespace Cli::Serve
