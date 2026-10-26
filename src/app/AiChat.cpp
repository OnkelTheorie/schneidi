#include "app/AiChat.h"

#include "app/AiClient.h"
#include "app/AiLog.h"
#include "app/McpClient.h"
#include "core/I18n.h"

#include <QBuffer>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>

#include <algorithm>

namespace {

QString pretty(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented)).trimmed(); }

QString cut(const QString& s, int max) { return s.size() <= max ? s : s.left(max) + " …"; }

// The tool's error message inside its JSON answer ({"ok":false,"error":{"message":…}}), else the text itself
QString errorOf(const QString& text)
{
    const QJsonObject o = QJsonDocument::fromJson(text.toUtf8()).object();
    const QString m = o["error"]["message"].toString();
    return (m.isEmpty() ? text : m).simplified();
}

QString capitalized(QString s)
{
    if (!s.isEmpty()) s[0] = s[0].toUpper();
    return s;
}

// What a tool works on, for its line: a file or project ("@app" = the open project)
QString targetOf(const QJsonObject& args)
{
    for (const char* key : {"file", "source", "project"})
        if (const QString v = args.value(key).toString(); !v.isEmpty()) return AiLog::targetName(v);
    for (const char* key : {"files", "sources"}) {
        const QJsonArray list = args.value(key).toArray();
        if (list.isEmpty()) continue;
        const QJsonValue first = list.first();
        const QString name = AiLog::targetName(first.isObject() ? first["file"].toString() : first.toString());
        return list.size() > 1 ? QString("%1 +%2").arg(name).arg(list.size() - 1) : name;
    }
    return {};
}

// The ops of an edit: "split, delete"
QString opsOf(const QJsonObject& args)
{
    QJsonValue v = args.value("ops");
    if (v.isString()) v = QJsonDocument::fromJson(v.toString().toUtf8()).array();
    if (v.isObject()) v = QJsonArray{v};
    QStringList names;
    for (const QJsonValue& op : v.toArray())
        if (const QString n = op["op"].toString(); !n.isEmpty() && !names.contains(n)) names << n;
    if (names.size() > 4) names = names.mid(0, 4) << "…";
    return names.join(", ");
}

} // namespace

AiChat::AiChat(QObject* parent) : QObject(parent), m_client(new AiClient(this)), m_mcp(new McpClient(this))
{
    connect(m_client, &AiClient::text, this, &AiChat::onText);
    connect(m_client, &AiClient::finished, this, &AiChat::onReply);
    connect(m_client, &AiClient::failed, this, &AiChat::onFailed);
    connect(m_client, &AiClient::retrying, this, [this](int seconds, const QString& why) {
        addNotice(T("%1 Neuer Versuch in %2 s …").arg(why).arg(seconds));
    });
    connect(m_mcp, &McpClient::ready, this, &AiChat::toolsReady);
    connect(m_mcp, &McpClient::failed, this, [this](const QString& message) {
        if (!m_busy) return;
        addError(message);
        finishTurn();
    });
}

AiChat::~AiChat() = default;

void AiChat::setTools(const QStringList& command, const QHash<QString, QString>& environment)
{
    m_mcp->setCommand(command, environment);
}

void AiChat::setLanguage(const QString& englishName) { m_language = englishName; }

int AiChat::add(Entry e)
{
    m_entries << std::move(e);
    emit entryAdded(int(m_entries.size()) - 1);
    return int(m_entries.size()) - 1;
}

void AiChat::change(int index, const std::function<void(Entry&)>& fn)
{
    if (index < 0 || index >= m_entries.size()) return;
    fn(m_entries[index]);
    emit entryChanged(index);
}

void AiChat::addNotice(const QString& text) { add({Entry::Kind::Notice, text}); }

void AiChat::addError(const QString& text) { add({Entry::Kind::Error, text}); }

void AiChat::setBusy(bool on)
{
    if (m_busy == on) return;
    m_busy = on;
    emit busyChanged(on);
}

void AiChat::appendUser(const QString& text, const QVector<Ai::ToolResult>& results)
{
    // Tool answers start a user message; text of the user after them joins it (the roles alternate)
    if (!m_history.isEmpty() && m_history.last().role == Ai::Message::Role::User && results.isEmpty()) {
        Ai::Message& last = m_history.last();
        last.text = last.text.isEmpty() ? text : last.text + "\n\n" + text;
        return;
    }
    Ai::Message m;
    m.text = text;
    m.results = results;
    m_history << m;
}

