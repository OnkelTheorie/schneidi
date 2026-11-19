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

QStringList RealDirectBackend::recipes() const { return {"short", "highlights"}; }

DirectBackend::JobHandle RealDirectBackend::suggest(const QString& recipe, const QJsonObject& material,
                                                    const QJsonObject& options, const Progress& onProgress,
                                                    const Done& onDone, QObject* receiver)
{
    QJsonObject args{{"recipe", recipe}, {"options", options}};
    if (material.contains("files")) args["files"] = material["files"];
    if (material.contains("timeline")) {
        args["timeline"] = material["timeline"];
        args["project"] = Cli::Live::AppProject;
    }
    const Cli::Live::JobHandle h = Cli::Live::runInBackground("direct", args, onProgress, onDone, m_host, receiver);
    return {[h] { h.cancel(); }};
}

DirectBackend::JobHandle RealDirectBackend::apply(const QJsonObject& session, const QJsonObject& choice,
                                                  const Progress& onProgress, const Done& onDone, QObject* receiver)
{
    return runLater(m_host, "direct", {{"session", session}, {"apply", choice}}, T("Regie: Übernehmen"), onProgress,
                    onDone, receiver);
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
    QJsonObject f{{"timeline", applied.value("timeline")},
                  {"captions", whisperThere() ? o.value("captions").toString("karaoke") : QString("off")},
                  {"sfx", o.value("sfx").toString("off")}};
    for (const char* k : {"game", "language"})
        if (o.contains(k)) f[k] = o.value(k);
    // The speech is read in the background (whisper takes a while), the captions and sounds go on as one undo step
    auto handle = std::make_shared<DirectBackend::JobHandle>();
    auto stopped = std::make_shared<bool>(false);
    LiveHost* host = m_host;
    const auto edit = [=](QJsonObject finishArgs) {
        if (*stopped) {
            onDone(canceledReply());
            return;
        }
        *handle = runLater(host, "direct", {{"finish", finishArgs}}, T("Regie: Untertitel und Effektklänge"), onProgress,
                           onDone, receiver);
    };
    if (f.value("captions").toString() == QLatin1String("off")) {
        edit(f);
    } else {
        QJsonObject t{{"source", Cli::Live::AppProject}, {"timeline", f.value("timeline")}, {"words", true}};
        if (o.contains("language")) t["language"] = o.value("language");
        const Cli::Live::JobHandle h = Cli::Live::runInBackground(
            "transcribe", t, onProgress,
            [=](const QJsonObject& reply) mutable {
                if (!reply.value("ok").toBool()) {
                    const QString code = reply.value("error").toObject().value("code").toString();
                    if (code != QLatin1String("NO_SPEECH")) {
                        onDone(reply);
                        return;
                    }
                    f["words"] = QJsonArray{};
                } else {
                    f["words"] = reply.value("result").toObject().value("words").toArray();
                }
                edit(f);
            },
            m_host, receiver);
        *handle = {[h] { h.cancel(); }};
    }
    return {[handle, stopped] {
        *stopped = true;
        if (handle->cancel) handle->cancel();
    }};
}
