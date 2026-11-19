// SPDX-License-Identifier: AGPL-3.0-only
// FAKE backend of the Direct page (see DirectBackend.h): replaced by the `direct` command (package R1).
#include "app/DirectBackend.h"

#include "core/I18n.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <algorithm>
#include <memory>

FakeDirectBackend::FakeDirectBackend(UserRunner runner, std::function<double(const QString&)> lengthOf, int stepMs)
    : m_runner(std::move(runner)), m_lengthOf(std::move(lengthOf)), m_stepMs(std::max(0, stepMs))
{
}

namespace {

QString kindOf(const QString& recipe)
{
    if (recipe == "talk") return "pause";
    if (recipe == "template") return "slot";
    if (recipe == "script") return "take";
    return "moment";
}

// One reason per fake moment, in turn: every key the page translates shows up
QJsonObject reasonFor(const QString& kind, int i, double len)
{
    if (kind == "pause")
        return {{"key", "pause"}, {"values", QJsonObject{{"s", len}}}, {"text", QString("pause (%1 s)").arg(len, 0, 'f', 1)}};
    if (kind == "slot") return {{"key", "slot_match"}, {"values", QJsonObject{{"slot", i + 1}}}, {"text", "fits the slot"}};
    if (kind == "take") return {{"key", "take"}, {"values", QJsonObject{{"n", i + 1}}}, {"text", "best take"}};
    switch (i % 4) {
    case 0: return {{"key", "kills"}, {"values", QJsonObject{{"n", 2}, {"extra", "multi"}}}, {"text", "2 kills (multi kill)"}};
    case 1: return {{"key", "laughter"}, {"values", QJsonObject{{"s", 2.1}}}, {"text", "laughter (2.1 s)"}};
    case 2: return {{"key", "loud"}, {"values", QJsonObject{{"n", 3}}}, {"text", "3 loud moments"}};
    default: return {{"key", "motion"}, {"values", QJsonObject{}}, {"text", "lots of movement"}};
    }
}

} // namespace

QJsonObject FakeDirectBackend::makeSession(const QString& recipe, const QJsonObject& material, const QJsonObject& options) const
{
    QStringList files;
    for (const QJsonValue& v : material.value("files").toArray()) files << v.toString();
    if (files.isEmpty() && material.contains("timeline")) files << material.value("timeline").toString();
    const QString kind = kindOf(recipe);
    QJsonArray mat, suggestions, order;
    double sum = 0;
    int n = 0;
    for (const QString& file : files) {
        double len = m_lengthOf ? m_lengthOf(file) : 0;
        if (len <= 0) len = 30;
        mat.append(QJsonObject{{"file", file},
                               {"length_s", len},
                               {"has_video", true},
                               {"has_audio", true},
                               {"analysis", QJsonObject{{"sound", "done"}, {"picture", "done"}, {"events", "done"},
                                                        {"speech", recipe == "talk" ? "done" : "none"}}}});
        // Two pieces per file: at 20-40 % and 60-80 % of it (the second one off, like a weaker moment)
        for (int k = 0; k < 2; ++k) {
            const double from = len * (k == 0 ? 0.2 : 0.6), to = len * (k == 0 ? 0.4 : 0.8);
            ++n;
            const bool on = k == 0;
            QJsonObject s{{"n", n},
                          {"kind", kind},
                          {"file", file},
                          {"stream", 0},
                          {"from_s", from},
                          {"to_s", to},
                          {"score", 30.0 - n},
                          {"reason", reasonFor(kind, n - 1, to - from)},
                          {"labels", QJsonArray{}},
                          {"on", on}};
            if (recipe == "short") s["variant"] = 1;
            suggestions.append(s);
            if (on) {
                order.append(n);
                sum += to - from;
            }
        }
    }
    QJsonObject session{{"id", QString("d%1").arg(m_sessions + 1)},
                        {"recipe", recipe},
                        {"made", QDateTime::currentDateTime().toString(Qt::ISODate)},
                        {"options", options},
                        {"material", mat},
                        {"suggestions", suggestions},
                        {"order", order},
                        {"length_s", sum},
                        {"notes", QJsonArray{QJsonObject{{"key", "fake"}, {"text", "FAKE suggestions (no analysis yet)"}}}}};
    if (recipe == "short") session["target_s"] = options.value("max_s").toDouble(45);
    return session;
}

