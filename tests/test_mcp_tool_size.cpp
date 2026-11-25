// SPDX-License-Identifier: AGPL-3.0-only
// The MCP tool list stays small (cli/ToolText.h): an AI reads all of it before it does anything (it was ~96 KB, about
// 24,000 tokens, more than a local model with a 16k context holds). Each tool says what it is for with its common
// parameters; `help` keeps every text of the command, so nothing a description lost is out of reach. Errors teach:
// they name the parameter or op meant and the `help` call with the details. Sizes: tools/mcp_tool_sizes.py.
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "cli/Mcp.h"
#include "cli/ToolText.h"

#include "check.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdio>

namespace {

// The whole tools/list answer (compact JSON) at most this many characters: ~4,000 tokens (characters / 4)
constexpr qsizetype kBudget = 16000;
// The MCP initialize instructions
constexpr qsizetype kInstructions = 1200;
// Tools whose short text waits for other branches (their parameters change there): not counted yet. TODO: empty
const QStringList kPending{"short", "direct", "render"};

QJsonObject request(const QJsonObject& msg, const Cli::Mcp::Options& opt = {})
{
    QJsonObject answer;
    Cli::Mcp::handle(
        msg, opt, [](const QJsonObject&, const Cli::Mcp::Send&, const Cli::Mcp::Done&) {},
        [&answer](const QJsonObject& m) { answer = m; });
    return answer["result"].toObject();
}

QString messageOf(const QString& command, const QJsonObject& args)
{
    const QJsonObject r = Cli::Mcp::callTool({{"name", command}, {"arguments", args}}, {}, {});
    const QJsonObject o = QJsonDocument::fromJson(r["content"][0]["text"].toString().toUtf8()).object();
    return o["error"]["message"].toString();
}

qsizetype size(const QJsonValue& v)
{
    return v.isObject() ? QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact).size()
                        : QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact).size();
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);

    // ---- The size: the whole list and the instructions ----
    const QJsonArray tools = request({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}})["tools"].toArray();
    CHECK_EQ(tools.size(), int(Cli::commands().size()));
    const qsizetype total = size(QJsonObject{{"tools", tools}});
    std::printf("       tools/list: %lld characters (~%lld tokens), budget %lld\n", qlonglong(total),
                qlonglong(total / 4), qlonglong(kBudget));
    qsizetype pending = 0;
    for (const QJsonValue& t : tools) {
        std::printf("         %-16s %5lld\n", qPrintable(t["name"].toString()), qlonglong(size(t)));
        if (kPending.contains(t["name"].toString())) pending += size(t) + 1;
    }
    if (pending) std::printf("       not counted yet (pending: %s): %lld\n", qPrintable(kPending.join(", ")), qlonglong(pending));
    CHECK(total - pending <= kBudget);
    const QString instructions =
        request({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", QJsonObject{}}})["instructions"].toString();
    std::printf("       instructions: %lld characters\n", qlonglong(instructions.size()));
    CHECK(instructions.size() <= kInstructions);
    CHECK(instructions.contains("`guide`") && instructions.contains("`help`") && instructions.contains("@app")
          && instructions.contains("absolute") && instructions.contains("\"4.5s\""));

    // ---- Every tool has its short text, and every parameter is either shown or left to `help` on purpose ----
    for (const Cli::Command& c : Cli::commands()) {
        const Cli::ToolText::Tool* t = Cli::ToolText::find(c.name);
        if (!t && kPending.contains(c.name)) continue;
        if (!CHECK(t)) {
            std::printf("       %s has no short text in cli/ToolText.cpp\n", qPrintable(c.name));
            continue;
        }
        QStringList names;
        for (const Cli::Param& p : c.params) {
            names << p.name;
            if (!CHECK(t->params.contains(p.name) != t->more.contains(p.name)))
                std::printf("       %s: parameter %s must be in 'params' or in 'more' (once)\n", qPrintable(c.name),
                            qPrintable(p.name));
            // A required parameter is always shown
            if (p.required && !CHECK(t->params.contains(p.name)))
                std::printf("       %s: required parameter %s is not shown\n", qPrintable(c.name), qPrintable(p.name));
        }
        for (const QString& n : t->params.keys() + t->more)
            if (!CHECK(names.contains(n))) std::printf("       %s: no parameter %s\n", qPrintable(c.name), qPrintable(n));
        // A tool that leaves parameters to help says so
        if (!t->more.isEmpty() && c.name != "help" && !CHECK(t->description.size() < 400))
            std::printf("       %s: description %lld characters\n", qPrintable(c.name), qlonglong(t->description.size()));
    }
    // The shown parameters with their fixed values as enum; `help` has them too, with the full texts
    for (const QJsonValue& v : tools) {
        if (v["name"].toString() != "app") continue;
        const QJsonObject action = v["inputSchema"]["properties"]["action"].toObject();
        CHECK(action["enum"].toArray().contains("seek") && !action.contains("description"));
        CHECK(!v["inputSchema"]["properties"].toObject().contains("mode")); // left to help
    }
    {
        const Cli::Command* app = Cli::find("app");
        const QJsonObject props = Cli::inputSchema(*app)["properties"].toObject();
        CHECK(props.contains("mode") && props["mode"]["description"].toString().contains("smart"));
        CHECK(props["action"]["enum"].toArray().contains("clean_proxies"));
        CHECK(props["action"]["description"].toString().contains("render_cache"));
    }

    // ---- edit: the op names by group, every op once, details through help ----
    {
        QStringList grouped;
        for (const auto& [group, ops] : Cli::ToolText::editOpGroups())
            for (const QString& op : ops) {
                if (!CHECK(!grouped.contains(op))) std::printf("       op %s in two groups\n", qPrintable(op));
                grouped << op;
            }
        QStringList all;
        for (const Cli::detail::OpDef& d : Cli::detail::editOps()) {
            all << d.name;
            if (!CHECK(grouped.contains(d.name)))
                std::printf("       op %s is in no group of cli/ToolText.cpp editOpGroups\n", qPrintable(d.name));
        }
        for (const QString& op : grouped)
            if (!CHECK(all.contains(op))) std::printf("       group names op %s, which does not exist\n", qPrintable(op));
        const QString brief = Cli::ToolText::editOpsBrief();
        CHECK(brief.contains("speed_ramp") && brief.contains("help {command:'edit', op:'name'}") && brief.size() < 2200);
        for (const QJsonValue& v : tools)
            if (v["name"].toString() == "edit") CHECK_EQ(v["inputSchema"]["properties"]["ops"]["description"].toString(), brief);
    }

    // ---- Nothing is lost: help with a command has every parameter with its full text ----
    for (const Cli::Command& c : Cli::commands()) {
        const QJsonObject r = Cli::Mcp::callTool({{"name", "help"}, {"arguments", QJsonObject{{"command", c.name}}}}, {}, {});
        const QJsonObject o = QJsonDocument::fromJson(r["content"][0]["text"].toString().toUtf8()).object();
        const QJsonObject cmd = o["commands"][0].toObject();
        CHECK_EQ(cmd["summary"].toString(), c.summary);
        const QJsonObject props = cmd["params"]["properties"].toObject();
        for (const Cli::Param& p : c.params) {
            const QString text = props[p.name]["description"].toString();
            if (!CHECK(c.name == "edit" && p.name == "ops" ? text.contains("relative") : text == p.help))
                std::printf("       help %s: parameter %s lost its text\n", qPrintable(c.name), qPrintable(p.name));
        }
    }
    {
        // help edit: the rules once (not every op a second time in the 'ops' text), the ops with usage and summary
        const QJsonObject r = Cli::Mcp::callTool({{"name", "help"}, {"arguments", QJsonObject{{"command", "edit"}}}}, {}, {});
        const QByteArray text = r["content"][0]["text"].toString().toUtf8();
        std::printf("       help edit: %lld characters\n", qlonglong(text.size()));
        CHECK(text.size() < 30000);
        const QJsonObject o = QJsonDocument::fromJson(text).object();
        CHECK(o["commands"][0]["ops"].toArray().size() > 90);
    }

    // ---- Errors teach: the name meant, the valid ones, the help call ----
    {
        CHECK(Cli::ToolText::nearest("colour", {"color", "colors", "clip"}).first() == "color");
        CHECK(Cli::ToolText::nearest("lenght", {"length", "at"}) == QStringList{"length"});
        CHECK(Cli::ToolText::nearest("add_title", {"title", "subtitle", "trim"}).contains("title"));
        CHECK(Cli::ToolText::nearest("xyz", {"color", "length"}).isEmpty());

        QTemporaryDir dir;
        const QString project = dir.filePath("t.schneidi");
        CHECK(messageOf("new", {{"project", project}}).isEmpty());
        const QString unknown = messageOf("info", {{"project", project}, {"summery", true}});
        CHECK(unknown.contains("did you mean 'summary'") && unknown.contains("help {command:'info'}"));
        const QString missing = messageOf("export_timeline", {{"project", project}});
        CHECK(missing.contains("missing parameter 'out'") && missing.contains("help {command:'export_timeline'}"));
        const QString wrong = messageOf("frames", {{"source", project}, {"count", "many"}});
        CHECK(wrong.contains("whole number") && wrong.contains("help {command:'frames'}"));
        const QString op = messageOf("edit", {{"project", project}, {"ops", QJsonArray{QJsonObject{{"op", "add_title"}}}}});
        CHECK(op.contains("did you mean 'title'") && op.contains("help {command:'edit', op:'title'}"));
        const QString key = messageOf("edit", {{"project", project},
                                               {"ops", QJsonArray{QJsonObject{{"op", "title"}, {"text", "x"}, {"colour", "#fff"}}}}});
        CHECK(key.contains("use 'color'") && key.contains("help {command:'edit', op:'title'}"));
        const QString tool = messageOf("probes", {});
        CHECK(tool.contains("did you mean probe"));
        for (const QString& m : {unknown, missing, wrong, op, key, tool}) std::printf("       %s\n", qPrintable(m));
    }
    return Check::result();
}
