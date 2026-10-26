// The app's AI chat end to end: a MainWindow with the live server, the real `schneidi-cli mcp` as the chat's tools and
// a fake AI provider in this process (a local HTTP server answering with scripted, streamed answers in the Anthropic
// and the OpenAI protocol). Covered: the setup page (no provider yet) and starting from it; a request whose tool calls
// look at the project and split a clip in the app (one undo step, lit up, in the AI log) while AI connections from
// outside are switched off; what the provider gets (key header, guide in the system prompt, tools, the thinking block
// sent back unchanged, tool results, pictures of `frames` as a user message after the tool answers); review mode
// (accept = one undo step, discard = all back and the AI is told, editing meanwhile keeps the single steps); errors
// (bad key, overloaded then fine, no server), stopping a request, test runs that never send; the chat section in
// Settings › AI (test connection, model list, key stored).
// AI_CHAT_DUMP=<folder>: ai-setup.png, ai-conversation.png, ai-approval.png, ai-window.png, ai-settings.png.

#include "check.h"

#include "app/AiChat.h"
#include "app/AiChatPanel.h"
#include "app/AiClient.h"
#include "app/AiKeys.h"
#include "app/AiLog.h"
#include "app/AiSettingsPage.h"
#include "app/MainWindow.h"
#include "app/McpClient.h"
#include "app/SettingsDialog.h"
#include "app/Theme.h"
#include "cli/Live.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "engine/Engine.h"

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUndoStack>
#include <clocale>
#include <functional>

namespace {

const QByteArray kKey = "test-key-123";

bool waitFor(const std::function<bool()>& done, int ms = 60000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) QApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

void settle(int ms = 100)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) QApplication::processEvents(QEventLoop::AllEvents, 10);
}

// A provider on 127.0.0.1: records every request, answers with the next scripted response
class FakeProvider : public QObject {
public:
    struct Request {
        QByteArray method, path;
        QHash<QByteArray, QByteArray> headers; // lower case names
        QJsonObject body;
    };
    struct Response {
        int status = 200;
        QByteArray type = "text/event-stream";
        QByteArray body;
        int delayMs = 0;
        QByteArray location; // a redirect
    };
    QVector<Request> requests;
    QVector<Response> script;

    FakeProvider()
    {
        m_server.listen(QHostAddress::LocalHost, 0);
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* s = m_server.nextPendingConnection()) {
                connect(s, &QTcpSocket::readyRead, this, [this, s] { onData(s); });
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
    }
    QString address() const { return QString("http://127.0.0.1:%1").arg(m_server.serverPort()); }
    const Request& last() const { return requests.last(); }

private:
    void onData(QTcpSocket* s)
    {
        QByteArray& buf = m_buffers[s];
        buf += s->readAll();
        const qsizetype end = buf.indexOf("\r\n\r\n");
        if (end < 0) return;
        const QList<QByteArray> lines = buf.left(end).split('\n');
        Request r;
        const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
        r.method = first.value(0);
        r.path = first.value(1);
        for (int i = 1; i < lines.size(); ++i) {
            const qsizetype colon = lines[i].indexOf(':');
            if (colon > 0) r.headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
        }
        const int length = r.headers.value("content-length").toInt();
        if (buf.size() < end + 4 + length) return;
        r.body = QJsonDocument::fromJson(buf.mid(end + 4, length)).object();
        m_buffers.remove(s);
        requests << r;
        const Response answer = script.isEmpty() ? Response{500, "application/json", R"({"error":{"message":"no script"}})"}
                                                 : script.takeFirst();
        QPointer<QTcpSocket> guard(s);
        QTimer::singleShot(answer.delayMs, this, [guard, answer] {
            if (!guard || guard->state() != QAbstractSocket::ConnectedState) return;
            QByteArray head = "HTTP/1.1 " + QByteArray::number(answer.status) + " X\r\nContent-Type: " + answer.type
                              + "\r\nConnection: close\r\n";
            if (answer.status == 529 || answer.status == 429) head += "Retry-After: 0\r\n";
            if (!answer.location.isEmpty()) head += "Location: " + answer.location + "\r\n";
            if (!answer.type.contains("event-stream")) head += "Content-Length: " + QByteArray::number(answer.body.size()) + "\r\n";
            guard->write(head + "\r\n" + answer.body);
            guard->disconnectFromHost();
        });
    }

    QTcpServer m_server;
    QHash<QTcpSocket*, QByteArray> m_buffers;
};

QByteArray sse(const QVector<QJsonObject>& events)
{
    QByteArray out;
    for (const QJsonObject& e : events)
        out += "event: " + e["type"].toString().toUtf8() + "\ndata: " + QJsonDocument(e).toJson(QJsonDocument::Compact) + "\n\n";
    return out;
}