DirectBackend::JobHandle FakeDirectBackend::suggest(const QString& recipe, const QJsonObject& material,
                                                    const QJsonObject& options, const Progress& onProgress,
                                                    const Done& onDone, QObject* receiver)
{
    QJsonObject reply;
    if (recipe != "short" && recipe != "highlights" && recipe != "talk" && recipe != "template" && recipe != "script")
        reply = {{"ok", false}, {"error", QJsonObject{{"code", "UNKNOWN_RECIPE"}, {"message", "unknown recipe " + recipe}}}};
    else if (material.value("files").toArray().isEmpty() && !material.contains("timeline"))
        reply = {{"ok", false}, {"error", QJsonObject{{"code", "NO_MATERIAL"}, {"message", "no material"}}}};
    else {
        reply = {{"ok", true},
                 {"result", QJsonObject{{"session", makeSession(recipe, material, options)},
                                        {"next", "direct session:'…' apply:{on:'all'}"}}}};
        ++m_sessions;
    }
    // A few progress steps on a timer, then the reply; cancel() stops it with CANCELED
    struct State {
        int step = 0;
        bool over = false;
    };
    auto state = std::make_shared<State>();
    auto* timer = new QTimer(receiver);
    QPointer<QTimer> guard(timer);
    const int steps = 4;
    QObject::connect(timer, &QTimer::timeout, receiver, [=] {
        if (state->over) return;
        if (state->step < steps) {
            if (onProgress) onProgress(double(state->step) / steps, state->step < 2 ? "sound" : "picture");
            ++state->step;
            return;
        }
        state->over = true;
        timer->deleteLater();
        if (onDone) onDone(reply);
    });
    timer->start(m_stepMs);
    JobHandle h;
    h.cancel = [state, guard, onDone] {
        if (state->over) return;
        state->over = true;
        if (guard) guard->deleteLater();
        else return; // the receiver is gone: nothing comes
        if (onDone) onDone({{"ok", false}, {"error", QJsonObject{{"code", "CANCELED"}, {"message", "canceled"}}}});
    };
    return h;
}

QJsonObject FakeDirectBackend::apply(const QJsonObject& session, const QJsonObject& choice)
{
    if (!m_runner)
        return {{"ok", false}, {"error", QJsonObject{{"code", "NO_RUNNER"}, {"message", "no user runner"}}}};
    // The checked suggestions in the chosen order, with adjusted ranges
    QHash<int, QJsonObject> byN;
    for (const QJsonValue& v : session.value("suggestions").toArray()) byN.insert(v.toObject().value("n").toInt(), v.toObject());
    for (const QJsonValue& v : choice.value("adjust").toArray()) {
        const QJsonObject a = v.toObject();
        const int n = a.value("n").toInt();
        if (!byN.contains(n)) continue;
        byN[n]["from_s"] = a.value("from_s");
        byN[n]["to_s"] = a.value("to_s");
    }
    QList<int> order;
    const QJsonValue on = choice.value("on");
    for (const QJsonValue& v : choice.value("order").toArray()) order << v.toInt();
    if (on.isArray()) {
        QList<int> wanted;
        for (const QJsonValue& v : on.toArray()) wanted << v.toInt();
        order.erase(std::remove_if(order.begin(), order.end(), [&](int n) { return !wanted.contains(n); }), order.end());
        for (int n : wanted)
            if (!order.contains(n)) order << n;
    }
    QJsonArray ops{QJsonObject{{"op", "new_timeline"}, {"name", "Regie " + session.value("recipe").toString()}}};
    int clips = 0;
    for (int n : order) {
        const QJsonObject s = byN.value(n);
        const QString file = s.value("file").toString();
        if (!QFileInfo(file).isFile()) continue; // the fake places only files
        ops.append(QJsonObject{{"op", "append"},
                               {"media", file},
                               {"in", QString("%1s").arg(s.value("from_s").toDouble(), 0, 'f', 3)},
                               {"out", QString("%1s").arg(s.value("to_s").toDouble(), 0, 'f', 3)}});
        ++clips;
    }
    QJsonObject reply = m_runner("edit", {{"project", "@app"}, {"ops", ops}}, T("Regie übernehmen"));
    if (reply.value("ok").toBool()) {
        ++m_applied;
        QJsonObject r = reply.value("result").toObject();
        r["clips"] = clips;
        reply["result"] = r;
    }
    return reply;
}
