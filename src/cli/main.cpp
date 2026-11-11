// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli: schneidi without a window, for scripts and AIs. Same project files and editing logic as the app.
//   schneidi-cli help                      the commands in short; help <command>: its parameters (JSON)
//   schneidi-cli info film.schneidi        timeline as JSON
//   schneidi-cli mcp                       MCP server on stdio (Claude Desktop, Claude Code, …)
//   schneidi-cli serve --root /data        MCP server over HTTP (AIs on other devices, Docker), cli/Serve.h
//   schneidi-cli setup claude              connect Claude Code / Claude Desktop (MCP server + guide)
// stdout carries only JSON; logs and progress go to stderr.
#include "buildinfo.h"
#include "cli/Commands.h"
#include "cli/Live.h"
#include "cli/Mcp.h"
#include "cli/Sandbox.h"
#include "cli/Serve.h"
#include "cli/Setup.h"
#include "core/CodeTrust.h"
#include "core/I18n.h"
#include "core/SettingsIsolation.h"
#include "engine/Engine.h"
#include "engine/FfmpegLog.h"
#include "engine/Licenses.h"
#include "engine/picture/gpu/Gpu.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <iostream>
#include <iterator>
#include <string>

#ifdef Q_OS_WIN
#include <io.h> // _isatty
#else
#include <unistd.h> // isatty
#endif

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
            if (value == "-") value = readStdin();
            if (raw.contains(name)) {
                // Given twice: a list takes both (--at 0 --at 2s = --at 0,2s); anything else is refused. The last
                // one silently replaced the first, so `frames --at 0 --at 2s` showed only 2 s.
                const Cli::Param* p = nullptr;
                for (const Cli::Param& x : cmd.params)
                    if (x.name == name) p = &x;
                const bool list = p && (p->type == Cli::ParamType::Times || p->type == Cli::ParamType::Paths
                                        || p->type == Cli::ParamType::Sources);
                // (", " for times: "0,1s" would be 0.1 s in German writing)
                if (list && raw[name].isString())
                    value = raw[name].toString() + (p->type == Cli::ParamType::Times ? ", " : ",") + value;
                else if (raw[name].toString() != value)
                    throw Cli::Error{"USAGE", "--" + a.mid(2).section('=', 0, 0) + " is given twice"};
            }
            raw[name] = value;
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
    SettingsIsolation::apply(); // test runs only (SCHNEIDI_TEST_SETTINGS): never the user's settings, before any use
    // No window, no sound: titles still need Qt's painting (fonts), hence a GUI application on the offscreen platform
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName("schneidi"); // same backups, effects and settings as the app
#ifdef Q_OS_WIN
    QGuiApplication::setOrganizationName("schneidi");
#endif
    QGuiApplication::setApplicationVersion(QString::fromUtf8(SCHNEIDI_VERSION_LABEL) + (*SCHNEIDI_BUILD ? QString(" (%1)").arg(SCHNEIDI_BUILD) : QString()));
    I18n::install("en");
    std::setlocale(LC_NUMERIC, "C"); // C libraries (FFmpeg option parsing) read numbers with a dot
    CodeTrust::prune(); // trust records of deleted files (at most once a day)

    QStringList args = app.arguments().mid(1);
    bool pretty = false;
    if (args.removeAll("--pretty")) pretty = true;
    // Progress notes ("Export: libx264, 12 threads ...") only with --verbose; warnings and errors always go to stderr
    if (args.removeAll("--verbose")) qputenv("SCHNEIDI_VERBOSE", "1");
    else QLoggingCategory::setFilterRules(QStringLiteral("*.debug=false\n*.info=false"));
    if (args.isEmpty() || args.first() == "--help" || args.first() == "-h") args = {"help"};
    if (args.first() == "--version") {
        print({{"ok", true}, {"version", QCoreApplication::applicationVersion()}, {"api_version", Cli::ApiVersion}}, pretty);
        return 0;
    }
    QString name = args.takeFirst();
    // `template <action> [value] …` = `templates --<action> value …` (info, fill, save, import, export, remove, rename;
    // list = all)
    if (name == "template") {
        name = "templates";
        if (!args.isEmpty() && !args.first().startsWith("--")) {
            const QString action = args.takeFirst().replace('-', '_');
            if (action != "list") {
                if (args.isEmpty() || args.first().startsWith("--")) {
                    print(errorJson("USAGE", QString("template %1 needs a value (template id, project or file)").arg(action)), pretty);
                    return 2;
                }
                args = QStringList{"--" + action, args.takeFirst()} + args;
            }
        }
    }
    if (name == "setup") return Cli::Setup::run(args, pretty);
    // A person at a terminal (stderr is one, no AI tool runs us): a short tip when Claude is here but not connected -
    // the same offer rules as the app's question (a few times, two weeks apart). Never in mcp/serve, never on stdout.
