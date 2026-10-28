// AI chat providers without network (app/AiProvider.h, app/AiKeys.h): requests of both protocols (Anthropic Messages
// API, OpenAI-compatible chat completions) from one conversation – tools, tool results with pictures (OpenAI: as a user
// message after the tool answers), the Anthropic content sent back unchanged (thinking blocks), caching, the headers
// with the key; streamed answers split anywhere (text, tool calls, usage, errors in the stream, servers without blank
// lines, Ollama's whole calls); error answers to messages (401, 429 with Retry-After, overloaded, 404, a request larger
// than the model's context …); model lists; where a key comes from (environment > company file > keychain > settings,
// never another provider's variable), storing it (keychain first, no second plain copy), the company file (merged,
// locked, broken) and the provider choice; what goes back to the model from long tool answers; the chat's one-line tool
// steps; tool calls and final answers a model wrote as JSON; the short request for small contexts (when, the tools in
// short) and the context servers name (llama.cpp, Ollama, LM Studio, vLLM).
// AI_KEYCHAIN_TEST=1 also writes, reads and deletes a key in the Secret Service of the session bus – only on a
// private bus: dbus-run-session -- sh -c 'echo -n x | gnome-keyring-daemon --unlock; build-tests/tests/test_ai_providers'

#include "check.h"

#include "app/AiChat.h"
#include "app/AiContext.h"
#include "app/AiKeys.h"
#include "app/AiProvider.h"
#include "core/I18n.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSettings>
#include <QTemporaryDir>

#include <map>