QJsonObject start(int input = 1000, int cached = 0)
{
    return {{"type", "message_start"},
            {"message", QJsonObject{{"id", "msg"}, {"usage", QJsonObject{{"input_tokens", input}, {"cache_read_input_tokens", cached}, {"output_tokens", 1}}}}}};
}

QJsonObject stop(const QString& reason, int output)
{
    return {{"type", "message_delta"}, {"delta", QJsonObject{{"stop_reason", reason}}}, {"usage", QJsonObject{{"output_tokens", output}}}};
}

// Anthropic: (a thinking block,) a tool call
FakeProvider::Response anthropicTool(const QString& id, const QString& name, const QJsonObject& input, bool thinking = false)
{
    QVector<QJsonObject> e{start()};
    int i = 0;
    if (thinking) {
        e << QJsonObject{{"type", "content_block_start"}, {"index", i}, {"content_block", QJsonObject{{"type", "thinking"}, {"thinking", ""}, {"signature", ""}}}}
          << QJsonObject{{"type", "content_block_delta"}, {"index", i}, {"delta", QJsonObject{{"type", "signature_delta"}, {"signature", "sig-abc"}}}}
          << QJsonObject{{"type", "content_block_stop"}, {"index", i}};
        ++i;
    }
    const QString json = QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Compact));
    e << QJsonObject{{"type", "content_block_start"}, {"index", i}, {"content_block", QJsonObject{{"type", "tool_use"}, {"id", id}, {"name", name}, {"input", QJsonObject{}}}}}
      << QJsonObject{{"type", "content_block_delta"}, {"index", i}, {"delta", QJsonObject{{"type", "input_json_delta"}, {"partial_json", json.left(5)}}}}
      << QJsonObject{{"type", "content_block_delta"}, {"index", i}, {"delta", QJsonObject{{"type", "input_json_delta"}, {"partial_json", json.mid(5)}}}}
      << QJsonObject{{"type", "content_block_stop"}, {"index", i}} << stop("tool_use", 30) << QJsonObject{{"type", "message_stop"}};
    return {200, "text/event-stream", sse(e)};
}

FakeProvider::Response anthropicText(const QString& text, int delayMs = 0)
{
    return {200, "text/event-stream",
            sse({start(),
                 {{"type", "content_block_start"}, {"index", 0}, {"content_block", QJsonObject{{"type", "text"}, {"text", ""}}}},
                 {{"type", "content_block_delta"}, {"index", 0}, {"delta", QJsonObject{{"type", "text_delta"}, {"text", text.left(4)}}}},
                 {{"type", "content_block_delta"}, {"index", 0}, {"delta", QJsonObject{{"type", "text_delta"}, {"text", text.mid(4)}}}},
                 {{"type", "content_block_stop"}, {"index", 0}},
                 stop("end_turn", 12),
                 {{"type", "message_stop"}}}),
            delayMs};
}

FakeProvider::Response openAiTool(const QString& id, const QString& name, const QJsonObject& args)
{
    const QString json = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
    const auto chunk = [](const QJsonObject& o) { return "data: " + QJsonDocument(o).toJson(QJsonDocument::Compact) + "\n\n"; };
    const auto call = [](const QJsonObject& c) {
        return QJsonObject{{"choices", QJsonArray{QJsonObject{{"index", 0}, {"delta", QJsonObject{{"tool_calls", QJsonArray{c}}}}}}}};
    };
    QByteArray body = chunk(call({{"index", 0}, {"id", id}, {"type", "function"}, {"function", QJsonObject{{"name", name}, {"arguments", json.left(6)}}}}))
                      + chunk(call({{"index", 0}, {"function", QJsonObject{{"arguments", json.mid(6)}}}}))
                      + chunk({{"choices", QJsonArray{QJsonObject{{"index", 0}, {"delta", QJsonObject{}}, {"finish_reason", "tool_calls"}}}}})
                      + "data: [DONE]\n\n";
    return {200, "text/event-stream", body};
}

FakeProvider::Response openAiText(const QString& text)
{
    const auto chunk = [](const QJsonObject& o) { return "data: " + QJsonDocument(o).toJson(QJsonDocument::Compact) + "\n\n"; };
    return {200, "text/event-stream",
            chunk({{"choices", QJsonArray{QJsonObject{{"index", 0}, {"delta", QJsonObject{{"content", text}}}}}}})
                + chunk({{"choices", QJsonArray{QJsonObject{{"index", 0}, {"delta", QJsonObject{}}, {"finish_reason", "stop"}}}}})
                + chunk({{"choices", QJsonArray{}}, {"usage", QJsonObject{{"prompt_tokens", 500}, {"completion_tokens", 7}}}})
                + "data: [DONE]\n\n"};
}

int clips(MainWindow& w) { return int(w.liveProject()->timeline().video[0].clips.size()); }
int undoIndex(MainWindow& w) { return w.liveProject()->undoStack()->index(); }