bool AiChat::send(const QString& text, const Ai::Endpoint& endpoint, bool review)
{
    if (m_busy || text.trimmed().isEmpty()) return false;
    if (m_offline) {
        addError(T("Testlauf: der KI-Chat sendet nichts."));
        return false;
    }
    m_endpoint = endpoint;
    m_stopped = false;
    m_rounds = 0;
    m_request = text.trimmed();
    // Pictures of earlier requests are not sent again: the model has seen them, and a long chat would outgrow what
    // providers take per request (count and size of pictures). It can look again with `frames`.
    for (Ai::Message& m : m_history)
        for (Ai::ToolResult& t : m.results)
            if (!t.images.isEmpty()) {
                t.text += QString("\n[%1 picture(s) of this answer were in an earlier request and are not sent again]")
                              .arg(t.images.size());
                t.images.clear();
            }
    add({Entry::Kind::User, m_request});
    QString message = m_request;
    if (!m_notes.isEmpty()) {
        QStringList notes;
        for (const QString& n : m_notes) notes << QString("[Note from the schneidi app: %1]").arg(n);
        message = notes.join('\n') + "\n\n" + message;
        m_notes.clear();
    }
    if (!m_system.isEmpty() && review != m_review) // the prompt stays as it is for the whole chat
        message = QString("[Note from the schneidi app: %1]\n\n").arg(
                      review ? "from now on the user reviews your changes at the end of each request and accepts or "
                               "discards them together"
                             : "from now on your changes take effect right away, one undo step per edit")
                  + message;
    m_review = review;
    appendUser(message);
    setBusy(true);
    if (m_mcp->isReady()) toolsReady();
    else m_mcp->start();
    return true;
}

void AiChat::toolsReady()
{
    if (!m_busy || m_stopped || m_guide == Guide::Asking) return; // the guide's answer goes on
    if (m_guide == Guide::Read) {
        requestModel();
        return;
    }
    // The editing guide, once: it is the heart of the instructions (the same text as the `guide` tool)
    m_guide = Guide::Asking;
    m_mcp->call("guide", {}, {}, [this](const McpClient::Result& r) {
        m_guide = r.error ? Guide::No : Guide::Read; // stopped while it was read: asked again next time
        if (!r.error) {
            const QJsonObject o = QJsonDocument::fromJson(r.text.toUtf8()).object();
            m_guideText = o["guide"].toString();
            QStringList topics;
            for (const QJsonValue& t : o["topics"].toArray())
                topics << QString("- %1: %2").arg(t["topic"].toString(), t["title"].toString());
            if (!topics.isEmpty())
                m_guideText += "\n\nTask guides (call `guide` with the topic before such a task):\n" + topics.join('\n');
        }
        if (m_busy && !m_stopped) requestModel();
    });
}

QString AiChat::systemPrompt() const
{
    QStringList parts;
    if (!m_mcp->instructions().isEmpty()) parts << m_mcp->instructions();
    if (!m_guideText.isEmpty()) parts << m_guideText;
    QString app = QString(
                      "# You are the AI chat inside the schneidi app\n"
                      "The user writes to you in the AI panel of the schneidi app while their project is open there.\n"
                      "- Use \"@app\" as the project for every tool (the project open in the app, unsaved changes "
                      "included) unless the user names another file.\n"
                      "- Answer in %1 (the language of the user's schneidi interface) unless the user writes in "
                      "another language. Keep answers short and plain: the user sees your tool steps already. Say what "
                      "you changed and where.\n"
                      "- Every `edit` call is one undo step in the app: put one logical change into one call. Do not "
                      "save the project (`app` save) unless the user asks.\n")
                      .arg(m_language);
    if (!m_guideText.isEmpty())
        app += "- The editing guide above is the main text of the `guide` tool: call `guide` only with a topic.\n";
    app += m_review ? "- The user reviews your changes: when your answer is done, they accept all changes of the "
                      "request as one undo step or discard them. Do the whole request in one go instead of asking for "
                      "confirmation in between.\n"
                    : "- Your changes take effect right away; the user can undo each edit.\n";
    parts << app;
    return parts.join("\n\n");
}

void AiChat::requestModel()
{
    if (++m_rounds > kMaxRounds) {
        addNotice(T("Nach %1 Schritten angehalten. Schreib „weiter“, wenn die KI weitermachen soll.").arg(kMaxRounds));
        finishTurn();
        return;
    }
    if (m_system.isEmpty()) m_system = systemPrompt();
    Ai::Request r;
    r.model = m_endpoint.model;
    r.provider = m_endpoint.provider;
    r.system = m_system;
    r.tools = m_mcp->tools();
    r.messages = m_history;
    r.fallbacks = Ai::fallbacksFor(m_endpoint);
    m_streaming = -1;
    m_client->send(m_endpoint, r);
}

void AiChat::onText(const QString& delta)
{
    if (!m_busy) return;
    if (m_streaming < 0) {
        Entry e{Entry::Kind::Assistant, delta};
        e.state = Entry::State::Running;
        m_streaming = add(e);
        return;
    }
    change(m_streaming, [&delta](Entry& e) { e.text += delta; });
}

