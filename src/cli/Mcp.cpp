// MCP server (Model Context Protocol), see Mcp.h. stdio transport: JSON-RPC 2.0, one message per line; no network
// port, so it works the same on Linux and Windows.
#include "cli/Mcp.h"

#include "cli/Commands.h"
#include "cli/Sandbox.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QThread>

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

namespace Cli::Mcp {

namespace {

QString imageBase64(const QImage& img)
{
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "JPG", 80);
    return QString::fromLatin1(bytes.toBase64());
}

QJsonObject text(const QJsonObject& o)
{
    return {{"type", "text"}, {"text", QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact))}};
}

QString instructions(const Options& opt)
{
    QString s = "schneidi is a video editor. `guide` explains how to edit well with it (workflow and "
                "how-tos per task) - read it first. Edit projects (.schneidi) with these tools: `new` creates one, "
                "`probe`/`silence`/`scenes`/`peaks`/`frames`/`transcribe`/`loudness`/`scopes` look at (and listen to) "
                "the material (`sync` lines up recordings of the same event by their sound, `highlights` finds kills "
                "and loud moments for a highlight video), `edit` changes a "
                "timeline (see its 'ops' description, `help` with command 'edit' and op '<name>' has details "
                "per op; `effects` lists effects, LUTs and transitions), `info` shows the "
                "result (a project can have several timelines), `render` writes a video, `restore` undoes a "
                "change (`backups` lists the saved states).";
    if (opt.app)
        s += " If the project is open in the schneidi app, all tools "
             "work live on what the app shows (unsaved changes included): `edit` changes it there as one "
             "undo step and does not save, `info` adds the user's playhead and selection, `app` controls "
             "the app (play, seek, select, undo, save, proxies). \"@app\" as project = the project open "
             "in the running app.";
    if (opt.root.isEmpty())
        s += " Use absolute file paths.";
    else
        s += QString(" Files: everything happens in the work folder %1 on this computer; paths are relative to it (or "
                     "absolute inside it), nothing outside it can be used.")
                 .arg(opt.root);
    if (opt.server)
        s += QString(" This is a schneidi server: `files` lists the material in the work folder, `upload` fetches a "
                     "file from a URL into it (people can also PUT /files/<path> over HTTP), and answers that wrote "
                     "files carry a `url` to download them (with the same Authorization header). A call that takes "
                     "longer than about %1 s answers {\"job\", \"state\":\"running\"} and keeps running: `jobs` with "
                     "that job (and wait) gives its progress and then its result.")
                 .arg(opt.jobWaitS);
    s += " Times are frames in the project frame rate, seconds like \"4.5s\" or timecodes.";
    return s;
}

QJsonArray toolList(const Options& opt)
{
    QJsonArray tools;
    for (const Command& c : commands()) {
        if (!offered(c.name, opt)) continue;
        // help: only for the details of the edit ops (the tool list already has the rest)
        QString summary = c.name == "help" ? QString("Details of the edit ops (parameters, ranges): "
                                                     "command 'edit' and op 'name' (or a comma list; "
                                                     "without op: all ops, long).")
                                           : c.summary;
        static const QStringList longTools{"render", "transcribe", "sync", "highlights", "scenes"};
        if (opt.server && longTools.contains(c.name))
            summary += QString(" On this server: if it takes longer than %1 s, the answer is a job (see `jobs`).")
                           .arg(opt.jobWaitS);
        tools << QJsonObject{{"name", c.name}, {"description", summary}, {"inputSchema", inputSchema(c)}};
    }
    for (const QJsonValue& t : opt.extraTools) tools << t;
    return tools;
}

QJsonObject initializeResult(const QJsonObject& params, const Options& opt)
{
    // The client's version if spoken here, else the newest (the client decides whether it can go on)
    const QString asked = params["protocolVersion"].toString();
    const QString version = versions().contains(asked) ? asked : versions().first();
    return {{"protocolVersion", version},
            {"capabilities", QJsonObject{{"tools", QJsonObject{}}}},
            {"serverInfo", QJsonObject{{"name", "schneidi"}, {"version", QCoreApplication::applicationVersion()}}},
            {"instructions", instructions(opt)}};
}

// Test hooks of the server tests (tests/test_serve.cpp): a slow call (with one progress step in the middle) and a
// crashing worker, without slow or broken media
void testHooks(const QString& tool, const std::function<void(double)>& progress)
{
    static const int delay = qEnvironmentVariableIntValue("SCHNEIDI_TEST_CALL_DELAY_MS");
    if (qEnvironmentVariable("SCHNEIDI_TEST_CRASH_TOOL") == tool) std::abort();
    if (delay <= 0) return;
    QThread::msleep(delay / 2);
    if (progress) progress(0.5);
    QThread::msleep(delay - delay / 2);
}

} // namespace

