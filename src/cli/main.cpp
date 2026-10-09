// schneidi-cli: schneidi without a window, for scripts and AIs. Same project files and editing logic as the app.
//   schneidi-cli help                      all commands (JSON)
//   schneidi-cli info film.schneidi        timeline as JSON
//   schneidi-cli mcp                       MCP server on stdio (Claude Desktop, Claude Code, …)
// stdout carries only JSON; logs and progress go to stderr.
#include "buildinfo.h"
#include "cli/Commands.h"
#include "cli/Mcp.h"
#include "core/I18n.h"
#include "engine/Engine.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <clocale>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <string>

namespace {

void print(const QJsonObject& o, bool pretty)
{
    const QByteArray out = QJsonDocument(o).toJson(pretty ? QJsonDocument::Indented : QJsonDocument::Compact);
    std::fwrite(out.constData(), 1, size_t(out.size()), stdout);
    if (!pretty) std::fputc('\n', stdout);
    std::fflush(stdout);
}

QJsonObject errorJson(const QString& code, const QString& message)
{
    return {{"ok", false}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
}

// Command line -> raw JSON arguments: positional values fill the positional parameters in order
// (the last one takes all remaining values if it is a list), --name value / --flag set the others.
// "-" as a value reads it from stdin (e.g. edit operations).
QJsonObject parseArgs(const Cli::Command& cmd, const QStringList& args)
{
    QJsonObject raw;
    QVector<const Cli::Param*> positional;
    for (const Cli::Param& p : cmd.params)
        if (p.positional) positional << &p;
    int next = 0;
    const auto readStdin = [] {
        std::string s((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
        return QString::fromStdString(s);
    };
    for (int i = 0; i < args.size(); ++i) {
        const QString& a = args[i];
        if (a.startsWith("--") && a.size() > 2) {
            QString name = a.mid(2).replace('-', '_');
            QString value;
            if (const int eq = name.indexOf('='); eq > 0) {
                value = name.mid(eq + 1);
                name = name.left(eq);
            } else {
                const Cli::Param* p = nullptr;
                for (const Cli::Param& x : cmd.params)
                    if (x.name == name) p = &x;
                const bool flag = p && p->type == Cli::ParamType::Boolean;
                if (flag && (i + 1 >= args.size() || args[i + 1].startsWith("--"))) value = "true";
                else if (i + 1 < args.size()) value = args[++i];
                else throw Cli::Error{"USAGE", "missing value for --" + name};
            }
            raw[name] = value == "-" ? readStdin() : value;
            continue;
        }
        if (next >= positional.size()) throw Cli::Error{"USAGE", "unexpected argument: " + a};
        const Cli::Param* p = positional[next];
        const bool list = p->type == Cli::ParamType::Paths || p->type == Cli::ParamType::Times;
        if (list) {
            QJsonArray arr = raw[p->name].toArray();
            arr << a;
            raw[p->name] = arr;
        } else {
            raw[p->name] = a == "-" ? readStdin() : a;
            ++next;
        }
    }
    return raw;
}

} // namespace

int main(int argc, char* argv[])
{
    // No window, no sound: titles still need Qt's painting (fonts), hence a GUI application on the offscreen platform
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    if (qEnvironmentVariableIsEmpty("SDL_AUDIODRIVER")) qputenv("SDL_AUDIODRIVER", "dummy");
    qputenv("SDL_NO_SIGNAL_HANDLERS", "1");
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName("schneidi"); // same backups, effects and settings as the app
#ifdef Q_OS_WIN
    QGuiApplication::setOrganizationName("schneidi");
#endif
    QGuiApplication::setApplicationVersion(QString("%1 (%2)").arg(SCHNEIDI_VERSION, SCHNEIDI_BUILD));
    I18n::install("en");
    std::setlocale(LC_NUMERIC, "C"); // MLT reads numbers with a dot

    QStringList args = app.arguments().mid(1);
    bool pretty = false;
    if (args.removeAll("--pretty")) pretty = true;
    if (args.isEmpty() || args.first() == "--help" || args.first() == "-h") args = {"help"};
    if (args.first() == "--version") {
        print({{"ok", true}, {"version", QCoreApplication::applicationVersion()}, {"api_version", Cli::ApiVersion}}, pretty);
        return 0;
    }
    const QString name = args.takeFirst();

    if (name != "help") {
        QString error;
        if (!Engine::initMlt(&error)) {
            print(errorJson("MLT_INIT", error), pretty);
            return 1;
        }
    }
    if (name == "mcp") return Cli::runMcp();

    const Cli::Command* cmd = Cli::find(name);
    if (!cmd) {
        print(errorJson("USAGE", QString("unknown command '%1' (see: schneidi-cli help)").arg(name)), pretty);
        return 2;
    }
    Cli::Context ctx;
    int lastPercent = -1;
    ctx.progress = [&lastPercent](double p) {
        const int percent = int(p * 100);
        if (percent == lastPercent) return;
        lastPercent = percent;
        std::fprintf(stderr, "{\"progress\":%d}\n", percent);
        std::fflush(stderr);
    };
    QJsonObject args0;
    try {
        args0 = Cli::normalize(*cmd, parseArgs(*cmd, args));
    } catch (const Cli::Error& e) {
        print(errorJson(e.code, e.message), pretty);
        return 2;
    }
    try {
        QJsonObject result = cmd->run(args0, ctx);
        result.insert("ok", true);
        print(result, pretty);
        return 0;
    } catch (const Cli::Error& e) {
        print(errorJson(e.code, e.message), pretty);
        return 1;
    }
}