void AiChat::onReply(const Ai::Reply& reply)
{
    if (!m_busy) return;
    m_usage += reply.usage;
    emit usageChanged();
    // The whole text (also when it did not stream)
    if (m_streaming >= 0) {
        change(m_streaming, [&reply](Entry& e) {
            e.text = reply.text;
            e.state = Entry::State::Done;
        });
    } else if (!reply.text.trimmed().isEmpty()) {
        add({Entry::Kind::Assistant, reply.text});
    }
    m_streaming = -1;
    if (reply.refused()) {
        // The refused answer is not kept: its tool calls would stay unanswered and break every later request (the
        // providers want the partial output dropped); what the user writes next joins their last message
        addError(T("Das Modell hat die Anfrage abgelehnt."));
        finishTurn();
        return;
    }
    Ai::Message m;
    m.role = Ai::Message::Role::Assistant;
    m.text = reply.text;
    m.calls = reply.calls;
    if (m_endpoint.protocol == Ai::Protocol::Anthropic) m.anthropic = reply.anthropic;
    m_history << m;
    if (reply.calls.isEmpty()) {
        if (reply.stop == "model_context_window_exceeded")
            addNotice(T("Der Chat ist zu lang für das Modell geworden: „Neuer Chat“ fängt frisch an."));
        else if (reply.cutOff()) addNotice(T("Die Antwort wurde abgeschnitten (zu lang)."));
        else if (reply.text.trimmed().isEmpty()) addNotice(T("Die KI hat nichts geantwortet."));
        finishTurn();
        return;
    }
    m_calls = reply.calls;
    m_results.clear();
    nextTool();
}

void AiChat::onFailed(const QString& message)
{
    if (!m_busy) return;
    if (m_streaming >= 0) change(m_streaming, [](Entry& e) { e.state = Entry::State::Failed; });
    m_streaming = -1;
    addError(message);
    finishTurn();
}

void AiChat::nextTool()
{
    if (!m_busy || m_stopped) return;
    if (m_results.size() >= m_calls.size()) {
        appendUser({}, m_results);
        m_calls.clear();
        m_results.clear();
        requestModel();
        return;
    }
    if (!m_mcp->isReady()) {
        // The tool server went away (crashed): every call of this answer gets its answer, the turn ends; the next
        // request starts the server again (else the model would call dead tools round after round)
        for (int i = int(m_results.size()); i < m_calls.size(); ++i)
            m_results << Ai::ToolResult{m_calls[i].id, m_calls[i].name, "not run: the schneidi tools stopped", {}, true};
        appendUser({}, m_results);
        addError(T("Die Werkzeuge der KI sind ausgefallen. Schreib einfach weiter: sie starten dann neu."));
        finishTurn();
        return;
    }
    const Ai::ToolCall call = m_calls[m_results.size()];
    Entry e{Entry::Kind::Tool};
    e.tool = call.name;
    e.summary = toolLine(call.name, call.input);
    e.details = pretty(call.input);
    e.state = Entry::State::Running;
    const int entry = add(e);
    if (!call.error.isEmpty()) { // never reached the tool
        toolDone(entry, call, QString("could not read the arguments of this call: %1").arg(call.error), {}, true);
        return;
    }
    m_mcp->call(
        call.name, call.input,
        [this, entry](int percent) { change(entry, [percent](Entry& x) { x.percent = percent; }); },
        [this, entry, call](const McpClient::Result& r) { toolDone(entry, call, r.text, r.images, r.error); });
}

void AiChat::toolDone(int entry, const Ai::ToolCall& call, const QString& text, const QVector<Ai::Image>& images, bool error)
{
    if (!m_busy) return;
    change(entry, [&](Entry& e) {
        e.state = m_stopped ? Entry::State::Stopped : error ? Entry::State::Failed : Entry::State::Done;
        e.percent = -1;
        if (error && !m_stopped) e.summary += " – " + cut(errorOf(text), 160);
        e.details += "\n\n→ " + text; // whole (the panel shows the start)
        for (const Ai::Image& img : images)
            if (QImage pic = QImage::fromData(QByteArray::fromBase64(img.base64)); !pic.isNull())
                e.images << pic.scaledToHeight(std::min(pic.height(), 360), Qt::SmoothTransformation);
    });
    // The model reads English; a stopped call may still have happened inside the app
    const QString answer = m_stopped ? QString("stopped by the user (the call may have finished in the app anyway)") : text;
    m_results << capped({call.id, call.name, answer, m_stopped ? QVector<Ai::Image>() : images, error || m_stopped});
    nextTool();
}