const QStringList& versions()
{
    static const QStringList list{"2025-06-18", "2025-03-26", "2024-11-05"};
    return list;
}

bool offered(const QString& tool, const Options& opt)
{
    if (tool == "app") return opt.app;
    if (tool == "extensions") return opt.extensions;
    return true;
}

QJsonObject toolResult(const QJsonObject& answer)
{
    QJsonObject o = answer;
    if (!o.contains("ok")) o.insert("ok", true);
    return {{"content", QJsonArray{text(o)}}};
}

QJsonObject toolError(const QString& code, const QString& message)
{
    const QJsonObject error{{"code", code}, {"message", message}};
    return {{"isError", true}, {"content", QJsonArray{text(QJsonObject{{"ok", false}, {"error", error}})}}};
}

QJsonObject callTool(const QJsonObject& params, const Options& opt, const Send& notify)
{
    const QString name = params["name"].toString();
    const Command* cmd = find(name);
    if (!cmd || !offered(name, opt)) return toolError("NOT_FOUND", "unknown tool " + name);
    Context ctx;
    ctx.mcp = true;
    // Long jobs (render, transcribe, loudness …) report progress when the client asked for it
    const QJsonValue token = params["_meta"].toObject().value("progressToken");
    if (!token.isUndefined() && !token.isNull() && notify) {
        auto last = std::make_shared<int>(-1);
        ctx.progress = [token, last, notify](double p) {
            const int percent = std::clamp(int(p * 100), 0, 100);
            if (percent == *last) return;
            *last = percent;
            notify({{"jsonrpc", "2.0"}, {"method", "notifications/progress"},
                    {"params", QJsonObject{{"progressToken", token}, {"progress", percent}, {"total", 100}}}});
        };
    }
    try {
        testHooks(name, ctx.progress);
        QJsonObject result = execute(*cmd, normalize(*cmd, params["arguments"].toObject()), ctx);
        result = Sandbox::mapBack(result).toObject(); // paths as the client knows them (--map)
        QJsonObject out = toolResult(result);
        QJsonArray content = out["content"].toArray();
        for (const QImage& img : ctx.images)
            content << QJsonObject{{"type", "image"}, {"data", imageBase64(img)}, {"mimeType", "image/jpeg"}};
        out["content"] = content;
        return out;
    } catch (const Error& e) {
        return toolError(e.code, e.message);
    }
}

bool handle(const QJsonObject& msg, const Options& opt, const ToolCall& call, const Send& send)
{
    const QString method = msg["method"].toString();
    // Notifications (initialized, cancelled …) and responses need no answer
    if (!msg.contains("id") || method.isEmpty()) return false;
    const QJsonValue id = msg.value("id");
    const auto reply = [send, id](const QJsonObject& result) {
        send({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
    };
    if (method == "initialize") {
        reply(initializeResult(msg["params"].toObject(), opt));
    } else if (method == "tools/list") {
        reply({{"tools", toolList(opt)}});
    } else if (method == "tools/call") {
        call(msg["params"].toObject(), send, reply);
    } else if (method == "ping") {
        reply({});
    } else {
        send({{"jsonrpc", "2.0"}, {"id", id},
              {"error", QJsonObject{{"code", -32601}, {"message", "method not found: " + method}}}});
    }
    return true;
}

int run(const Options& opt)
{
    const Send send = [](const QJsonObject& msg) {
        const QByteArray line = QJsonDocument(msg).toJson(QJsonDocument::Compact) + '\n';
        std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
        std::fflush(stdout);
    };
    const ToolCall here = [&opt](const QJsonObject& params, const Send& notify, const Done& done) {
        done(callTool(params, opt, notify));
    };
    std::string line;
    while (std::getline(std::cin, line)) {
        const QByteArray data = QByteArray::fromStdString(line).trimmed();
        if (data.isEmpty()) continue;
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
        if (!doc.isObject()) {
            send({{"jsonrpc", "2.0"}, {"id", QJsonValue::Null},
                  {"error", QJsonObject{{"code", -32700}, {"message", "parse error: " + err.errorString()}}}});
            continue;
        }
        handle(doc.object(), opt, here, send);
    }
    return 0;
}

} // namespace Cli::Mcp
