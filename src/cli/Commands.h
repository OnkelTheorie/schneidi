#pragma once
// Command layer of schneidi-cli: every command is defined once (name, parameters, handler) and serves both
// frontends, the command line (`schneidi-cli <command> …`) and the MCP server (`schneidi-cli mcp`, one tool per
// command). Input and output are JSON; frame numbers count in the project frame rate.

#include <QImage>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <functional>

namespace Cli {

inline constexpr int ApiVersion = 1;

// Failure of a command: stable code for programs, message for people (and AIs)
struct Error {
    QString code;    // e.g. "NOT_FOUND", "BAD_ARGUMENT", "EXISTS", "RENDER_FAILED"
    QString message;
};

enum class ParamType {
    String,
    Path,    // file or folder, made absolute
    Integer,
    Number,
    Boolean,
    Time,    // frames (integer), "12.5s" (seconds) or timecode "00:01:02:03"
    Times,   // list of times
    Paths,   // list of files
    Sources, // list of files, each optionally an object {file, …settings for this file}
    Json,    // any JSON value (edit operations)
};

struct Param {
    QString name;
    ParamType type = ParamType::String;
    QString help;
    bool required = false;
    bool positional = false; // command line: given without --name (in order)
};

// Running a command: images collected for the MCP answer, progress for long jobs
struct Context {
    bool mcp = false;
    QVector<QImage> images; // frames tool: shown to the AI directly
    std::function<void(double)> progress; // 0..1, may be empty
    // Command line only (empty in MCP): results printed as they come, one JSON line each (`watch` follow)
    std::function<void(const QJsonObject&)> line;
};

struct Command {
    QString name;
    QString summary;
    QVector<Param> params;
    std::function<QJsonObject(const QJsonObject& args, Context& ctx)> run; // throws Error
};

const QVector<Command>& commands();
const Command* find(const QString& name);

// Runs a command: if its project is open in the running app (cli/Live.h), changing commands run inside the app
// and reading ones on a copy of what the app shows; otherwise on the file. Both frontends call this.
QJsonObject execute(const Command& cmd, const QJsonObject& args, Context& ctx);
// Commands that change the project (or control the app) and therefore run inside the app when it has it open
bool runsInApp(const Command& cmd, const QJsonObject& args);

// JSON schema of the parameters (MCP inputSchema, `help`)
QJsonObject inputSchema(const Command& cmd);
// Convert/validate raw arguments (command line strings or MCP JSON) to the declared types; throws Error
QJsonObject normalize(const Command& cmd, const QJsonObject& raw);

} // namespace Cli
