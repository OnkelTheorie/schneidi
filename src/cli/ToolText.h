// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The short texts of the MCP tool list. An AI reads the whole tool list before it does anything (it was ~96 KB, about
// 24,000 tokens: more than many local models hold), so each tool there says only what it is for, when to use it and
// what goes wrong without being told, and shows its common parameters. Everything else - every parameter, defaults,
// answer fields, error codes, edge cases - stays in the command's own texts (Command::summary, Param::help in
// Commands.cpp), which `help` with the command shows; edit ops: `help` with command 'edit' and op '<name>'.
// A parameter left out of the tool list still works (normalize takes every parameter of the command).

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <utility>

namespace Cli {

struct Command;

namespace ToolText {

struct Param {
    QString text;        // a few words; empty = the name says it (or the instructions do, "project", "timeline")
    QStringList choices; // a fixed list of values: the schema's enum
};

struct Tool {
    QString description;
    QHash<QString, Param> params; // shown in the tool list (in the command's order)
    QStringList more;             // left out of the tool list: `help` has them
};

// The short text of a tool; nullptr = none yet (the tool list then shows the command's full texts)
const Tool* find(const QString& command);
// The tool list entry's description and input schema (short where a Tool exists)
QString description(const Command& cmd);
QJsonObject schema(const Command& cmd);
// The values of a parameter with a fixed list (empty = free), for the enum of the schemas
QStringList choices(const QString& command, const QString& param);

// The edit ops by group ("cut", {"append", "place", …}); every op is in one group (test_mcp_tool_size)
const QVector<std::pair<QString, QStringList>>& editOpGroups();
// The `ops` description of the tool list: the op names by group, time formats, where the details are
QString editOpsBrief();

// The names of `candidates` closest to `name` (typing errors, other words for the same thing: "colour" -> "color",
// "lenght" -> "length", "new_track" -> "add_track"), best first, at most `max`; empty when none is close
QStringList nearest(const QString& name, const QStringList& candidates, int max = 3);

} // namespace ToolText
} // namespace Cli