namespace {

QString header(const QList<QPair<QByteArray, QByteArray>>& h, const QByteArray& name)
{
    for (const auto& [k, v] : h)
        if (k == name) return QString::fromUtf8(v);
    return {};
}

// A conversation with everything: a request, an assistant turn with a tool call (and Anthropic's own content with a
// thinking block), the tool's answer with a picture and an error, a follow-up text
QVector<Ai::Message> conversation()
{
    Ai::Message user;
    user.text = "Cut the pauses";
    Ai::Message assistant;
    assistant.role = Ai::Message::Role::Assistant;
    assistant.text = "Let me look.";
    assistant.calls << Ai::ToolCall{"toolu_1", "frames", QJsonObject{{"source", "@app"}, {"count", 2}}, {}}
                    << Ai::ToolCall{"toolu_2", "silence", QJsonObject{{"file", "@app"}}, {}};
    assistant.anthropic = QJsonArray{
        QJsonObject{{"type", "thinking"}, {"thinking", ""}, {"signature", "sig-123"}},
        QJsonObject{{"type", "text"}, {"text", "Let me look."}},
        QJsonObject{{"type", "tool_use"}, {"id", "toolu_1"}, {"name", "frames"}, {"input", QJsonObject{{"source", "@app"}, {"count", 2}}}},
        QJsonObject{{"type", "tool_use"}, {"id", "toolu_2"}, {"name", "silence"}, {"input", QJsonObject{{"file", "@app"}}}}};
    Ai::Message results;
    results.results << Ai::ToolResult{"toolu_1", "frames", "{\"ok\":true}", {Ai::Image{"QUJD", "image/jpeg"}}, false}
                    << Ai::ToolResult{"toolu_2", "silence", "{\"ok\":false}", {}, true};
    results.text = "and keep it short";
    return {user, assistant, results};
}

Ai::Request request(const QString& provider)
{
    Ai::Request r;
    r.model = "m1";
    r.provider = provider;
    r.system = "You edit videos.";
    r.tools = QJsonArray{QJsonObject{{"name", "info"}, {"description", "Show a project"},
                                     {"inputSchema", QJsonObject{{"type", "object"}}}},
                         QJsonObject{{"name", "edit"}, {"description", "Edit"}, {"inputSchema", QJsonObject{{"type", "object"}}}}};
    r.messages = conversation();
    return r;
}

void testPresets()
{
    CHECK(Ai::preset("claude") && Ai::preset("claude")->protocol == Ai::Protocol::Anthropic);
    CHECK_EQ(Ai::preset("claude")->model, QString("claude-sonnet-5-5"));
    CHECK(Ai::preset("ollama") && !Ai::preset("ollama")->needsKey);
    CHECK_EQ(Ai::preset("ollama")->address, QString("http://localhost:11434/v1"));
    CHECK(!Ai::preset("nope"));
    CHECK(Ai::isLocal("http://localhost:11434/v1"));
    CHECK(Ai::isLocal("http://127.0.0.1:8080"));
    CHECK(Ai::isLocal("http://[::1]:8080/v1"));
    CHECK(!Ai::isLocal("https://api.anthropic.com"));
    CHECK(!Ai::isLocal("http://192.168.1.20:8000/v1")); // another machine: the privacy notice asks
    // Reached directly, never through the system's proxy: this computer and the local network
    for (const char* h : {"localhost", "ollama.localhost", "127.0.0.1", "::1", "[::1]", "192.168.1.20", "10.1.2.3",
                          "172.20.0.5", "169.254.3.4", "fe80::1", "fd12::5", "gpu-box.local"})
        if (!CHECK(Ai::isNearHost(h))) std::printf("       %s\n", h);
    for (const char* h : {"api.anthropic.com", "8.8.8.8", "172.32.0.1", "2001:db8::1", "openrouter.ai", ""})
        if (!CHECK(!Ai::isNearHost(h))) std::printf("       %s\n", h);

    Ai::Endpoint a{Ai::Protocol::Anthropic, "https://api.anthropic.com/", "k", "claude-sonnet-5-5", "claude"};
    CHECK_EQ(Ai::chatUrl(a).toString(), QString("https://api.anthropic.com/v1/messages"));
    CHECK(Ai::modelsUrl(a).toString().startsWith("https://api.anthropic.com/v1/models"));
    a.address = "https://proxy.example.com/anthropic/v1";
    CHECK_EQ(Ai::chatUrl(a).toString(), QString("https://proxy.example.com/anthropic/v1/messages"));
    Ai::Endpoint o{Ai::Protocol::OpenAi, "http://localhost:11434/v1", {}, "qwen3", "ollama"};
    CHECK_EQ(Ai::chatUrl(o).toString(), QString("http://localhost:11434/v1/chat/completions"));
    CHECK_EQ(Ai::modelsUrl(o).toString(), QString("http://localhost:11434/v1/models"));

    // Headers: the key only where it belongs, nothing without one
    auto h = Ai::headers({Ai::Protocol::Anthropic, "https://api.anthropic.com", "sk-ant-x", "claude-sonnet-5-5", "claude"});
    CHECK_EQ(header(h, "x-api-key"), QString("sk-ant-x"));
    CHECK(!header(h, "anthropic-version").isEmpty());
    CHECK(header(h, "authorization").isEmpty());
    CHECK(header(h, "anthropic-beta").isEmpty());
    CHECK(!header(Ai::headers({Ai::Protocol::Anthropic, "https://api.anthropic.com", "k", "m", "claude"}, true),
                  "anthropic-beta").isEmpty());
    h = Ai::headers({Ai::Protocol::OpenAi, "https://api.openai.com/v1", "sk-x", "gpt-5", "openai"});
    CHECK_EQ(header(h, "authorization"), QString("Bearer sk-x"));
    CHECK(header(h, "x-api-key").isEmpty());
    h = Ai::headers(o);
    CHECK(header(h, "authorization").isEmpty());
    // Server-side fallback only on Anthropic's own API with the models that have it
    CHECK(Ai::fallbacksFor({Ai::Protocol::Anthropic, "https://api.anthropic.com", "k", "claude-sonnet-5-5", "claude"}));
    CHECK(!Ai::fallbacksFor({Ai::Protocol::Anthropic, "https://proxy.example.com", "k", "claude-sonnet-5-5", "claude"}));
    CHECK(!Ai::fallbacksFor({Ai::Protocol::Anthropic, "https://api.anthropic.com", "k", "claude-haiku-4-5", "claude"}));
}

void testAnthropicBody()
{
    Ai::Request r = request("claude");
    r.fallbacks = true;
    const QJsonObject b = Ai::anthropicBody(r);
    CHECK_EQ(b["model"].toString(), QString("m1"));
    CHECK(b["stream"].toBool());
    CHECK(b["max_tokens"].toInt() > 1000);
    CHECK(!b.contains("temperature"));
    CHECK_EQ(b["fallbacks"].toString(), QString("default"));
    // Caching: one mark at the end of the system prompt (covers the tools before it), automatic for the conversation
    const QJsonArray system = b["system"].toArray();
    CHECK_EQ(system.size(), 1);
    CHECK_EQ(system[0]["text"].toString(), QString("You edit videos."));
    CHECK_EQ(system[0]["cache_control"]["type"].toString(), QString("ephemeral"));
    CHECK_EQ(b["cache_control"]["type"].toString(), QString("ephemeral"));
    const QJsonArray tools = b["tools"].toArray();
    CHECK_EQ(tools.size(), 2);
    CHECK_EQ(tools[1]["name"].toString(), QString("edit"));
    CHECK(tools[1].toObject().contains("input_schema"));
    CHECK(!tools[1].toObject().contains("inputSchema"));
    const QJsonArray m = b["messages"].toArray();
    if (!CHECK_EQ(m.size(), 3)) return;
    CHECK_EQ(m[0]["role"].toString(), QString("user"));
    CHECK_EQ(m[0]["content"][0]["text"].toString(), QString("Cut the pauses"));
    // The assistant turn exactly as it came (thinking block with its signature)
    CHECK_EQ(m[1]["content"].toArray(), conversation()[1].anthropic);
    // Tool results first, the picture inside the result, the error marked; then the text
    const QJsonArray c = m[2]["content"].toArray();
    if (!CHECK_EQ(c.size(), 3)) return;
    CHECK_EQ(c[0]["type"].toString(), QString("tool_result"));
    CHECK_EQ(c[0]["tool_use_id"].toString(), QString("toolu_1"));
    CHECK_EQ(c[0]["content"][1]["type"].toString(), QString("image"));
    CHECK_EQ(c[0]["content"][1]["source"]["data"].toString(), QString("QUJD"));
    CHECK_EQ(c[0]["content"][1]["source"]["media_type"].toString(), QString("image/jpeg"));
    CHECK(!c[0].toObject().contains("is_error"));
    CHECK(c[1]["is_error"].toBool());
    CHECK_EQ(c[2]["text"].toString(), QString("and keep it short"));
    // An assistant turn from another provider earlier in the chat: built from its text and calls
    QVector<Ai::Message> msgs = conversation();
    msgs[1].anthropic = {};
    r.messages = msgs;
    const QJsonObject rebuilt = Ai::anthropicBody(r);
    const QJsonArray built = rebuilt["messages"][1]["content"].toArray();
    CHECK_EQ(built.size(), 3);
    CHECK_EQ(built[1]["type"].toString(), QString("tool_use"));
    CHECK_EQ(built[1]["input"]["count"].toInt(), 2);
    // Without the optional parts (a proxy that refused them)
    r.fallbacks = false;
    r.autoCache = false;
    const QJsonObject plain = Ai::anthropicBody(r);
    CHECK(!plain.contains("fallbacks") && !plain.contains("cache_control"));
    CHECK(!plain["system"][0].toObject().contains("cache_control"));
}

void testOpenAiBody()
{
    const QJsonObject b = Ai::openAiBody(request("openai"));
    CHECK(b["stream"].toBool());
    CHECK(b["stream_options"]["include_usage"].toBool());
    CHECK(!b.contains("max_tokens") && !b.contains("temperature"));
    CHECK_EQ(b["tools"][0]["type"].toString(), QString("function"));
    CHECK_EQ(b["tools"][0]["function"]["name"].toString(), QString("info"));
    CHECK(b["tools"][0]["function"].toObject().contains("parameters"));
    const QJsonArray m = b["messages"].toArray();
    QStringList roles;
    for (const QJsonValue& v : m) roles << v["role"].toString();
    // system, user, assistant (tool calls), two tool answers, the pictures as a user message, the user's text
    CHECK_EQ(roles, QStringList({"system", "user", "assistant", "tool", "tool", "user", "user"}));
    if (m.size() < 7) return;
    CHECK_EQ(m[2]["content"].toString(), QString("Let me look."));
    CHECK_EQ(m[2]["tool_calls"][0]["id"].toString(), QString("toolu_1"));
    CHECK_EQ(QJsonDocument::fromJson(m[2]["tool_calls"][0]["function"]["arguments"].toString().toUtf8()).object()["count"].toInt(), 2);
    CHECK_EQ(m[3]["tool_call_id"].toString(), QString("toolu_1"));
    CHECK(m[3]["content"].toString().contains("image"));
    CHECK(m[4]["content"].toString().startsWith("ERROR"));
    const QJsonArray pics = m[5]["content"].toArray();
    CHECK_EQ(pics.size(), 2);
    CHECK_EQ(pics[1]["image_url"]["url"].toString(), QString("data:image/jpeg;base64,QUJD"));
    CHECK_EQ(m[6]["content"].toString(), QString("and keep it short"));
    // Unknown servers: no stream_options (strict servers refuse unknown fields)
    CHECK(!Ai::openAiBody(request("custom")).contains("stream_options"));
    // An assistant turn with only tool calls: content null
    QVector<Ai::Message> msgs = conversation();
    msgs[1].text.clear();
    Ai::Request r = request("openai");
    r.messages = msgs;
    const QJsonObject onlyCalls = Ai::openAiBody(r);
    CHECK(onlyCalls["messages"][2]["content"].isNull());
}

// Feeds `data` in pieces of `step` bytes
Ai::Reply stream(Ai::Protocol p, const QByteArray& data, int step, QString* streamed = nullptr, bool* finished = nullptr)
{
    Ai::StreamParser parser(p);
    QString text;
    for (int i = 0; i < data.size(); i += step) text += parser.feed(data.mid(i, step));
    if (streamed) *streamed = text;
    if (finished) *finished = parser.finished();
    return parser.reply();
}

QByteArray sse(const QStringList& events)
{
    QByteArray out;
    for (const QString& e : events) {
        const QJsonObject o = QJsonDocument::fromJson(e.toUtf8()).object();
        out += "event: " + o["type"].toString().toUtf8() + "\ndata: " + e.toUtf8() + "\n\n";
    }
    return out;
}

void testAnthropicStream()
{
    const QByteArray data = sse({
        R"({"type":"message_start","message":{"id":"msg_1","usage":{"input_tokens":10,"cache_read_input_tokens":100,"cache_creation_input_tokens":5,"output_tokens":1}}})",
        R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":"","signature":""}})",
        R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"hm"}})",
        R"({"type":"content_block_delta","index":0,"delta":{"type":"signature_delta","signature":"sig-"}})",
        R"({"type":"content_block_delta","index":0,"delta":{"type":"signature_delta","signature":"abc"}})",
        R"({"type":"content_block_stop","index":0})",
        R"({"type":"content_block_start","index":1,"content_block":{"type":"text","text":""}})",
        R"({"type":"ping"})",
        R"({"type":"content_block_delta","index":1,"delta":{"type":"text_delta","text":"Schneide "}})",
        R"({"type":"content_block_delta","index":1,"delta":{"type":"text_delta","text":"bei 2 s ✂"}})",
        R"({"type":"content_block_stop","index":1})",
        R"({"type":"content_block_start","index":2,"content_block":{"type":"tool_use","id":"toolu_9","name":"edit","input":{}}})",
        R"({"type":"content_block_delta","index":2,"delta":{"type":"input_json_delta","partial_json":"{\"project\": \"@a"}})",
        R"({"type":"content_block_delta","index":2,"delta":{"type":"input_json_delta","partial_json":"pp\", \"ops\": [{\"op\":\"split\",\"at\":\"2s\"}]}"}})",
        R"({"type":"content_block_stop","index":2})",
        R"({"type":"content_block_start","index":3,"content_block":{"type":"tool_use","id":"toolu_10","name":"info","input":{}}})",
        R"({"type":"content_block_stop","index":3})",
        R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":42}})",
        R"({"type":"message_stop"})",
    });
    for (int step : {1, 7, 64, int(data.size())}) {
        QString streamed;
        bool finished = false;
        const Ai::Reply r = stream(Ai::Protocol::Anthropic, data, step, &streamed, &finished);
        CHECK(finished);
        CHECK_EQ(streamed, QString("Schneide bei 2 s ✂"));
        CHECK_EQ(r.text, QString("Schneide bei 2 s ✂"));
        CHECK_EQ(r.stop, QString("tool_use"));
        CHECK_EQ(r.usage.input, qint64(115));
        CHECK_EQ(r.usage.cached, qint64(100));
        CHECK_EQ(r.usage.output, qint64(42));
        if (!CHECK_EQ(r.calls.size(), 2)) continue;
        CHECK_EQ(r.calls[0].id, QString("toolu_9"));
        CHECK_EQ(r.calls[0].name, QString("edit"));
        CHECK_EQ(r.calls[0].input["project"].toString(), QString("@app"));
        CHECK_EQ(r.calls[0].input["ops"][0]["at"].toString(), QString("2s"));
        CHECK(r.calls[0].error.isEmpty());
        CHECK(r.calls[1].input.isEmpty() && r.calls[1].error.isEmpty()); // no arguments
        // The content to send back: the thinking block with its signature, the tool input as an object
        if (!CHECK_EQ(r.anthropic.size(), 4)) continue;
        CHECK_EQ(r.anthropic[0]["signature"].toString(), QString("sig-abc"));
        CHECK_EQ(r.anthropic[0]["thinking"].toString(), QString("hm"));
        CHECK_EQ(r.anthropic[2]["input"]["ops"][0]["op"].toString(), QString("split"));
    }
    // An error in the stream (overloaded before anything came), cut-off arguments, a refusal
    Ai::Reply r = stream(Ai::Protocol::Anthropic,
                         "event: error\ndata: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}\n\n", 5);
    CHECK_EQ(r.error, QString("Overloaded"));
    CHECK_EQ(r.errorType, QString("overloaded_error"));
    r = stream(Ai::Protocol::Anthropic,
               sse({R"({"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"t","name":"edit","input":{}}})",
                    R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{\"ops\": [{"}})",
                    R"({"type":"message_delta","delta":{"stop_reason":"max_tokens"},"usage":{"output_tokens":9}})"}),
               13);
    CHECK(r.cutOff());
    if (CHECK_EQ(r.calls.size(), 1)) CHECK(!r.calls[0].error.isEmpty());
    r = stream(Ai::Protocol::Anthropic, sse({R"({"type":"message_delta","delta":{"stop_reason":"refusal"}})"}), 100);
    CHECK(r.refused());
    r = stream(Ai::Protocol::Anthropic, sse({R"({"type":"message_delta","delta":{"stop_reason":"model_context_window_exceeded"}})"}), 100);
    CHECK(r.cutOff());
    // A server-side fallback in the middle: thinking and tool calls before the last "fallback" block are dropped
    r = stream(Ai::Protocol::Anthropic,
               sse({R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":"a","signature":"s"}})",
                    R"({"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"toolu_declined","name":"info","input":{}}})",
                    R"({"type":"content_block_start","index":2,"content_block":{"type":"fallback"}})",
                    R"({"type":"content_block_start","index":3,"content_block":{"type":"tool_use","id":"toolu_ok","name":"info","input":{}}})",
                    R"({"type":"message_delta","delta":{"stop_reason":"tool_use"}})", R"({"type":"message_stop"})"}),
               100);
    if (CHECK_EQ(r.calls.size(), 1)) CHECK_EQ(r.calls[0].id, QString("toolu_ok"));
    if (CHECK_EQ(r.anthropic.size(), 2)) CHECK_EQ(r.anthropic[0].toObject()["type"].toString(), QString("fallback"));
}