int count(const AiChat* chat, AiChat::Entry::Kind kind)
{
    int n = 0;
    for (const AiChat::Entry& e : chat->entries()) n += e.kind == kind;
    return n;
}

const AiChat::Entry* lastOf(const AiChat* chat, AiChat::Entry::Kind kind)
{
    for (auto it = chat->entries().crbegin(); it != chat->entries().crend(); ++it)
        if (it->kind == kind) return &*it;
    return nullptr;
}

// The text of the last user message of a request (Anthropic: its text blocks)
QString lastUserText(const QJsonObject& body)
{
    const QJsonArray m = body["messages"].toArray();
    for (auto it = m.end(); it != m.begin();) {
        --it;
        const QJsonObject msg = it->toObject();
        if (msg["role"].toString() != "user") continue;
        if (msg["content"].isString()) return msg["content"].toString();
        QString t;
        for (const QJsonValue& b : msg["content"].toArray())
            if (b["type"].toString() == "text") t += b["text"].toString();
        return t;
    }
    return {};
}

// No two messages of the same role after each other (Anthropic needs them alternating)
bool alternating(const QJsonObject& body)
{
    QString before;
    for (const QJsonValue& v : body["messages"].toArray()) {
        if (v["role"].toString() == before) return false;
        before = v["role"].toString();
    }
    return true;
}

// The next request only adds to the one before (Claude's thinking blocks are valid only while everything before them
// stays byte for byte: an edited history is a 400 for newer accounts)
bool appendOnly(const QJsonObject& before, const QJsonObject& after)
{
    if (before["system"] != after["system"] || before["tools"] != after["tools"]) return false;
    const QJsonArray a = before["messages"].toArray(), b = after["messages"].toArray();
    if (b.size() < a.size()) return false;
    for (int i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) return false;
    return true;
}

// The network side alone: a redirect to another address gets neither the key nor the conversation
void testClient()
{
    FakeProvider fake, other;
    Ai::Endpoint e;
    e.protocol = Ai::Protocol::Anthropic;
    e.provider = "claude";
    e.address = fake.address();
    e.model = "claude-sonnet-5-5";
    e.key = kKey;
    Ai::Request r;
    r.model = e.model;
    Ai::Message m;
    m.text = "hello";
    r.messages << m;
    AiClient client;
    QString failure;
    bool done = false;
    QObject::connect(&client, &AiClient::failed, [&](const QString& message) { failure = message; });
    QObject::connect(&client, &AiClient::finished, [&](const Ai::Reply&) { done = true; });
    fake.script = {{307, "application/json", "{}", 0, other.address().toUtf8() + "/v1/messages"}};
    client.send(e, r);
    CHECK(waitFor([&] { return !client.busy(); }));
    CHECK(!done);
    CHECK(failure.contains(other.address()));
    CHECK(other.requests.isEmpty());
    // The same address (another path): followed, with the key
    failure.clear();
    fake.script = {{307, "application/json", "{}", 0, fake.address().toUtf8() + "/v2/messages"}, anthropicText("Hallo")};
    client.send(e, r);
    CHECK(waitFor([&] { return !client.busy(); }));
    CHECK(done && failure.isEmpty());
    if (CHECK_EQ(fake.requests.size(), 3)) {
        CHECK_EQ(fake.requests[2].path, QByteArray("/v2/messages"));
        CHECK_EQ(fake.requests[2].headers.value("x-api-key"), kKey);
    }
}

void dump(QWidget* w, const QString& name)
{
    const QString dir = qEnvironmentVariable("AI_CHAT_DUMP");
    if (dir.isEmpty()) return;
    settle(150);
    w->grab().save(dir + "/" + name);
}

