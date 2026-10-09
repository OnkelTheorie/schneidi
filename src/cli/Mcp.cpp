// MCP server (Model Context Protocol) on stdin/stdout: JSON-RPC 2.0, one message per line. Every command of
// Commands.cpp is one tool; no network port, so it works the same on Linux and Windows.
#include "cli/Mcp.h"

#include "cli/Commands.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>
#include <iostream>
#include <string>

namespace Cli {

namespace {

void send(const QJsonObject& msg)
{
    const QByteArray line = QJsonDocument(msg).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fflush(stdout);
}

void reply(const QJsonValue& id, const QJsonObject& result) { send({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}); }

void replyError(const QJsonValue& id, int code, const QString& message)
{
    send({{"jsonrpc", "2.0"}, {"id", id}, {"error", QJsonObject{{"code", code}, {"message", message}}}});
}

QString imageBase64(const QImage& img)
{
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "JPG", 80);
    return QString::fromLatin1(bytes.toBase64());
}

QJsonObject callTool(const QJsonObject& params)
{
    const QString name = params["name"].toString();
    const Command* cmd = find(name);
    const auto text = [](const QJsonObject& o) {
        return QJsonObject{{"type", "text"}, {"text", QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact))}};
    };
    const auto failure = [&text](const QString& code, const QString& message) {
        const QJsonObject error{{"code", code}, {"message", message}};
        QJsonArray content;
        content << text(QJsonObject{{"ok", false}, {"error", error}});
        return QJsonObject{{"isError", true}, {"content", content}};
    };
    if (!cmd) return failure("NOT_FOUND", "unknown tool " + name);
    Context ctx;
    ctx.mcp = true;
    try {
        QJsonObject result = cmd->run(normalize(*cmd, params["arguments"].toObject()), ctx);
        result.insert("ok", true);
        QJsonArray content;
        content << text(result);
        for (const QImage& img : ctx.images)
            content << QJsonObject{{"type", "image"}, {"data", imageBase64(img)}, {"mimeType", "image/jpeg"}};
        return {{"content", content}};
    } catch (const Error& e) {
        return failure(e.code, e.message);
    }
}

} // namespace

int runMcp()
{
    std::string line;
    while (std::getline(std::cin, line)) {
        const QByteArray data = QByteArray::fromStdString(line).trimmed();
        if (data.isEmpty()) continue;
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
        if (!doc.isObject()) {
            replyError(QJsonValue::Null, -32700, "parse error: " + err.errorString());
            continue;
        }
        const QJsonObject msg = doc.object();
        const QString method = msg["method"].toString();
        const QJsonValue id = msg.value("id");
        const bool request = msg.contains("id");
        if (method == "initialize") {
            const QString version = msg["params"]["protocolVersion"].toString("2025-06-18");
            reply(id, {{"protocolVersion", version},
                       {"capabilities", QJsonObject{{"tools", QJsonObject{}}}},
                       {"serverInfo", QJsonObject{{"name", "schneidi"}, {"version", QCoreApplication::applicationVersion()}}},
                       {"instructions",
                        "schneidi is a video editor. Edit projects (.schneidi) with these tools: `new` creates one, "
                        "`probe`/`silence`/`frames` look at the material, `edit` changes the timeline (see its 'ops' "
                        "description), `info` shows the result, `render` writes a video. The person can open the "
                        "project in the schneidi app afterwards for fine cutting. Use absolute file paths. Times are "
                        "frames in the project frame rate, seconds like \"4.5s\" or timecodes."}});
        } else if (method == "tools/list") {
            QJsonArray tools;
            for (const Command& c : commands()) {
                if (c.name == "help") continue;
                tools << QJsonObject{{"name", c.name}, {"description", c.summary}, {"inputSchema", inputSchema(c)}};
            }
            reply(id, {{"tools", tools}});
        } else if (method == "tools/call") {
            reply(id, callTool(msg["params"].toObject()));
        } else if (method == "ping") {
            reply(id, {});
        } else if (request) {
            replyError(id, -32601, "method not found: " + method);
        } // notifications (initialized, cancelled …) need no answer
    }
    return 0;
}

} // namespace Cli
