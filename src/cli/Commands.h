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
};

struct Command {
    QString name;
    QString summary;
    QVector<Param> params;
    std::function<QJsonObject(const QJsonObject& args, Context& ctx)> run; // throws Error
};

const QVector<Command>& commands();
const Command* find(const QString& name);

// JSON schema of the parameters (MCP inputSchema, `help`)
QJsonObject inputSchema(const Command& cmd);
// Convert/validate raw arguments (command line strings or MCP JSON) to the declared types; throws Error
QJsonObject normalize(const Command& cmd, const QJsonObject& raw);

} // namespace Cli