void testOpenAiStream()
{
    const auto chunk = [](const QString& o) { return "data: " + o.toUtf8() + "\n\n"; };
    QByteArray data = chunk(R"({"choices":[{"index":0,"delta":{"role":"assistant","content":"Ich "}}]})")
                      + chunk(R"({"choices":[{"index":0,"delta":{"content":"sehe nach."}}]})")
                      + chunk(R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"call_a","type":"function","function":{"name":"frames","arguments":""}}]}}]})")
                      + chunk(R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"source\":\"@app\","}}]}}]})")
                      + chunk(R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":1,"id":"call_b","type":"function","function":{"name":"info","arguments":"{}"}}]}}]})")
                      + chunk(R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"\"count\":2}"}}]}}]})")
                      + chunk(R"({"choices":[{"index":0,"delta":{},"finish_reason":"tool_calls"}]})")
                      + chunk(R"({"choices":[],"usage":{"prompt_tokens":300,"completion_tokens":20,"prompt_tokens_details":{"cached_tokens":256}}})")
                      + "data: [DONE]\n\n";
    for (int step : {1, 11, int(data.size())}) {
        QString streamed;
        bool finished = false;
        const Ai::Reply r = stream(Ai::Protocol::OpenAi, data, step, &streamed, &finished);
        CHECK(finished);
        CHECK_EQ(streamed, QString("Ich sehe nach."));
        CHECK_EQ(r.stop, QString("tool_calls"));
        CHECK_EQ(r.usage.input, qint64(300));
        CHECK_EQ(r.usage.cached, qint64(256));
        CHECK_EQ(r.usage.output, qint64(20));
        if (!CHECK_EQ(r.calls.size(), 2)) continue;
        CHECK_EQ(r.calls[0].id, QString("call_a"));
        CHECK_EQ(r.calls[0].name, QString("frames"));
        CHECK_EQ(r.calls[0].input["count"].toInt(), 2);
        CHECK_EQ(r.calls[1].name, QString("info"));
        CHECK(r.anthropic.isEmpty());
    }
    // Ollama: whole calls without an index, no blank lines between the events, arguments as an object
    data = "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"id\":\"call_1\",\"function\":{\"name\":\"info\",\"arguments\":"
           "\"{\\\"project\\\":\\\"@app\\\"}\"}},{\"function\":{\"name\":\"silence\",\"arguments\":{\"file\":\"@app\"}}}]}}]}\n"
           "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n"
           "data: [DONE]\n";
    Ai::Reply r = stream(Ai::Protocol::OpenAi, data, 3);
    if (CHECK_EQ(r.calls.size(), 2)) {
        CHECK_EQ(r.calls[0].input["project"].toString(), QString("@app"));
        CHECK_EQ(r.calls[1].name, QString("silence"));
        CHECK_EQ(r.calls[1].input["file"].toString(), QString("@app"));
        CHECK(!r.calls[1].id.isEmpty()); // made up: providers need one to match the answer
    }
    // An error in the stream (OpenRouter)
    r = stream(Ai::Protocol::OpenAi, chunk(R"({"error":{"message":"Provider returned error","code":502}})"), 4);
    CHECK_EQ(r.error, QString("Provider returned error"));
    // ... also next to "choices" in the middle of an answer; "error": null is none
    r = stream(Ai::Protocol::OpenAi,
               chunk(R"({"choices":[{"index":0,"delta":{"content":"Hal"}}],"error":null})")
                   + chunk(R"({"choices":[{"index":0,"delta":{},"finish_reason":"error"}],"error":{"message":"upstream timeout"}})"),
               7);
    CHECK_EQ(r.text, QString("Hal"));
    CHECK_EQ(r.error, QString("upstream timeout"));
    r = stream(Ai::Protocol::OpenAi, chunk(R"({"choices":[{"index":0,"delta":{},"finish_reason":"error"}]})"), 7);
    CHECK(!r.error.isEmpty());
}

