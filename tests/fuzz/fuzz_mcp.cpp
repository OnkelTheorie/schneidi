// Fuzz target: MCP messages (JSON-RPC, cli/Mcp.h) as `schneidi-cli mcp` reads a line and `serve` reads the body of
// POST /mcp (one message or a batch). Every message is answered by Mcp::handle; a tools/call checks its arguments
// like the server does before a job starts (Commands normalize: types, aliases, paths in the work folder) and reads
// its times (frames, "4.5s", timecodes), but runs no tool.
//
// Rules checked: every request gets exactly one JSON-RPC answer, and every path an argument check accepts is inside
// the work folder (also through links).
#include "FuzzCommon.h"
#include "FuzzJson.h"

#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "cli/Mcp.h"
#include "cli/Sandbox.h"

#include <QJsonDocument>

namespace {

using namespace Cli;

Fuzz::WorkFolder& world()
{
    static Fuzz::WorkFolder w;
    return w;
}

Mcp::Options& options()
{
    static Mcp::Options o;
    return o;
}

void checkArguments(const Command& cmd, const QJsonObject& args)
{
    ProjectFormat fmt;
    for (const Param& p : cmd.params) {
        if (!args.contains(p.name)) continue;
        const QJsonValue v = args[p.name];
        switch (p.type) {
        case ParamType::Path: world().checkInside(v.toString()); break;
        case ParamType::Paths:
            for (const QJsonValue& x : v.toArray()) world().checkInside(x.toString());
            break;
        case ParamType::Sources:
            for (const QJsonValue& x : v.toArray()) world().checkInside(x.isObject() ? x["file"].toString() : x.toString());
            break;
        case ParamType::Time:
            try {
                FUZZ_ASSERT(detail::parseTime(v, fmt, p.name) >= 0);
            } catch (const Error&) {
            }
            break;
        case ParamType::Times:
            for (const QJsonValue& x : v.toArray()) try {
                    FUZZ_ASSERT(detail::parseTime(x, fmt, p.name) >= 0);
                } catch (const Error&) {
                }
            break;
        default: break;
        }
    }
}

} // namespace

FUZZ_JSON_MUTATOR

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    Fuzz::init("mcp");
    Sandbox::configure({world().root, {"Z:\\Clips=sub"}, false});
    Mcp::Options& o = options();
    o.server = true;
    o.app = false;
    o.extensions = false;
    o.root = Sandbox::root();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const QByteArray bytes(reinterpret_cast<const char*>(data), qsizetype(size));
    const QJsonDocument doc = QJsonDocument::fromJson(bytes);
    QJsonArray messages;
    if (doc.isArray()) messages = doc.array();
    else if (doc.isObject()) messages << doc.object();
    else return 0;

    const Mcp::ToolCall call = [](const QJsonObject& params, const Mcp::Send&, const Mcp::Done& done) {
        const QString name = params["name"].toString();
        const Command* cmd = find(name);
        if (!cmd || !Mcp::offered(name, options())) {
            done(Mcp::toolError("NOT_FOUND", "unknown tool " + name));
            return;
        }
        try {
            checkArguments(*cmd, normalize(*cmd, params["arguments"].toObject()));
        } catch (const Error& e) {
            done(Mcp::toolError(e.code, e.message));
            return;
        }
        done(Mcp::toolResult({}));
    };
    for (const QJsonValue& m : messages) {
        if (!m.isObject()) continue;
        const QJsonObject msg = m.toObject();
        int answers = 0;
        const Mcp::Send send = [&](const QJsonObject& answer) {
            ++answers;
            FUZZ_ASSERT(answer["jsonrpc"] == "2.0");
            FUZZ_ASSERT(answer.contains("result") != answer.contains("error"));
            FUZZ_ASSERT(answer.value("id") == msg.value("id"));
            (void)QJsonDocument(answer).toJson(QJsonDocument::Compact);
        };
        const bool request = Mcp::handle(msg, options(), call, send);
        FUZZ_ASSERT(answers == (request ? 1 : 0));
    }
    return 0;
}