void test(const QString& dir)
{
    const QString media = Check::makeMedia(dir + "/a.mp4", {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=4",
                                                            "-f", "lavfi", "-i", "sine=frequency=440:duration=4", "-c:v",
                                                            "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-shortest"});
    if (!CHECK(!media.isEmpty())) return;
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return;
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    w.importFiles({media}, true); // V1/A1: one clip of 100 frames
    QApplication::processEvents();
    if (!CHECK_EQ(clips(w), 1)) return;

    FakeProvider fake;
    Cli::Live::Server server(&w);
    if (!CHECK(server.start(&err))) return;
    w.setAiChatTools({SCHNEIDI_CLI, "mcp"}, server.chatEnvironment());
    // AI connections from outside are off: the app's own chat still works
    server.disconnectClient("AI connections are turned off in the settings");
    AiChat* chat = w.aiChat();
    AiChatPanel* panel = w.aiChatPanel();
    if (!CHECK(chat && panel)) return;
    chat->client()->setRetryDelays({50, 50});
    w.showAiChat(true);

    // ---- Setup: no provider yet; the key comes from SCHNEIDI_AI_KEY, so "start" is enough ----
    CHECK(panel->setupShown());
    dump(panel, "ai-setup.png");
    auto* startButton = panel->findChild<QPushButton*>("ai_setup_start");
    if (!CHECK(startButton)) return;
    startButton->click();
    CHECK_EQ(w.aiSettings().chatProvider, QString("claude"));
    CHECK(!panel->setupShown());
    AiSettings s = w.aiSettings();
    s.chatAddress = fake.address();
    w.setAiSettings(s);

    // ---- A request: look, then split in the app (Anthropic, streamed) ----
    fake.script = {anthropicTool("toolu_1", "info", {{"project", "@app"}, {"summary", true}}, true),
                   anthropicTool("toolu_2", "edit", {{"project", "@app"}, {"ops", QJsonArray{QJsonObject{{"op", "split"}, {"at", "2s"}}}}}),
                   anthropicText("Fertig: bei 2 s geteilt.")};
    const int undo0 = undoIndex(w);
    panel->sendText("Teile den Clip bei 2 Sekunden");
    CHECK(panel->input()->toPlainText().isEmpty());
    CHECK(waitFor([&] { return !chat->busy(); }));
    if (!CHECK_EQ(fake.requests.size(), 3)) {
        for (const AiChat::Entry& e : chat->entries()) std::printf("       %d %s %s\n", int(e.kind), qPrintable(e.text), qPrintable(e.summary));
        return;
    }
    const FakeProvider::Request& r1 = fake.requests[0];
    CHECK_EQ(r1.path, QByteArray("/v1/messages"));
    CHECK_EQ(r1.headers.value("x-api-key"), kKey);
    CHECK(!r1.headers.value("anthropic-version").isEmpty());
    CHECK(!r1.headers.contains("authorization"));
    CHECK(!r1.body.contains("fallbacks")); // not Anthropic's own address
    CHECK_EQ(r1.body["model"].toString(), QString("claude-sonnet-5-5"));
    const QString system = r1.body["system"][0]["text"].toString();
    CHECK(system.contains("Editing video with schneidi")); // the guide
    CHECK(system.contains("@app"));
    CHECK(system.contains("German"));
    QStringList tools;
    for (const QJsonValue& t : r1.body["tools"].toArray()) tools << t["name"].toString();
    CHECK(tools.contains("edit") && tools.contains("info") && tools.contains("frames") && tools.contains("guide"));
    CHECK_EQ(lastUserText(r1.body), QString("Teile den Clip bei 2 Sekunden"));
    // The thinking block goes back unchanged, the tool answer with it
    const QJsonArray m2 = fake.requests[1].body["messages"].toArray();
    if (CHECK_EQ(m2.size(), 3)) {
        CHECK_EQ(m2[1]["content"][0]["type"].toString(), QString("thinking"));
        CHECK_EQ(m2[1]["content"][0]["signature"].toString(), QString("sig-abc"));
        CHECK_EQ(m2[1]["content"][1]["input"]["summary"].toBool(), true);
        CHECK_EQ(m2[2]["content"][0]["tool_use_id"].toString(), QString("toolu_1"));
        CHECK(m2[2]["content"][0]["content"][0]["text"].toString().contains("\"ok\":true"));
    }
    const QJsonArray m3 = fake.requests[2].body["messages"].toArray();
    if (CHECK_EQ(m3.size(), 5)) {
        CHECK_EQ(m3[4]["content"][0]["tool_use_id"].toString(), QString("toolu_2"));
        CHECK(!m3[4]["content"][0].toObject().contains("is_error"));
    }
    for (const FakeProvider::Request& r : fake.requests) CHECK(alternating(r.body));
    // In the app: split, one undo step, lit up and in the AI log
    CHECK_EQ(clips(w), 2);
    CHECK_EQ(undoIndex(w), undo0 + 1);
    CHECK(w.liveProject()->undoStack()->undoText().startsWith(T("KI: %1").arg("split")));
    bool logged = false;
    for (const AiLog::Entry& e : w.aiLog()->entries()) logged |= e.step == "split";
    CHECK(logged);
    // In the chat: the request, two steps, the answer; tokens counted
    CHECK_EQ(count(chat, AiChat::Entry::Kind::User), 1);
    CHECK_EQ(count(chat, AiChat::Entry::Kind::Tool), 2);
    if (const AiChat::Entry* a = lastOf(chat, AiChat::Entry::Kind::Assistant); CHECK(a))
        CHECK_EQ(a->text, QString("Fertig: bei 2 s geteilt."));
    if (const AiChat::Entry* t = lastOf(chat, AiChat::Entry::Kind::Tool); CHECK(t)) {
        CHECK(t->state == AiChat::Entry::State::Done);
        CHECK_EQ(t->summary, T("Bearbeitet: %1").arg("split"));
    }
    CHECK_EQ(chat->usage().input, qint64(3000));
    CHECK_EQ(chat->history().size(), 6);
    CHECK(!count(chat, AiChat::Entry::Kind::Error));

    // ---- Review mode: accept = one undo step ----
    s.approval = AiSettings::Approval::Review;
    w.setAiSettings(s);
    fake.script = {anthropicTool("toolu_3", "edit", {{"project", "@app"}, {"ops", QJsonArray{QJsonObject{{"op", "split"}, {"at", "1s"}}}}}),
                   anthropicTool("toolu_4", "edit", {{"project", "@app"}, {"ops", QJsonArray{QJsonObject{{"op", "split"}, {"at", "3s"}}}}}),
                   anthropicText("Zweimal geteilt.")};
    const int undo1 = undoIndex(w);
    panel->sendText("Teile bei 1 und 3 Sekunden");
    CHECK(waitFor([&] { return !chat->busy(); }));
    CHECK_EQ(clips(w), 4); // live in the timeline already
    CHECK_EQ(undoIndex(w), undo1 + 2);
    CHECK(panel->approvalShown());
    CHECK(!panel->input()->isEnabled()); // the AI waits
    // The system prompt stays the chat's (cached by the provider); the change of mode comes as a note
    CHECK_EQ(fake.requests.last().body["system"][0]["text"].toString(), system);
    CHECK(lastUserText(fake.requests[3].body).contains("reviews your changes"));
    dump(panel, "ai-approval.png");
    if (auto* accept = panel->findChild<QPushButton*>("ai_accept"); CHECK(accept)) accept->click();
    CHECK(!panel->approvalShown());
    CHECK(panel->input()->isEnabled());
    CHECK_EQ(undoIndex(w), undo1 + 1);
    CHECK(w.liveProject()->undoStack()->undoText().startsWith(T("KI-Chat: %1").arg("Teile bei 1")));
    CHECK_EQ(clips(w), 4);
    w.liveProject()->undoStack()->undo();
    CHECK_EQ(clips(w), 2);
    w.liveProject()->undoStack()->redo();
    CHECK_EQ(clips(w), 4);

    // ---- Discard: all back, and the AI hears it with the next request ----
    fake.script = {anthropicTool("toolu_5", "edit", {{"project", "@app"}, {"ops", QJsonArray{QJsonObject{{"op", "split"}, {"at", "0.5s"}}}}}),
                   anthropicText("Geteilt.")};
    const int undo2 = undoIndex(w);
    panel->sendText("Teile bei 0,5 s");
    CHECK(waitFor([&] { return !chat->busy(); }));
    CHECK_EQ(clips(w), 5);
    CHECK(panel->approvalShown());
    if (auto* discard = panel->findChild<QPushButton*>("ai_discard"); CHECK(discard)) discard->click();
    CHECK_EQ(clips(w), 4);
    CHECK_EQ(undoIndex(w), undo2);
    CHECK(!panel->approvalShown());
    fake.script = {anthropicText("Alles klar.")};
    panel->sendText("Danke");
    CHECK(waitFor([&] { return !chat->busy(); }));
    CHECK(lastUserText(fake.last().body).contains("discarded"));
    CHECK(lastUserText(fake.last().body).contains("Danke"));

    // ---- Editing meanwhile: the bar goes, the AI's changes stay single steps ----
    fake.script = {anthropicTool("toolu_6", "edit", {{"project", "@app"}, {"ops", QJsonArray{QJsonObject{{"op", "split"}, {"at", "1.5s"}}}}}),
                   anthropicText("Geteilt.")};
    const int undo3 = undoIndex(w);
    panel->sendText("Teile bei 1,5 s");
    CHECK(waitFor([&] { return !chat->busy(); }));
    CHECK(panel->approvalShown());
    w.liveProject()->edit("user", [](Timeline& tl) { tl.markIn = 7; });
    CHECK(!panel->approvalShown());
    CHECK_EQ(undoIndex(w), undo3 + 2);
    CHECK_EQ(clips(w), 5);
    s.approval = AiSettings::Approval::Auto;
    w.setAiSettings(s);
    // The whole conversation so far (tool rounds, accepted, discarded, notes of the app): only ever appended to
    for (qsizetype i = 1; i < fake.requests.size(); ++i)
        if (!CHECK(appendOnly(fake.requests[i - 1].body, fake.requests[i].body))) std::printf("       request %lld\n", i);

    // ---- OpenAI-compatible server: pictures of `frames` follow the tool answers as a user message ----
    chat->clear();
    CHECK(chat->entries().isEmpty() && chat->history().isEmpty());
    s.chatProvider = "custom";
    s.chatAddress = fake.address() + "/v1";
    s.chatModels.insert("custom", "test-model");
    w.setAiSettings(s);
    CHECK(!panel->setupShown());
    fake.script = {openAiTool("call_1", "frames", {{"source", "@app"}, {"count", 2}}), openAiText("Zwei Bilder: Testbild mit Farbbalken.")};
    const int before = int(fake.requests.size());
    panel->sendText("Wie sieht das Video aus?");
    CHECK(waitFor([&] { return !chat->busy(); }));
    if (CHECK_EQ(fake.requests.size(), before + 2)) {
        const FakeProvider::Request& o1 = fake.requests[before];
        CHECK_EQ(o1.path, QByteArray("/v1/chat/completions"));
        CHECK_EQ(o1.headers.value("authorization"), "Bearer " + kKey);
        CHECK(!o1.headers.contains("x-api-key"));
        CHECK_EQ(o1.body["model"].toString(), QString("test-model"));
        CHECK_EQ(o1.body["messages"][0]["role"].toString(), QString("system"));
        CHECK(!o1.body.contains("stream_options")); // an unknown server
        const QJsonArray m = fake.requests[before + 1].body["messages"].toArray();
        QStringList roles;
        for (const QJsonValue& v : m) roles << v["role"].toString();
        CHECK_EQ(roles, QStringList({"system", "user", "assistant", "tool", "user"}));
        if (m.size() == 5) {
            CHECK_EQ(m[2]["tool_calls"][0]["id"].toString(), QString("call_1"));
            CHECK_EQ(m[3]["tool_call_id"].toString(), QString("call_1"));
            const QJsonArray pics = m[4]["content"].toArray();
            CHECK_EQ(pics.size(), 3); // a caption, two pictures
            CHECK(pics.last()["image_url"]["url"].toString().startsWith("data:image/jpeg;base64,"));
        }
    }
    if (const AiChat::Entry* t = lastOf(chat, AiChat::Entry::Kind::Tool); CHECK(t)) {
        CHECK_EQ(t->images.size(), 2);
        CHECK(t->state == AiChat::Entry::State::Done);
    }
    CHECK_EQ(chat->usage().input, qint64(500));

    // A click on a picture shows it bigger in a window of its own; never a popup (on X11 a popup grabs mouse and
    // keyboard, and one opened from a press kept the grab: the whole desktop stopped answering). Click/Escape close it.
    QLabel* thumb = nullptr;
    for (QLabel* l : panel->findChildren<QLabel*>())
        if (l->toolTip() == T("Klick: größer zeigen")) thumb = l;
    if (CHECK(thumb)) {
        const auto pictureWindow = [] {
            for (QWidget* top : QApplication::topLevelWidgets())
                if (top->isVisible() && top->inherits("QLabel") && top->windowTitle() == T("KI-Chat")) return top;
            return static_cast<QWidget*>(nullptr);
        };
        const auto click = [](QWidget* target, QEvent::Type type) {
            const QPointF at(target->width() / 2.0, target->height() / 2.0);
            QMouseEvent e(type, at, target->mapToGlobal(at), Qt::LeftButton,
                          type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(target, &e);
        };
        click(thumb, QEvent::MouseButtonPress);
        CHECK(!pictureWindow()); // opens on release, not while the button is down
        click(thumb, QEvent::MouseButtonRelease);
        QPointer<QWidget> big = pictureWindow();
        if (CHECK(big)) {
            CHECK(!QApplication::activePopupWidget());
            CHECK(big->windowType() == Qt::Dialog);
            QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(big, &esc);
            CHECK(waitFor([&] { return big.isNull(); }));
        }
        click(thumb, QEvent::MouseButtonRelease);
        big = pictureWindow();
        if (CHECK(big)) {
            click(big, QEvent::MouseButtonRelease); // a click on the picture closes it too
            CHECK(waitFor([&] { return big.isNull(); }));
        }
    }
    dump(panel, "ai-conversation.png");
    dump(&w, "ai-window.png");

    // ---- Errors ----
    s.chatProvider = "claude";
    s.chatAddress = fake.address();
    w.setAiSettings(s);
    fake.script = {{401, "application/json", R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})"}};
    panel->sendText("Hallo");
    CHECK(waitFor([&] { return !chat->busy(); }));
    if (const AiChat::Entry* e = lastOf(chat, AiChat::Entry::Kind::Error); CHECK(e)) {
        CHECK(e->text.contains("invalid x-api-key"));
        CHECK(e->text.contains(T("Der Schlüssel wurde abgelehnt. Prüfe ihn in Einstellungen › KI.")));
        CHECK(!e->text.contains(kKey)); // never the key itself
    }
    // The pictures of the request before are not sent again (a note says so)
    CHECK(!QJsonDocument(fake.last().body).toJson().contains("base64"));
    CHECK(QJsonDocument(fake.last().body).toJson().contains("not sent again"));
    // Overloaded, then fine: tried again by itself
    fake.script = {{529, "application/json", R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})"},
                   anthropicText("Hallo!")};
    panel->sendText("Hallo?");
    CHECK(waitFor([&] { return !chat->busy(); }));
    if (const AiChat::Entry* n = lastOf(chat, AiChat::Entry::Kind::Notice); CHECK(n))
        CHECK(n->text.contains(T("Der Anbieter ist gerade überlastet.")));
    if (const AiChat::Entry* a = lastOf(chat, AiChat::Entry::Kind::Assistant); CHECK(a)) CHECK_EQ(a->text, QString("Hallo!"));
    for (const FakeProvider::Request& r : fake.requests.mid(fake.requests.size() - 2)) CHECK(alternating(r.body));
    // Stop while it waits: the request stays answerable, the next one goes out fine
    fake.script = {anthropicText("zu spät", 5000), anthropicText("Jetzt.")};
    panel->sendText("Langsam");
    CHECK(waitFor([&] { return fake.script.size() == 1; }, 10000));
    settle(120);
    auto* working = panel->findChild<QLabel*>("ai_working"); // nothing streams yet: "the AI is working"
    if (CHECK(working)) CHECK(working->isVisible());
    chat->stop();
    CHECK(!chat->busy());
    if (working) CHECK(!working->isVisible());
    if (const AiChat::Entry* n = lastOf(chat, AiChat::Entry::Kind::Notice); CHECK(n)) CHECK_EQ(n->text, T("Gestoppt."));
    panel->sendText("Und jetzt?");
    CHECK(waitFor([&] { return !chat->busy(); }));
    CHECK(alternating(fake.last().body));
    CHECK(lastUserText(fake.last().body).contains("Langsam")); // the stopped request is still part of the chat
    if (const AiChat::Entry* a = lastOf(chat, AiChat::Entry::Kind::Assistant); CHECK(a)) CHECK_EQ(a->text, QString("Jetzt."));
    // Stop during a tool call: every call is answered, the chat goes on
    fake.script = {anthropicTool("toolu_7", "silence", {{"file", media}}), anthropicText("Weiter.")};
    panel->sendText("Such Pausen");
    CHECK(waitFor([&] { return lastOf(chat, AiChat::Entry::Kind::Tool) && lastOf(chat, AiChat::Entry::Kind::Tool)->tool == "silence"; }, 20000));
    chat->stop();
    if (const AiChat::Entry* t = lastOf(chat, AiChat::Entry::Kind::Tool); CHECK(t)) CHECK(t->state != AiChat::Entry::State::Running);
    panel->sendText("Weiter bitte");
    CHECK(waitFor([&] { return !chat->busy(); }));
    {
        const QJsonArray m = fake.last().body["messages"].toArray();
        CHECK(alternating(fake.last().body));
        bool answered = false;
        for (const QJsonValue& v : m)
            for (const QJsonValue& b : v["content"].toArray())
                answered |= b["tool_use_id"].toString() == "toolu_7";
        CHECK(answered);
    }
    // A refused answer with a tool call: not run, not kept (unanswered it would break every later request)
    {
        FakeProvider::Response refused = anthropicTool("toolu_r", "info", {{"project", "@app"}});
        refused.body.replace("\"stop_reason\":\"tool_use\"", "\"stop_reason\":\"refusal\"");
        fake.script = {refused, anthropicText("Gern.")};
        panel->sendText("Etwas Heikles");
        CHECK(waitFor([&] { return !chat->busy(); }));
        if (const AiChat::Entry* e = lastOf(chat, AiChat::Entry::Kind::Error); CHECK(e))
            CHECK_EQ(e->text, T("Das Modell hat die Anfrage abgelehnt."));
        panel->sendText("Dann etwas anderes");
        CHECK(waitFor([&] { return !chat->busy(); }));
        CHECK(alternating(fake.last().body));
        CHECK(!QJsonDocument(fake.last().body).toJson().contains("toolu_r"));
        CHECK(lastUserText(fake.last().body).contains("Dann etwas anderes"));
    }
    // No server there
    s.chatAddress = "http://127.0.0.1:1";
    w.setAiSettings(s);
    panel->sendText("Hallo");
    CHECK(waitFor([&] { return !chat->busy(); }, 30000));
    if (const AiChat::Entry* e = lastOf(chat, AiChat::Entry::Kind::Error); CHECK(e))
        CHECK(e->text.contains("127.0.0.1"));
    // Test runs never send
    const int sent = int(fake.requests.size());
    w.setAiChatOffline(true);
    panel->sendText("Hallo");
    CHECK(!chat->busy());
    CHECK_EQ(int(fake.requests.size()), sent);
    if (const AiChat::Entry* e = lastOf(chat, AiChat::Entry::Kind::Error); CHECK(e)) CHECK_EQ(e->text, T("Testlauf: der KI-Chat sendet nichts."));
    CHECK_EQ(panel->input()->toPlainText(), QString("Hallo")); // stays in the input
    w.setAiChatOffline(false);
    CHECK(!chat->transcript().contains(kKey));

    // ---- Settings › AI, chat section: test connection, model list, key ----
    s.chatAddress = fake.address();
    Ai::Keys keys(nullptr, Ai::settingsKeyStore());
    keys.remove("claude", nullptr);
    SettingsDialog dlg(s);
    dlg.aiPage()->addChatSection(&keys);
    auto* section = dlg.aiPage()->findChild<QWidget*>("ai_chat_section");
    auto* provider = dlg.aiPage()->findChild<QComboBox*>("ai_chat_provider");
    auto* address = dlg.aiPage()->findChild<QLineEdit*>("ai_chat_address");
    auto* model = dlg.aiPage()->findChild<QComboBox*>("ai_chat_model");
    auto* keyField = dlg.aiPage()->findChild<QLineEdit*>("ai_chat_key");
    auto* keyState = dlg.aiPage()->findChild<QLabel*>("ai_chat_key_state");
    auto* result = dlg.aiPage()->findChild<QLabel*>("ai_chat_result");
    if (!CHECK(section && provider && address && model && keyField && keyState && result)) return;
    CHECK_EQ(provider->currentData().toString(), QString("claude"));
    CHECK_EQ(address->text(), fake.address());
    CHECK_EQ(model->currentText(), QString("claude-sonnet-5-5"));
    CHECK(keyState->text().contains("SCHNEIDI_AI_KEY"));
    fake.script = {{200, "application/json", R"({"content":[{"type":"text","text":"OK"}],"stop_reason":"end_turn"})"}};
    dlg.aiPage()->findChild<QPushButton*>("ai_chat_test")->click();
    CHECK(waitFor([&] { return result->text().contains("claude-sonnet-5-5"); }, 10000));
    CHECK_EQ(result->text(), T("Verbindung steht: %1 antwortet.").arg("claude-sonnet-5-5"));
    CHECK_EQ(fake.last().body["max_tokens"].toInt(), 64);
    CHECK(!fake.last().body["stream"].toBool());
    fake.script = {{200, "application/json", R"({"data":[{"id":"claude-sonnet-5-5"},{"id":"claude-opus-5-5"}]})"}};
    dlg.aiPage()->findChild<QPushButton*>("ai_chat_models")->click();
    CHECK(waitFor([&] { return model->count() == 2; }, 10000));
    CHECK(fake.last().path.startsWith("/v1/models"));
    CHECK_EQ(fake.last().method, QByteArray("GET"));
    model->setCurrentText("claude-opus-5-5");
    // Switching the provider and back keeps what was typed; the approval mode
    provider->setCurrentIndex(provider->findData("ollama"));
    CHECK_EQ(model->currentText(), QString("qwen3"));
    CHECK(address->text().isEmpty());
    CHECK_EQ(address->placeholderText(), QString("http://localhost:11434/v1"));
    provider->setCurrentIndex(provider->findData("claude"));
    CHECK_EQ(model->currentText(), QString("claude-opus-5-5"));
    address->setText(fake.address());
    auto* approval = dlg.aiPage()->findChild<QComboBox*>("ai_chat_approval");
    if (CHECK(approval)) approval->setCurrentIndex(approval->findData(int(AiSettings::Approval::Review)));
    const AiSettings chosen = dlg.aiSettings();
    CHECK_EQ(chosen.chatProvider, QString("claude"));
    CHECK_EQ(chosen.chatModel(), QString("claude-opus-5-5"));
    CHECK(chosen.approval == AiSettings::Approval::Review);
    CHECK_EQ(chosen.chatAddress, fake.address());
    // A typed key goes to the store on OK (here the settings: tests have no keychain)
    keyField->setText("sk-typed");
    QString keyError;
    CHECK(dlg.aiPage()->applyChatKey(&keyError));
    CHECK(keyField->text().isEmpty());
    CHECK(keys.hasStored("claude"));
    CHECK_EQ(Ai::settingsKeyStore()->read("claude", nullptr), QString("sk-typed"));
    keys.remove("claude", nullptr);
    dlg.resize(600, 760);
    dlg.showTab(SettingsDialog::Tab::Ai);
    dlg.show();
    dump(&dlg, "ai-settings.png");
    dlg.hide();
}

} // namespace

int main(int argc, char** argv)
{
    QTemporaryDir tmp;
    // The chat's schneidi-cli: backups and its registry in here; never a real key from the environment
    qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
    for (const char* var : {"ANTHROPIC_API_KEY", "OPENAI_API_KEY", "OPENROUTER_API_KEY"}) qunsetenv(var);
    qputenv("SCHNEIDI_AI_KEY", kKey);
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("ai_chat");
    QSettings().remove("ai");
    Theme::apply(app);
    I18n::setLanguage("de"); // the chat tells the AI the interface language; not the system's (C in a container)
    I18n::install("de");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg fehlt");
    testClient();
    test(tmp.path());
    QSettings().remove("ai");
    return Check::result();
}
