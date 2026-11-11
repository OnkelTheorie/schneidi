// SPDX-License-Identifier: AGPL-3.0-only
// MCP server (Model Context Protocol), see Mcp.h. stdio transport: JSON-RPC 2.0, one message per line; no network
// port, so it works the same on Linux and Windows.
#include "cli/Mcp.h"

#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "cli/Live.h"
#include "cli/Sandbox.h"
#include "engine/MemoryRelease.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QThread>

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace Cli::Mcp {

namespace {

// An MCP server lives as long as the AI's chat, mostly idle: after a tool it gives back what the tool used. The picture
// engine keeps buffers for reuse and the heap keeps freed memory (a 3-s render of a 1080x1920 timeline left 1.3 GB).
void releaseMemory()
{
    MemoryRelease::release();
}

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
                "how-tos per task) - read it first. Edit projects (.schneidi) with these tools: `new` creates one "
                "(from a start template such as podcast or vertical; `templates` lists them and content templates, "
                "projects with slots that `templates` fill makes a finished project of), "
                "`probe`/`silence`/`scenes`/`peaks`/`frames`/`transcribe`/`loudness`/`scopes` look at (and listen to) "
                "the material (`sync` lines up recordings of the same event by their sound, `highlights` finds kills "
                "and loud moments for a highlight video, `beats` gives a song's tempo, bars, phrases, breaks and "
                "drops, `index` knows every shot - quality, speech, sounds, descriptions you write - and finds "
                "\"the calm wide shot of the shop\"), `edit` changes a "
                "timeline (see its 'ops' description, `help` with command 'edit' and op '<name>' has details "
                "per op; op 'speak' turns text into a spoken voice clip, op 'voiceover' places a recorded voice; ops 'comp'/'node' build a compositing graph on a "
                "clip - keys, masks, merges, text - and `frames` with clip/node shows the picture at a node; `effects` "
                "lists effects, LUTs, transitions, accents, effect sounds and with kind 'nodes' the node types; an effect "
                "schneidi lacks you write yourself: effect 'ffmpeg_filter' (a filter chain) or 'shader' (GLSL)), `info` shows the "
                "result (a project can have several timelines, each may have its own resolution: a 9:16 short "
                "next to a 16:9 edit), `check` finds what typically goes wrong before "
                "rendering (gaps, sound, game sound buried under the music, stutter in recordings, text a phone app "
                "covers, too long without a cut, a late hook, where viewers left by a retention CSV), `render` writes a video (presets for YouTube Shorts, Instagram Reels, TikTok), `short` makes a "
                "finished vertical short of recordings in one call (best moments, reframe, captions, check, render), "
                "`restore` undoes a change (`backups` lists the saved states), `brand` is the user's brand kit "
                "(logo, colors, fonts, intro/outro; titles can follow it), "
                "`watch` takes new files of watched folders into the media pool (a recorder's output; for automated "
                "work: new recording -> highlights -> render).";
    if (opt.app)
        s += " If the project is open in the schneidi app, all tools "
             "work live on what the app shows (unsaved changes included): `edit` changes it there as one "
             "undo step and does not save, `info` adds the user's playhead and selection, `app` controls "
             "the app (play, seek, select, undo, save, proxies, cleaning up old proxy files). \"@app\" as "
             "project = the project open in the running app.";
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
    s += " Times are frames in the project frame rate, seconds like \"4.5s\", timecodes \"HH:MM:SS:FF\" "
         "or clock times \"MM:SS\" / \"HH:MM:SS.sss\".";
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
                                                     "\"all\" = every op; without op: only what each op is for).")
                                           : c.summary;
        static const QStringList longTools{"render", "transcribe", "sync", "highlights", "scenes", "index"};
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
    if (qEnvironmentVariable("SCHNEIDI_TEST_THROW_TOOL") == tool) throw std::runtime_error("test exception");
    if (delay <= 0) return;
    QThread::msleep(delay / 2);
    if (progress) progress(0.5);
    QThread::msleep(delay - delay / 2);
}

// The program file this server runs from (an AppImage: the AppImage file) as it was at the start. An MCP server
// lives as long as the AI's chat: after an update it would go on answering with the old build without a word
struct ProgramFile {
    QString path;
    QDateTime modified;
    qint64 size = -1;
};

ProgramFile programFile(const QString& path)
{
    const QFileInfo fi(path);
    return {path, fi.exists() ? fi.lastModified() : QDateTime(), fi.exists() ? fi.size() : -1};
}