void testBodiesAndErrors()
{
    // Not streamed (connection test, servers that ignore "stream")
    Ai::Reply r = Ai::parseBody(Ai::Protocol::Anthropic,
                                QJsonDocument::fromJson(R"({"content":[{"type":"text","text":"OK"},{"type":"tool_use","id":"t1","name":"info","input":{"x":1}}],
                                    "stop_reason":"tool_use","usage":{"input_tokens":3,"cache_read_input_tokens":4,"output_tokens":5}})").object());
    CHECK_EQ(r.text, QString("OK"));
    CHECK_EQ(r.calls.size(), 1);
    CHECK_EQ(r.usage.input, qint64(7));
    r = Ai::parseBody(Ai::Protocol::OpenAi,
                      QJsonDocument::fromJson(R"({"choices":[{"message":{"content":"OK","tool_calls":[{"id":"c","function":{"name":"info","arguments":"{\"y\":2}"}}]},
                          "finish_reason":"tool_calls"}],"usage":{"prompt_tokens":8,"completion_tokens":2}})").object());
    CHECK_EQ(r.text, QString("OK"));
    if (CHECK_EQ(r.calls.size(), 1)) CHECK_EQ(r.calls[0].input["y"].toInt(), 2);
    CHECK(Ai::parseBody(Ai::Protocol::OpenAi, {}).calls.isEmpty()); // empty answer: nothing, no crash

    Ai::Failure f = Ai::httpFailure(401, R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})");
    CHECK(f.kind == Ai::Failure::Kind::Key);
    CHECK(!f.retry);
    CHECK(f.message.contains("invalid x-api-key"));
    f = Ai::httpFailure(429, R"({"error":{"message":"Rate limit reached","type":"requests"}})", "7");
    CHECK(f.kind == Ai::Failure::Kind::RateLimit);
    CHECK(f.retry);
    CHECK_EQ(f.retryAfterS, 7);
    f = Ai::httpFailure(529, R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})");
    CHECK(f.kind == Ai::Failure::Kind::Overloaded && f.retry);
    f = Ai::httpFailure(404, R"({"error":"model \"qwen9\" not found, try pulling it first"})"); // Ollama
    CHECK(f.kind == Ai::Failure::Kind::NotFound);
    CHECK(f.message.contains("qwen9"));
    f = Ai::httpFailure(503, "<html><body>Bad Gateway</body></html>"); // a proxy
    CHECK(f.kind == Ai::Failure::Kind::Server && f.retry);
    CHECK(f.message.contains("Bad Gateway"));
    CHECK(Ai::httpFailure(400, R"({"error":{"message":"bad"}})").kind == Ai::Failure::Kind::Request);
    CHECK(Ai::httpFailure(413, "{}").kind == Ai::Failure::Kind::TooLarge);
    // A request larger than the model's context, in the words of local servers: the numbers and what helps
    {
        const Ai::Failure llama = Ai::httpFailure(
            400, R"({"error":{"code":400,"message":"request (15768 tokens) exceeds the available context size (4096 tokens), try increasing it","type":"exceed_context_size_error"}})");
        CHECK(llama.kind == Ai::Failure::Kind::TooLarge && !llama.retry);
        CHECK(llama.message.contains("4096") && llama.message.contains("15768") && llama.message.contains("32768"));
        // The short request: less is needed and said
        const Ai::Failure small = Ai::httpFailure(
            400, R"x({"error":{"message":"request (6400 tokens) exceeds the available context size (4096 tokens)"}})x", {}, 6400);
        CHECK(small.message.contains("16384") && small.message.contains("6400") && !small.message.contains("32768"));
        qint64 context = 0, request = 0;
        CHECK(Ai::contextOverflow("This model's maximum context length is 8192 tokens. However, your messages resulted in "
                                  "15768 tokens. Please reduce the length of the messages.",
                                  &context, &request));
        CHECK(context == 8192 && request == 15768);
        CHECK(Ai::contextOverflow("Trying to keep the first 15768 tokens when context the overflows. However, the model is "
                                  "loaded with context length of only 4096 tokens, which is not enough.",
                                  &context, &request));
        CHECK(context == 4096 && request == 0);
        CHECK(!Ai::contextOverflow("model 'qwen' not found", &context, &request));
        // A large context that is full: a new chat helps, not the server's settings
        CHECK(Ai::contextAdvice(131072, 140000).contains(T("„Neuer Chat“ fängt frisch an.")));
        CHECK(!Ai::contextAdvice(131072, 140000).contains("32768"));
        CHECK(Ai::contextAdvice(16384, 17000, false, 6000).contains(T("„Neuer Chat“ fängt frisch an.")));
        CHECK(Ai::httpFailure(400, R"({"error":{"message":"bad tool schema"}})").kind == Ai::Failure::Kind::Request);
    }

    CHECK_EQ(Ai::parseModels(QJsonDocument::fromJson(R"({"data":[{"id":"gpt-5"},{"id":"gpt-4.1"}],"has_more":false})").object()),
             QStringList({"gpt-4.1", "gpt-5"}));
    CHECK_EQ(Ai::parseModels(QJsonDocument::fromJson(R"({"models":[{"name":"qwen3:8b"}]})").object()), QStringList{"qwen3:8b"});
}