#ifdef Q_OS_WIN
    const bool terminal = _isatty(_fileno(stderr));
#else
    const bool terminal = isatty(fileno(stderr));
#endif
    if (terminal && name != "mcp" && name != "serve" && qEnvironmentVariableIsEmpty("CLAUDECODE")
        && Cli::Setup::offerDue()) {
        Cli::Setup::offerShown();
        std::fprintf(stderr, "Tip: %s is installed. Connect it to schneidi: schneidi-cli setup claude\n"
                             "     (no more tips: schneidi-cli setup claude --never)\n",
                     qPrintable(Cli::Setup::claudeFound().join(" and ")));
    }

    // The HTTP server only routes: the engine runs in its workers (`mcp` processes)
    if (name == "serve") return Cli::Serve::run(args);
    if (name != "help") {
        FfmpegLog::install(nullptr);
        Engine::initHeadless();
    }
    // Third-party licenses of what this program really loads (FFmpeg's configuration, GPL libraries);
    // exit code 1 when a SCHNEIDI_NO_GPL build finds anything GPL (the check of the bundled packages)
    if (name == "--licenses") {
        QJsonArray parts, gpl;
        for (const Licenses::Part& p : Licenses::report()) {
            parts << QJsonObject{{"name", p.name}, {"license", p.license}, {"gpl", p.gpl}, {"path", p.path}};
            if (p.gpl) gpl << p.name;
        }
        const bool ok = !Licenses::builtWithoutGpl() || gpl.isEmpty();
        print({{"ok", ok}, {"no_gpl_build", Licenses::builtWithoutGpl()}, {"gpl", gpl}, {"parts", parts}}, pretty);
        return ok ? 0 : 1;
    }
    if (name == "mcp") {
        Cli::Mcp::Options opt;
        try {
            const auto o = Cli::Serve::parseOptions(args, {"root", "map"}, {"no-extensions"});
            Cli::Sandbox::configure({o.value("root").value(0), o.value("map")});
            opt.root = Cli::Sandbox::root();
            opt.app = Cli::Sandbox::appAllowed();
            opt.extensions = !o.contains("no-extensions");
        } catch (const Cli::Error& e) {
            print(errorJson(e.code, e.message), pretty);
            return 2;
        }
        Cli::Live::setClientName("schneidi-cli mcp");
        return Cli::Mcp::run(opt);
    }

    const Cli::Command* cmd = Cli::find(name);
    if (!cmd) {
        print(errorJson("USAGE", QString("unknown command '%1' (see: schneidi-cli help)").arg(name)), pretty);
        return 2;
    }
    Cli::Context ctx;
    // Progress lines on stderr: {"progress":45,"stage":"transcribe","eta_s":30}; a line per new percent or stage.
    // eta_s: from the time so far, once there is something to go by (steps of `short` run at different speeds: rough)
    struct ProgressState {
        std::mutex lock; // some analyses report from their worker threads
        int percent = -1;
        double last = 0;
        QString stage, printed;
        QElapsedTimer clock;
    };
    auto state = std::make_shared<ProgressState>();
    state->clock.start();
    state->stage = name;
    const auto printProgress = [state](double p) { // with state->lock held
        p = std::clamp(p, 0.0, 1.0);
        state->last = p;
        const int percent = int(p * 100);
        if (percent == state->percent && state->stage == state->printed) return;
        state->percent = percent;
        state->printed = state->stage;
        QJsonObject line{{"progress", percent}};
        if (!state->stage.isEmpty()) line["stage"] = state->stage;
        const double elapsed = state->clock.elapsed() / 1000.0;
        if (p >= 0.02 && p < 1 && elapsed >= 2) line["eta_s"] = int(std::lround(elapsed * (1 - p) / p));
        std::fprintf(stderr, "%s\n", QJsonDocument(line).toJson(QJsonDocument::Compact).constData());
        std::fflush(stderr);
    };
    ctx.progress = [state, printProgress](double p) {
        const std::lock_guard<std::mutex> hold(state->lock);
        printProgress(p);
    };
    ctx.stage = [state, printProgress](const QString& stage) {
        const std::lock_guard<std::mutex> hold(state->lock);
        if (stage == state->stage) return;
        state->stage = stage;
        if (state->percent >= 0) printProgress(state->last); // a new step: say so at once (not for the first command)
    };
    ctx.line = [](const QJsonObject& o) { print(o, false); };
    QJsonObject args0;
    try {
        args0 = Cli::normalize(*cmd, parseArgs(*cmd, args));
    } catch (const Cli::Error& e) {
        print(errorJson(e.code, e.message), pretty);
        return 2;
    }
    try {
        QJsonObject result = Cli::execute(*cmd, args0, ctx);
        result.insert("ok", true);
        print(result, pretty);
        return 0;
    } catch (const Cli::Error& e) {
        print(errorJson(e.code, e.message), pretty);
        return 1;
    }
}