// The file on disk is another one now (there and different: one being replaced may be gone for a moment)
bool updated(const ProgramFile& started)
{
    if (started.path.isEmpty() || started.size < 0) return false;
    const ProgramFile now = programFile(started.path);
    return now.size >= 0 && (now.size != started.size || now.modified != started.modified);
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

namespace {

// A tool that is not there: the call to make instead when it is an op of `edit` (small models call "trim" or
// "new_timeline" as tools), else the tools there are
QString unknownTool(const QString& name, const QJsonObject& args, const Options& opt)
{
    for (const detail::OpDef& d : detail::editOps()) {
        if (d.name != name) continue;
        QJsonObject op = args;
        const QJsonValue project = op.take("project");
        QJsonObject fields{{"op", name}};
        for (auto it = op.begin(); it != op.end(); ++it) fields.insert(it.key(), it.value());
        const QString fallback = qEnvironmentVariable(Live::ChatServerEnv).isEmpty() ? "<project file>" : Live::AppProject;
        const QJsonObject call{{"project", project.isString() ? project : QJsonValue(fallback)}, {"ops", QJsonArray{fields}}};
        return QString("unknown tool %1: '%1' is an op of the edit tool - call edit with %2 (op %3)")
            .arg(name, QString::fromUtf8(QJsonDocument(call).toJson(QJsonDocument::Compact)), d.usage);
    }
    QStringList tools;
    for (const Command& c : commands())
        if (offered(c.name, opt)) tools << c.name;
    return QString("unknown tool %1 (tools: %2)").arg(name, tools.join(", "));
}

} // namespace

QJsonObject callTool(const QJsonObject& params, const Options& opt, const Send& notify)
{
    const QString name = params["name"].toString();
    const Command* cmd = find(name);
    if (!cmd || !offered(name, opt))
        return toolError("NOT_FOUND", unknownTool(name, params["arguments"].toObject(), opt));
    Context ctx;
    ctx.mcp = true;
    ctx.partId = params["_meta"].toObject().value("schneidi/partId").toString();
    // Long jobs (render, transcribe, loudness …) report progress when the client asked for it
    const QJsonValue token = params["_meta"].toObject().value("progressToken");
    if (!token.isUndefined() && !token.isNull() && notify) {
        auto last = std::make_shared<int>(-1);
        // The step running now (of `short`: "transcribe", "render" …) as the notification's message
        auto stage = std::make_shared<QString>(name);
        ctx.progress = [token, last, stage, name, notify](double p) {
            const int percent = std::clamp(int(p * 100), 0, 100);
            if (percent == *last) return;
            *last = percent;
            QJsonObject params{{"progressToken", token}, {"progress", percent}, {"total", 100}};
            if (*stage != name) params["message"] = *stage;
            notify({{"jsonrpc", "2.0"}, {"method", "notifications/progress"}, {"params", params}});
        };
        ctx.stage = [stage](const QString& s) { *stage = s; };
    }
    QJsonObject out;
    try {
        testHooks(name, ctx.progress);
        QJsonObject args = params["arguments"].toObject();
        const bool appProject = defaultToAppProject(*cmd, args);
        QJsonObject result = execute(*cmd, normalize(*cmd, args), ctx);
        result = Sandbox::mapBack(result).toObject(); // paths as the client knows them (--map)
        if (appProject) {
            QJsonArray notes = result["notes"].toArray();
            notes << "no 'project' given: worked on \"@app\", the project open in the app (give it in every call)";
            result["notes"] = notes;
        }
        out = toolResult(result);
        QJsonArray content = out["content"].toArray();
        for (const QImage& img : ctx.images)
            content << QJsonObject{{"type", "image"}, {"data", imageBase64(img)}, {"mimeType", "image/jpeg"}};
        out["content"] = content;
    } catch (const Error& e) {
        out = toolError(e.code, e.message);
    } catch (const std::exception& e) {
        // Out of memory with a huge transcript, a library … : this call failed, the tool server goes on
        out = toolError("INTERNAL", QString("internal error: %1").arg(QString::fromLocal8Bit(e.what())));
    } catch (...) {
        out = toolError("INTERNAL", "internal error");
    }
    releaseMemory();
    return out;
}

bool hasNulKey(const QJsonValue& v)
{
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it)
            if (it.key().contains(QChar(0)) || hasNulKey(it.value())) return true;
    } else if (v.isArray()) {
        const QJsonArray a = v.toArray();
        return std::any_of(a.begin(), a.end(), [](const QJsonValue& x) { return hasNulKey(x); });
    }
    return false;
}

