// SPDX-License-Identifier: AGPL-3.0-only
#include "cli/DirectBackendReal.h"

#include "cli/Live.h"
#include "core/I18n.h"
#include "engine/Extensions.h"

#include <QJsonArray>
#include <QPointer>
#include <QTimer>

#include <atomic>
#include <memory>

namespace {

QJsonObject canceledReply()
{
    return {{"ok", false}, {"error", QJsonObject{{"code", "CANCELED"}, {"message", "canceled"}}}};
}

// A command for the user on the UI thread, one undo step - a moment later, so the page shows its "building" text
// first. cancel before it starts: CANCELED without running; while it runs the flag reaches its analyses
DirectBackend::JobHandle runLater(LiveHost* host, const QString& command, const QJsonObject& args, const QString& undoText,
                                  const DirectBackend::Progress& onProgress, const DirectBackend::Done& onDone,
                                  QObject* receiver)
{
    auto stop = std::make_shared<std::atomic<bool>>(false);
    QPointer<QObject> alive(receiver);
    if (onProgress) onProgress(-1, T("Baut die Timeline …"));
    QTimer::singleShot(30, receiver, [=] {
        if (receiver && !alive) return;
        if (stop->load()) {
            onDone(canceledReply());
            return;
        }
        const QJsonObject reply = Cli::Live::runForUser(host, command, args, undoText, stop.get());
        if (receiver && !alive) return;
        onDone(stop->load() && !reply.value("ok").toBool() ? canceledReply() : reply);
    });
    return {[stop] { stop->store(true); }};
}

bool whisperThere() { return !Extensions::whisperProgram().isEmpty() && !Extensions::installedWhisperModels().isEmpty(); }

} // namespace

RealDirectBackend::RealDirectBackend(LiveHost* host) : m_host(host) {}

QStringList RealDirectBackend::recipes() const { return {"short", "highlights", "talk", "template", "script"}; }

DirectBackend::JobHandle RealDirectBackend::suggest(const QString& recipe, const QJsonObject& material,
                                                    const QJsonObject& options, const Progress& onProgress,
                                                    const Done& onDone, QObject* receiver)
{
    QJsonObject args{{"recipe", recipe}, {"options", options}};
    if (material.contains("files")) args["files"] = material["files"];
    if (material.contains("timeline")) args["timeline"] = material["timeline"];
    // A timeline, the project's media or its slots: the background job's copy of the app's project
    if (material.contains("timeline") || recipe == QLatin1String("talk") || recipe == QLatin1String("template")
        || recipe == QLatin1String("script"))
        args["project"] = Cli::Live::AppProject;
    const Cli::Live::JobHandle h = Cli::Live::runInBackground("direct", args, onProgress, onDone, m_host, receiver);
    return {[h] { h.cancel(); }};
}

DirectBackend::JobHandle RealDirectBackend::apply(const QJsonObject& session, const QJsonObject& choice,
                                                  const Progress& onProgress, const Done& onDone, QObject* receiver)
{
    QJsonObject args{{"session", session}, {"apply", choice}};
    if (!projectFolder().isEmpty()) args["project_dir"] = projectFolder();
    return runLater(m_host, "direct", args, T("Regie: Übernehmen"), onProgress, onDone, receiver);
}

bool RealDirectBackend::canFinish(const QJsonObject& session) const
{
    if (session.value("recipe").toString() != QLatin1String("short")) return false;
    const QJsonObject o = session.value("options").toObject();
    const bool captions = o.value("captions").toString("karaoke") != QLatin1String("off") && whisperThere();
    return captions || o.value("sfx").toString() == QLatin1String("mild");
}

DirectBackend::JobHandle RealDirectBackend::finish(const QJsonObject& session, const QJsonObject& applied,
                                                   const Progress& onProgress, const Done& onDone, QObject* receiver)
{
    const QJsonObject o = session.value("options").toObject();
    const QString timeline = applied.value("timeline").toString();
    QJsonObject f{{"timeline", timeline},
                  {"captions", whisperThere() ? o.value("captions").toString("karaoke") : QString("off")},
                  {"sfx", o.value("sfx").toString("off")},
                  {"session", session},
                  {"prepare", true}};
    if (o.contains("language")) f["language"] = o.value("language");
    // 1. In the background: speech (whisper), the sound's peak, the kill feed - the ops with their numbers
    // 2. On the UI thread, one undo step: only those ops, refused when the timeline changed meanwhile
    struct State {
        DirectBackend::JobHandle step;
        bool stopped = false;
    };
    auto state = std::make_shared<State>();
    std::weak_ptr<State> weak = state;
    LiveHost* host = m_host;
    const Cli::Live::JobHandle measure = Cli::Live::runInBackground(
        "direct", {{"project", Cli::Live::AppProject}, {"finish", f}}, onProgress,
        [weak, host, timeline, onProgress, onDone, receiver](const QJsonObject& reply) {
            const std::shared_ptr<State> st = weak.lock();
            if (!reply.value("ok").toBool() || !st || st->stopped) {
                onDone(reply.value("ok").toBool() ? canceledReply() : reply);
                return;
            }
            const QJsonObject r = reply.value("result").toObject();
            QJsonArray notes = r.value("notes").toArray();
            const QJsonArray noteKeys = r.value("note_keys").toArray(); // the page translates them
            if (r.value("ops").toArray().isEmpty()) { // nothing to add (no speech, nothing chosen)
                onDone({{"ok", true},
                        {"result", QJsonObject{{"timeline", timeline}, {"notes", notes}, {"note_keys", noteKeys}, {"done", QJsonArray{}}}}});
                return;
            }
            st->step = runLater(host, "direct",
                                {{"finish", QJsonObject{{"timeline", timeline}, {"ops", r.value("ops")}, {"stamp", r.value("stamp")}}}},
                                T("Regie: Untertitel und Effektklänge"), onProgress,
                                [onDone, notes, noteKeys](const QJsonObject& edited) {
                                    if (!edited.value("ok").toBool()) return onDone(edited);
                                    QJsonObject res = edited.value("result").toObject();
                                    if (!notes.isEmpty()) res["notes"] = notes;
                                    if (!noteKeys.isEmpty()) res["note_keys"] = noteKeys;
                                    onDone({{"ok", true}, {"result", res}});
                                },
                                receiver);
        },
        m_host, receiver);
    // The caller keeps the handle: it holds the state (the callbacks only see it weakly, no cycle)
    return {[state, measure] {
        state->stopped = true;
        measure.cancel();
        if (state->step.cancel) state->step.cancel();
    }};
}
