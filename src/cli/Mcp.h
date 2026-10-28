// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// MCP server (Model Context Protocol): every command of Commands.cpp is one tool. The message handling is shared
// by both transports: stdio (`schneidi-cli mcp`, one JSON-RPC message per line, below) and HTTP (`schneidi-cli
// serve`, Serve.cpp), so initialize, tools/list and tools/call are answered in one place.

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

#include <functional>

namespace Cli::Mcp {

// Protocol versions spoken, newest first (2025-03-26 and 2024-11-05 clients get their own version back)
const QStringList& versions();

struct Options {
    bool server = false;     // `serve`: work folder, files/upload/jobs, long calls become jobs
    bool app = true;         // the `app` tool (live connection to the desktop app)
    bool extensions = true;  // the `extensions` tool (downloads programs)
    QString root;            // work folder (for the instructions), empty = none
    int jobWaitS = 50;       // server: how long a call waits before it answers with a job
    QJsonArray extraTools;   // tools of the transport (serve: jobs, files, upload)
};

using Send = std::function<void(const QJsonObject& message)>;
using Done = std::function<void(const QJsonObject& result)>;
// tools/call: the tool result through `done` (once, maybe later), progress notifications through `notify` before it
using ToolCall = std::function<void(const QJsonObject& params, const Send& notify, const Done& done)>;

// Answers one JSON-RPC message: a request gets its response through `send` (tools/call through `call`, maybe
// later), notifications and responses get none (returns false: nothing will be sent)
bool handle(const QJsonObject& msg, const Options& opt, const ToolCall& call, const Send& send);
// A key with U+0000 in it, anywhere inside: such messages are refused. Qt (6.8) finds "id\u0000…" under "id" or not,
// depending on what lies in memory after the key, so one message could be read two ways (found by fuzz_mcp)
bool hasNulKey(const QJsonValue& v);

// Runs a tool in this process; progress notifications through `notify` when the client gave a progress token
QJsonObject callTool(const QJsonObject& params, const Options& opt, const Send& notify);
// Tool results: the answer as JSON text (plus images), an error as {"ok":false,"error":{code,message}} with isError
QJsonObject toolResult(const QJsonObject& answer);
QJsonObject toolError(const QString& code, const QString& message);
// The tool is offered with these options (app and extensions can be off)
bool offered(const QString& tool, const Options& opt);

// MCP server on stdin/stdout until stdin closes
int run(const Options& opt);

} // namespace Cli::Mcp