bool handle(const QJsonObject& msg, const Options& opt, const ToolCall& call, const Send& send)
{
    if (hasNulKey(msg)) { // its id cannot be read for sure either
        send({{"jsonrpc", "2.0"}, {"id", QJsonValue::Null},
              {"error", QJsonObject{{"code", -32600}, {"message", "invalid request: a key contains U+0000"}}}});
        return true;
    }
    const QString method = msg["method"].toString();
    // Notifications (initialized, cancelled …) and responses need no answer
    if (!msg.contains("id")) return false;
    const QJsonValue id = msg.value("id");
    if (!msg.contains("method")) {
        if (msg.contains("result") || msg.contains("error")) return false; // a response
        // A request without its method: unanswered, the client would wait for it forever
        send({{"jsonrpc", "2.0"}, {"id", id},
              {"error", QJsonObject{{"code", -32600}, {"message", "invalid request: no 'method'"}}}});
        return true;
    }
    // A request all the same: unanswered, its HTTP request (and connection) would wait forever
    if (method.isEmpty()) {
        send({{"jsonrpc", "2.0"}, {"id", id},
              {"error", QJsonObject{{"code", -32600}, {"message", "invalid request: 'method' must be a non-empty string"}}}});
        return true;
    }
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
    QString exe = qEnvironmentVariable("APPIMAGE");
    if (exe.isEmpty()) exe = QCoreApplication::applicationFilePath();
    const ProgramFile started = programFile(exe);
    const ToolCall here = [&opt, started](const QJsonObject& params, const Send& notify, const Done& done) {
        QJsonObject result = callTool(params, opt, notify);
        if (updated(started)) {
            QJsonArray content = result["content"].toArray();
            content << QJsonObject{
                {"type", "text"},
                {"text", QString("Note: schneidi was updated (%1) while this MCP server kept running the old version: "
                                 "restart the schneidi MCP server (or start a new chat/session) to use the new version.")
                             .arg(QDir::toNativeSeparators(started.path))}};
            result["content"] = content;
        }
        done(result);
    };
    std::string line;
    while (std::getline(std::cin, line)) {
        const QByteArray data = QByteArray::fromStdString(line).trimmed();
        if (data.isEmpty()) continue;
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
        const auto invalid = [&send](const QString& message) {
            send({{"jsonrpc", "2.0"}, {"id", QJsonValue::Null},
                  {"error", QJsonObject{{"code", -32600}, {"message", "invalid request: " + message}}}});
        };
        if (doc.isArray()) {
            // A batch (2025-03-26 clients, as over HTTP): one array of the answers; notifications on the way go out
            // as they come. (It answered "parse error: no error occurred" before.)
            const QJsonArray messages = doc.array();
            if (messages.isEmpty()) {
                invalid("empty batch");
                continue;
            }
            if (Mcp::hasNulKey(messages)) {
                invalid("a key contains U+0000");
                continue;
            }
            QJsonArray answers;
            const Send collect = [&answers, &send](const QJsonObject& msg) {
                if (msg.contains("id") && (msg.contains("result") || msg.contains("error"))) answers << msg;
                else send(msg);
            };
            for (const QJsonValue& m : messages) {
                if (m.isObject()) handle(m.toObject(), opt, here, collect);
                else
                    answers << QJsonObject{{"jsonrpc", "2.0"}, {"id", QJsonValue::Null},
                                           {"error", QJsonObject{{"code", -32600}, {"message", "invalid request: a message is not a JSON object"}}}};
            }
            if (!answers.isEmpty()) {
                const QByteArray out = QJsonDocument(answers).toJson(QJsonDocument::Compact) + '\n';
                std::fwrite(out.constData(), 1, size_t(out.size()), stdout);
                std::fflush(stdout);
            }
            continue;
        }
        if (!doc.isObject()) {
            if (err.error == QJsonParseError::NoError) invalid("a message is a JSON object");
            else
                send({{"jsonrpc", "2.0"}, {"id", QJsonValue::Null},
                      {"error", QJsonObject{{"code", -32700}, {"message", "parse error: " + err.errorString()}}}});
            continue;
        }
        handle(doc.object(), opt, here, send);
    }
    return 0;
}

} // namespace Cli::Mcp