// A keychain in memory
class MemoryStore : public Ai::KeyStore {
public:
    std::map<QString, QString> keys;
    bool secureStore = true;
    bool failWrite = false;
    int reads = 0;
    QString name() const override { return "memory"; }
    bool secure() const override { return secureStore; }
    bool has(const QString& p) override { return keys.count(p) > 0; }
    QString read(const QString& p, QString*) override
    {
        ++reads;
        return keys.count(p) ? keys[p] : QString();
    }
    bool write(const QString& p, const QString& k, QString* error) override
    {
        if (failWrite) {
            if (error) *error = "locked";
            return false;
        }
        keys[p] = k;
        return true;
    }
    bool remove(const QString& p, QString*) override
    {
        keys.erase(p);
        return true;
    }
};

void writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(data);
}

void testKeys()
{
    std::map<QString, QString> env;
    auto chain = std::make_unique<MemoryStore>();
    auto plain = std::make_unique<MemoryStore>();
    MemoryStore* keychain = chain.get();
    MemoryStore* settings = plain.get();
    settings->secureStore = false;
    Ai::Keys keys(std::move(chain), std::move(plain), [&env](const QString& n) { return env.count(n) ? env[n] : QString(); });
    using K = Ai::KeySource::Kind;
    const Ai::Company none;
    CHECK(keys.resolve("claude", none).kind == K::None);
    CHECK(keys.secure());

    // Stored by the user: the keychain, no second plain copy
    settings->keys["claude"] = "old-plain";
    CHECK(keys.store("claude", "sk-chain", nullptr));
    CHECK_EQ(keychain->keys["claude"], QString("sk-chain"));
    CHECK(!settings->has("claude"));
    Ai::KeySource s = keys.resolve("claude", none);
    CHECK(s.kind == K::Keychain);
    CHECK_EQ(s.key, QString("sk-chain"));
    keys.resolve("claude", none);
    CHECK_EQ(keychain->reads, 1); // read once per session (unlocking may ask the user)
    CHECK(keys.hasStored("claude"));
    CHECK(!keys.hasStored("openai"));
    // Whether a key is stored is known without asking the keychain (it could start its service or a prompt); one
    // put there otherwise is still found when it is needed
    keychain->keys["openai"] = "sk-elsewhere";
    CHECK(!keys.hasStored("openai"));
    CHECK_EQ(keys.resolve("openai", none).key, QString("sk-elsewhere"));
    CHECK(keys.hasStored("openai"));
    keys.remove("openai", nullptr);
    CHECK(!keys.hasStored("openai"));

    // A company key for that provider (or for any, without "provider") comes before it, the environment first
    Ai::Company company;
    company.file = "/etc/xdg/schneidi/schneidi-ai.json";
    company.key = "sk-company";
    company.provider = "openai";
    CHECK(keys.resolve("claude", company).kind == K::Keychain); // not for this provider
    company.provider.clear();
    s = keys.resolve("claude", company);
    CHECK(s.kind == K::Company);
    CHECK_EQ(s.key, QString("sk-company"));
    env["ANTHROPIC_API_KEY"] = "sk-env";
    s = keys.resolve("claude", company);
    CHECK(s.kind == K::Environment);
    CHECK_EQ(s.where, QString("ANTHROPIC_API_KEY"));
    env["SCHNEIDI_AI_KEY"] = "sk-schneidi";
    CHECK_EQ(keys.resolve("claude", company).key, QString("sk-schneidi"));
    // Another provider's variable never: an Anthropic key does not go to OpenRouter
    env.erase("SCHNEIDI_AI_KEY");
    CHECK(keys.resolve("openrouter", none).kind == K::None);
    env["OPENAI_API_KEY"] = "sk-openai";
    CHECK(keys.resolve("openrouter", none).kind == K::None);
    CHECK_EQ(keys.resolve("openai", none).key, QString("sk-openai"));
    CHECK_EQ(Ai::keyVariables("custom"), QStringList{"SCHNEIDI_AI_KEY"});

    // Removing: from both
    CHECK(keys.remove("claude", nullptr));
    CHECK(!keychain->has("claude"));
    env.clear();
    CHECK(keys.resolve("claude", none).kind == K::None);
    // A keychain that refuses: the error, nothing stored in plain text instead
    keychain->failWrite = true;
    QString error;
    CHECK(!keys.store("claude", "sk-x", &error));
    CHECK_EQ(error, QString("locked"));
    CHECK(!settings->has("claude"));

    // Without a keychain: the settings (unencrypted)
    auto plain2 = std::make_unique<MemoryStore>();
    MemoryStore* settings2 = plain2.get();
    settings2->secureStore = false;
    Ai::Keys noChain(nullptr, std::move(plain2), [](const QString&) { return QString(); });
    CHECK(!noChain.secure());
    CHECK(noChain.store("ollama", "local", nullptr));
    CHECK_EQ(settings2->keys["ollama"], QString("local"));
    s = noChain.resolve("ollama", none);
    CHECK(s.kind == K::Settings);
    CHECK_EQ(noChain.storeName(), QString("memory"));

    // The settings store itself (QSettings of the test program)
    auto store = Ai::settingsKeyStore();
    CHECK(!store->secure());
    store->remove("claude", nullptr);
    CHECK(!store->has("claude"));
    CHECK(store->write("claude", "sk-s", nullptr));
    CHECK(store->has("claude"));
    CHECK_EQ(store->read("claude", nullptr), QString("sk-s"));
    store->remove("claude", nullptr);
    CHECK(!store->has("claude"));
    // Tests never touch the user's keychain
    CHECK(!Ai::systemKeyStore());
}