Ai::ToolResult AiChat::capped(Ai::ToolResult r)
{
    if (r.text.size() > kMaxToolText) {
        const int n = r.text[kMaxToolText - 1].isHighSurrogate() ? kMaxToolText - 1 : kMaxToolText; // whole characters
        r.text = r.text.left(n)
                 + QString("\n[cut by the schneidi app: the answer has %1 characters; ask for less, e.g. `info` with "
                           "summary:true or a shorter range]")
                       .arg(r.text.size());
    }
    // Pictures no larger than providers take in any request (a tall contact sheet, a 4K still)
    for (Ai::Image& img : r.images) {
        QImage pic = QImage::fromData(QByteArray::fromBase64(img.base64));
        if (pic.isNull() || std::max(pic.width(), pic.height()) <= kMaxImageEdge) continue;
        pic = pic.scaled(kMaxImageEdge, kMaxImageEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QByteArray data;
        QBuffer buffer(&data);
        buffer.open(QIODevice::WriteOnly);
        const bool png = img.mime == "image/png";
        if (!pic.save(&buffer, png ? "PNG" : "JPG", png ? -1 : 85)) continue;
        img.base64 = data.toBase64();
    }
    if (r.images.size() > kMaxImages) {
        r.text += QString("\n[only the first %1 of %2 images are attached; ask for fewer frames or a contact sheet]")
                      .arg(kMaxImages)
                      .arg(r.images.size());
        r.images.resize(kMaxImages);
    }
    return r;
}

void AiChat::finishTurn()
{
    if (!m_busy) return;
    m_calls.clear();
    m_results.clear();
    m_streaming = -1;
    setBusy(false);
    emit turnFinished();
}

void AiChat::stop()
{
    if (!m_busy) return;
    m_stopped = true;
    m_client->abort();
    // What the model wrote so far stays in the history (text only: a cut answer has no complete tool calls)
    if (m_streaming >= 0) {
        const QString partial = m_entries[m_streaming].text;
        change(m_streaming, [](Entry& e) { e.state = Entry::State::Stopped; });
        if (!partial.trimmed().isEmpty()) {
            Ai::Message m;
            m.role = Ai::Message::Role::Assistant;
            m.text = partial;
            m_history << m;
        }
    }
    // Tools of the last answer: the running one ends with the tool server; every call gets an answer
    if (!m_calls.isEmpty()) {
        m_mcp->stop(); // answers the running call (toolDone, stopped)
        for (int i = int(m_results.size()); i < m_calls.size(); ++i)
            m_results << Ai::ToolResult{m_calls[i].id, m_calls[i].name, "not run: the user stopped the request", {}, true};
        appendUser({}, m_results);
    } else if (!m_mcp->isReady()) {
        m_mcp->stop(); // still starting
    }
    addNotice(T("Gestoppt."));
    finishTurn();
    emit stopped();
}

void AiChat::clear()
{
    stop();
    m_history.clear();
    m_entries.clear();
    m_notes.clear();
    m_system.clear();
    m_usage = {};
    m_request.clear();
    emit cleared();
    emit usageChanged();
}

QString AiChat::transcript() const
{
    QStringList out;
    for (const Entry& e : m_entries) {
        switch (e.kind) {
        case Entry::Kind::User: out << T("Du: %1").arg(e.text); break;
        case Entry::Kind::Assistant: out << T("KI: %1").arg(e.text); break;
        case Entry::Kind::Tool: out << "  › " + e.summary; break;
        case Entry::Kind::Notice:
        case Entry::Kind::Error: out << "  (" + e.text + ")"; break;
        }
    }
    return out.join("\n\n") + "\n";
}

QString AiChat::toolLine(const QString& tool, const QJsonObject& args)
{
    // Steps inside the app
    static const QHash<QString, const char*> steps{
        {"info", N_("Sieht sich das Projekt an")},     {"guide", N_("Liest die Anleitung")},
        {"help", N_("Liest die Hilfe")},                {"effects", N_("Sieht sich die Effekte an")},
        {"new", N_("Legt ein Projekt an")},             {"import", N_("Importiert")},
        {"backups", N_("Sieht sich die Sicherungen an")}, {"restore", N_("Stellt einen gesicherten Stand her")},
    };
    QString line;
    if (tool == "edit") line = T("Bearbeitet: %1").arg(opsOf(args));
    else if (tool == "app") line = T("Steuert die App: %1").arg(args["action"].toString());
    else if (steps.contains(tool)) line = T(steps.value(tool));
    else line = capitalized(AiLog::jobText(tool)); // analyses, render … (same words as the AI log)
    if (tool == "guide" && !args["topic"].toString().isEmpty()) return line + " · " + args["topic"].toString();
    if (tool == "edit" || tool == "app" || tool == "guide" || tool == "help" || tool == "effects") return line;
    const QString target = targetOf(args);
    return target.isEmpty() ? line : line + " · " + target;
}