void testCompany()
{
    QTemporaryDir tmp;
    const QString user = tmp.path() + "/user", system = tmp.path() + "/system";
    CHECK(!Ai::company({user, system}).found());
    // System-wide: provider, address, key in a file next to it; the user's file changes the model
    writeFile(system + "/schneidi-ai.json",
              R"({"provider":"openai","base_url":"https://llm.firma.example/v1","model":"gpt-x","key_file":"key.txt"})");
    writeFile(system + "/key.txt", "sk-firma\n");
    writeFile(user + "/schneidi-ai.json", R"({"model":"gpt-y"})");
    Ai::Company c = Ai::company({user, system});
    CHECK(c.found());
    CHECK(c.error.isEmpty());
    CHECK_EQ(c.provider, QString("openai"));
    CHECK_EQ(c.address, QString("https://llm.firma.example/v1"));
    CHECK_EQ(c.model, QString("gpt-y"));
    CHECK_EQ(c.key, QString("sk-firma"));
    CHECK(!c.locked);

    // The provider choice: the user's own settings win, the company fills in, the provider's defaults last
    Ai::Choice ch = Ai::choose({}, {}, {}, c);
    CHECK_EQ(ch.provider, QString("openai"));
    CHECK_EQ(ch.address, QString("https://llm.firma.example/v1"));
    CHECK_EQ(ch.model, QString("gpt-y"));
    ch = Ai::choose("claude", {}, {}, c);
    CHECK_EQ(ch.address, QString("https://api.anthropic.com"));
    CHECK_EQ(ch.model, QString("claude-sonnet-5-5"));
    ch = Ai::choose("openai", "https://other.example/v1", {{"openai", "mine"}}, c);
    CHECK_EQ(ch.address, QString("https://other.example/v1"));
    CHECK_EQ(ch.model, QString("mine"));
    CHECK(!ch.locked);

    // Locked: provider and address from the company, whatever the user chose (the model stays the user's)
    writeFile(system + "/schneidi-ai.json",
              R"({"provider":"anthropic","address":"https://gateway.firma.example","key":"sk-lock","locked":true})");
    c = Ai::company({user, system});
    CHECK(c.locked);
    CHECK_EQ(c.provider, QString("claude"));
    CHECK_EQ(c.model, QString()); // the user's file does not reach into a locked one
    ch = Ai::choose("ollama", "http://localhost:11434/v1", {{"claude", "claude-opus-5-5"}}, c);
    CHECK(ch.locked);
    CHECK_EQ(ch.provider, QString("claude"));
    CHECK_EQ(ch.address, QString("https://gateway.firma.example"));
    CHECK_EQ(ch.model, QString("claude-opus-5-5"));

    // Broken files say why, and are not locked
    writeFile(system + "/schneidi-ai.json", "{ nope");
    c = Ai::company({system});
    CHECK(c.found() && !c.error.isEmpty() && !c.locked);
    CHECK_EQ(Ai::choose("claude", {}, {}, c).provider, QString("claude"));
    writeFile(system + "/schneidi-ai.json", R"({"provider":"skynet","locked":true})");
    c = Ai::company({system});
    CHECK(!c.error.isEmpty());
    CHECK(!Ai::choose("claude", {}, {}, c).locked);
}

void testChatHelpers()
{
    Ai::ToolResult r{"t", "info", QString(AiChat::kMaxToolText + 500, 'x'), {}, false};
    for (int i = 0; i < AiChat::kMaxImages + 3; ++i) r.images << Ai::Image{"QUJD", "image/jpeg"};
    const Ai::ToolResult c = AiChat::capped(r);
    CHECK(c.text.size() < r.text.size() + 400);
    CHECK(c.text.contains("cut by the schneidi app"));
    CHECK(c.text.contains(QString::number(r.text.size())));
    CHECK_EQ(c.images.size(), AiChat::kMaxImages);
    CHECK(c.text.contains("only the first"));
    const Ai::ToolResult small = AiChat::capped({"t", "info", "{\"ok\":true}", {}, false});
    CHECK_EQ(small.text, QString("{\"ok\":true}"));

    CHECK_EQ(AiChat::toolLine("edit", {{"project", "@app"}, {"ops", QJsonArray{QJsonObject{{"op", "split"}}, QJsonObject{{"op", "delete"}}}}}),
             T("Bearbeitet: %1").arg("split, delete"));
    CHECK_EQ(AiChat::toolLine("edit", {{"ops", R"([{"op":"title"}])"}}), T("Bearbeitet: %1").arg("title"));
    CHECK(AiChat::toolLine("silence", {{"file", "/x/talk.wav"}}).endsWith(" · talk.wav"));
    CHECK(AiChat::toolLine("info", {{"project", "@app"}}).endsWith(T("offenes Projekt")));
    CHECK(AiChat::toolLine("frames", {{"sources", QJsonArray{"/a.mp4", "/b.mp4"}}}).endsWith("a.mp4 +1"));
    CHECK_EQ(AiChat::toolLine("app", {{"action", "seek"}}), T("Steuert die App: %1").arg("seek"));
}

// Tool calls a small local model wrote as text: whole answers of calls only (also fenced, listed, in <tool_call> tags,
// arguments as a string, OpenAI's own shape); prose, other JSON and unknown keys are no calls
void testTextCalls()
{
    const auto one = [](const QString& text) {
        const QVector<Ai::ToolCall> calls = Ai::textCalls(text, "text_3");
        return calls.size() == 1 ? calls.first() : Ai::ToolCall{};
    };
    Ai::ToolCall c = one(R"({"name": "edit", "arguments": {"ops": [{"op": "trim", "clip": 1}]}})");
    CHECK_EQ(c.name, QString("edit"));
    CHECK_EQ(c.id, QString("text_3_1"));
    CHECK_EQ(c.input["ops"][0]["op"].toString(), QString("trim"));
    CHECK(c.error.isEmpty());
    c = one("```json\n{\"name\":\"info\",\"parameters\":{\"project\":\"@app\"}}\n```");
    CHECK_EQ(c.name, QString("info"));
    CHECK_EQ(c.input["project"].toString(), QString("@app"));
    c = one("{\"name\": \"edit\", \"arguments\": {\"project\": \"@app\"}}\n```"); // a stray closing fence
    CHECK_EQ(c.input["project"].toString(), QString("@app"));
    c = one("<|im_start|>\n{\"name\": \"guide\", \"arguments\": {\"topic\":\"social\"}}\n```"); // a leaked marker
    CHECK_EQ(c.input["topic"].toString(), QString("social"));
    c = one("<tool_response>\n{\"name\": \"new\", \"arguments\": {\"template\": \"vertical\"}}\n</tool_response>"); // another tag
    CHECK_EQ(c.name, QString("new"));
    c = one(R"({"type":"function","function":{"name":"info","arguments":"{\"summary\":true}"}})");
    CHECK_EQ(c.name, QString("info"));
    CHECK_EQ(c.input["summary"].toBool(), true);
    c = one(R"({"name":"info","arguments":"{\"summary\":"})"); // broken arguments: still a call, with the error
    CHECK_EQ(c.name, QString("info"));
    CHECK(!c.error.isEmpty());
    const QVector<Ai::ToolCall> tagged = Ai::textCalls(
        "Ich sehe mir das an.\n<tool_call>\n{\"name\":\"info\",\"arguments\":{}}\n</tool_call>\n<tool_call>{\"name\":"
        "\"frames\",\"arguments\":{\"source\":\"@app\"}}",
        "t");
    if (CHECK_EQ(tagged.size(), 2)) {
        CHECK_EQ(tagged[1].name, QString("frames"));
        CHECK_EQ(tagged[1].id, QString("t_2"));
    }
    CHECK_EQ(Ai::textCalls(R"([{"name":"info","arguments":{}},{"name":"check","arguments":{}}])", "t").size(), 2);
    CHECK_EQ(Ai::textCalls("{\"name\":\"info\",\"arguments\":{}}\n{\"name\":\"check\",\"arguments\":{}}", "t").size(), 2);
    // A plan of several calls over several lines each (qwen-coder): all of them, in order
    const QVector<Ai::ToolCall> plan = Ai::textCalls(
        "```json\n{\"name\": \"new_timeline\", \"arguments\": {\"name\": \"Kills\"}}\n{\"name\": \"edit\", \"arguments\": {\n"
        "    \"ops\": [\n        {\"op\": \"keep\", \"ranges\": [[\"0s\", \"1m\"]]}\n    ]\n}}\n```",
        "t");
    if (CHECK_EQ(plan.size(), 2)) CHECK(plan[0].name == "new_timeline" && plan[1].input["ops"][0]["op"] == "keep");
    // No calls
    for (const char* text : {"Fertig: der Clip ist 10 s lang.", R"(So geht es: {"name":"info","arguments":{}})",
                             R"({"name":"Timeline 1","length":120})", R"({"name":"info"})",
                             R"({"name":"info","arguments":{},"extra":1})", R"([{"name":"info","arguments":{}}, 5])", "{}",
                             "<tool_call>kein JSON</tool_call>", ""})
        CHECK(Ai::textCalls(QString::fromUtf8(text), "t").isEmpty());
}

// Only on a private session bus with a Secret Service (see the top of the file)
// The short request and the context of the model (app/AiContext.h)
void testContext()
{
    // When the short request is taken
    Ai::Endpoint local{Ai::Protocol::OpenAi, "http://localhost:11434/v1", {}, "qwen3", "ollama"};
    Ai::Endpoint remote{Ai::Protocol::OpenAi, "https://llm.example.com/v1", "k", "big", "custom"};
    Ai::Endpoint claude{Ai::Protocol::Anthropic, "https://api.anthropic.com", "k", "claude-sonnet-5-5", "claude"};
    CHECK(Ai::useCompact(Ai::Compact::Auto, local, 0));
    CHECK(Ai::useCompact(Ai::Compact::Auto, local, 131072)); // on this computer: speed first
    CHECK(!Ai::useCompact(Ai::Compact::Auto, remote, 0));
    CHECK(!Ai::useCompact(Ai::Compact::Auto, remote, 131072));
    CHECK(Ai::useCompact(Ai::Compact::Auto, remote, 16384));
    CHECK(!Ai::useCompact(Ai::Compact::Auto, claude, 0));
    CHECK(!Ai::useCompact(Ai::Compact::Off, local, 4096));
    CHECK(Ai::useCompact(Ai::Compact::On, claude, 0));
    CHECK_EQ(Ai::recommendedContext(6500), qint64(16384));
    CHECK_EQ(Ai::recommendedContext(15000), qint64(32768));
    CHECK_EQ(Ai::recommendedContext(100), qint64(8192));

    // First sentences: "e.g." ends nothing, long ones end at a clause outside brackets
    CHECK_EQ(Ai::firstSentence("Find pauses in the audio. Ranges are frames."), QString("Find pauses in the audio."));
    CHECK_EQ(Ai::firstSentence("Show a project, e.g. its clips. More."), QString("Show a project, e.g. its clips."));
    CHECK_EQ(Ai::firstSentence("Tempo of a song in one call, to cut to the music (not to a grid): bpm (70-220 by "
                               "default, half/double tempo resolved by which beats are loud) with a confidence, the "
                               "beat grid and more and more and more."),
             QString("Tempo of a song in one call, to cut to the music (not to a grid)."));
    CHECK(Ai::firstSentence(QString(400, 'x')).size() <= 182);

    // The tools in short: all of them, in order; `edit` lists its ops' signatures, rare tools only parameter names
    const auto tool = [](const QString& name, const QString& description, const QJsonObject& props) {
        return QJsonObject{{"name", name}, {"description", description},
                           {"inputSchema", QJsonObject{{"type", "object"}, {"properties", props}}}};
    };
    const QString longText = "where the kill feed is, \"x,y,w,h\" (fractions 0..1 or pixels; look with `frames` crop "
                             "first). Only the part with the player's mark";
    const QJsonArray tools{
        tool("help", "Details of the edit ops.", {{"command", QJsonObject{{"type", "string"}, {"description", "x"}}}}),
        tool("edit", "Apply edit operations to a project and save it. Answer: lots.",
             {{"ops", QJsonObject{{"type", "array"}, {"description", QString(10000, 'o')}}},
              {"project", QJsonObject{{"type", "string"}, {"description", "project file"}}}}),
        tool("highlights", "Candidates for a highlight video. More text.",
             {{"region", QJsonObject{{"type", "string"}, {"description", longText}}}}),
        tool("watch", "Watch folders: new files come in. More.",
             {{"folder", QJsonObject{{"type", "string"}, {"description", "the folder to watch"}}}}),
    };
    const QJsonArray ops{QJsonObject{{"op", "split"}, {"usage", "{op:'split', at, clip?}"}, {"summary", "cut"}},
                         QJsonObject{{"op", "trim"}, {"usage", "{op:'trim', clip, edge?, by|to}"}}};
    const QJsonArray c = Ai::compactTools(tools, ops);
    if (CHECK_EQ(c.size(), 4)) {
        CHECK_EQ(c[0]["name"].toString(), QString("help"));
        CHECK(c[0]["description"].toString().contains("command '<tool>'"));
        const QString edit = c[1]["inputSchema"]["properties"]["ops"]["description"].toString();
        CHECK(edit.contains("{op:'split', at, clip?}; {op:'trim', clip, edge?, by|to}"));
        CHECK(edit.contains("`help` with command 'edit' and op '<name>'") && edit.size() < 1000);
        CHECK_EQ(c[1]["description"].toString(), QString("Apply edit operations to a project and save it."));
        CHECK_EQ(c[1]["inputSchema"]["properties"]["project"]["description"].toString(), QString("project file"));
        CHECK_EQ(c[2]["inputSchema"]["properties"]["region"]["description"].toString(),
                 QString("where the kill feed is, \"x,y,w,h\""));
        CHECK(!c[3]["inputSchema"]["properties"]["folder"].toObject().contains("description"));
        CHECK_EQ(c[3]["inputSchema"]["properties"]["folder"]["type"].toString(), QString("string"));
        CHECK(c[3]["description"].toString().contains("`help` with command 'watch'"));
    }
    // Without the ops `edit` stays whole (the model could not edit otherwise)
    CHECK_EQ(Ai::compactTools(tools, {})[1].toObject(), tools[1].toObject());
    CHECK(Ai::compactGuide("- social: Short").contains("call `guide` with the topic before such a task):\n- social: Short"));

    // Who is asked for the context, and how
    const auto kinds = [](const Ai::Endpoint& e) {
        QStringList k;
        for (const Ai::ContextProbe& p : Ai::contextProbes(e)) k << p.kind;
        return k;
    };
    CHECK(kinds(claude).isEmpty());
    CHECK(kinds(Ai::Endpoint{Ai::Protocol::OpenAi, "https://api.openai.com/v1", "k", "gpt-5", "openai"}).isEmpty());
    CHECK_EQ(kinds(local), QStringList({"ollama-show", "ollama-ps"}));
    CHECK_EQ(kinds(remote), QStringList({"ollama-show", "ollama-ps", "llama.cpp", "lmstudio", "models"}));
    for (const Ai::ContextProbe& p : Ai::contextProbes(remote)) {
        if (p.kind == "llama.cpp") CHECK_EQ(p.url.toString(), QString("https://llm.example.com/props"));
        if (p.kind == "models") CHECK_EQ(p.url.toString(), QString("https://llm.example.com/v1/models"));
        if (p.kind == "ollama-show") {
            CHECK_EQ(p.method, QByteArray("POST"));
            CHECK_EQ(QJsonDocument::fromJson(p.body).object()["model"].toString(), QString("big"));
        }
    }
    const auto json = [](const char* text) { return QJsonDocument::fromJson(text).object(); };
    CHECK_EQ(Ai::contextOf("ollama-show", json(R"({"parameters":"stop \"<|im_end|>\"\nnum_ctx                        16384","model_info":{"qwen2.context_length":32768}})"), "qwen3"),
             qint64(16384));
    CHECK_EQ(Ai::contextOf("ollama-show", json(R"({"model_info":{"qwen2.context_length":32768}})"), "qwen3"), qint64(0));
    CHECK_EQ(Ai::contextOf("ollama-ps", json(R"({"models":[{"name":"other:7b","context_length":2048},{"name":"qwen3:latest","model":"qwen3:latest","context_length":4096}]})"), "qwen3"),
             qint64(4096));
    CHECK_EQ(Ai::contextOf("ollama-ps", json(R"({"models":[]})"), "qwen3"), qint64(0));
    CHECK_EQ(Ai::contextOf("llama.cpp", json(R"({"default_generation_settings":{"n_ctx":8192},"total_slots":1})"), "x"), qint64(8192));
    CHECK_EQ(Ai::contextOf("llama.cpp", json(R"({"n_ctx":16384})"), "x"), qint64(16384));
    CHECK_EQ(Ai::contextOf("lmstudio", json(R"({"data":[{"id":"qwen3","state":"loaded","max_context_length":131072,"loaded_context_length":4096}]})"), "qwen3"),
             qint64(4096));
    CHECK_EQ(Ai::contextOf("models", json(R"({"data":[{"id":"big","max_model_len":32768}]})"), "big"), qint64(32768));
    CHECK_EQ(Ai::contextOf("models", json(R"({"data":[{"id":"other","max_model_len":32768}]})"), "big"), qint64(0));
    CHECK_EQ(Ai::contextFrom({{"ollama-ps", 4096}, {"ollama-show", 16384}}), qint64(16384)); // num_ctx counts
    CHECK_EQ(Ai::contextFrom({{"ollama-ps", 4096}, {"ollama-show", 0}}), qint64(4096));
    CHECK_EQ(Ai::contextFrom({{"llama.cpp", 0}}), qint64(0));
}

// Final answers a small model wrote as JSON: the text
void testTextAnswer()
{
    CHECK_EQ(Ai::textAnswer(R"({"text": "Fertig."})"), QString("Fertig."));
    CHECK_EQ(Ai::textAnswer("```json\n{\"answer\": \"Zwei Clips.\"}\n```"), QString("Zwei Clips."));
    CHECK_EQ(Ai::textAnswer(R"({"type": "text", "text": "Ok"})"), QString("Ok"));
    CHECK_EQ(Ai::textAnswer(R"({"text": "a", "clips": 3})"), QString(R"({"text": "a", "clips": 3})"));
    CHECK_EQ(Ai::textAnswer(R"({"answer": 3})"), QString(R"({"answer": 3})"));
    CHECK_EQ(Ai::textAnswer("Plain text."), QString("Plain text."));
}

void testSecretService()
{
    if (qEnvironmentVariable("AI_KEYCHAIN_TEST") != "1") return;
    auto store = Ai::secretServiceStore();
    if (!CHECK(store)) return;
    QString error;
    store->remove("test-provider", &error);
    CHECK(!store->has("test-provider"));
    CHECK(store->write("test-provider", "sk-secret-ü", &error));
    CHECK(store->has("test-provider"));
    CHECK_EQ(store->read("test-provider", &error), QString("sk-secret-ü"));
    CHECK(store->write("test-provider", "sk-second", &error)); // replaces
    CHECK_EQ(store->read("test-provider", &error), QString("sk-second"));
    CHECK(store->remove("test-provider", &error));
    CHECK(!store->has("test-provider"));
    std::printf("Secret Service: geprüft (%s)\n", qPrintable(store->name()));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("ai_providers");
    QSettings().remove("ai"); // from an earlier run
    I18n::install("de");
    testPresets();
    testAnthropicBody();
    testOpenAiBody();
    testAnthropicStream();
    testOpenAiStream();
    testBodiesAndErrors();
    testKeys();
    testCompany();
    testChatHelpers();
    testTextCalls();
    testTextAnswer();
    testContext();
    testSecretService();
    return Check::result();
}
