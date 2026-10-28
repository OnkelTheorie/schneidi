#pragma once
// AI chat providers (the "AI" panel, app/AiChat.h). Two protocols cover nearly all of them: Anthropic's Messages API
// and the OpenAI-compatible /chat/completions (OpenAI, OpenRouter, Ollama, LM Studio, vLLM, Mistral, Groq, company
// proxies …). This file builds their requests from one neutral conversation and reads their answers (streamed as
// server-sent events, or one JSON body); the network itself is app/AiClient.h. No widgets, no network: tested
// directly (tests/test_ai_providers.cpp).

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

namespace Ai {

enum class Protocol { Anthropic, OpenAi };

// What the user picks (Settings › AI, the chat's setup); the address and model stay editable
struct Preset {
    QString id;        // "claude", "openai", "openrouter", "ollama", "custom" (saved in the settings)
    const char* name;  // N_() text, shown with T()
    Protocol protocol;
    QString address;   // default base URL (empty: the user gives one)
    QString model;     // default model (empty: the user gives one)
    QString keyEnv;    // environment variable with this provider's key (empty: only SCHNEIDI_AI_KEY)
    bool needsKey;     // false: works without one (local servers)
    bool modelList;    // the provider lists its models (GET …/models)
};
const QVector<Preset>& presets();
const Preset* preset(const QString& id); // nullptr = unknown
QString presetName(const QString& id);   // translated, the id itself when unknown

// Where requests go
struct Endpoint {
    Protocol protocol = Protocol::Anthropic;
    QString address; // base URL ("https://api.anthropic.com", "http://localhost:11434/v1")
    QString key;     // empty: no key sent (local servers)
    QString model;
    QString provider; // preset id (some request details depend on it)
};
// Loopback address: nothing leaves this computer (no privacy notice)
bool isLocal(const QString& address);
// A host on this computer or in the local network (private and link-local addresses, *.local): reached directly, never
// through the system's proxy (which cannot reach it)
bool isNearHost(const QString& host);
QUrl chatUrl(const Endpoint& e);   // …/v1/messages or …/chat/completions
QUrl modelsUrl(const Endpoint& e); // …/v1/models or …/models
// Headers of every request (key, protocol version); the key never goes anywhere else
QList<QPair<QByteArray, QByteArray>> headers(const Endpoint& e, bool fallbacks = false);
// Anthropic's server-side fallback on refusals (beta): only on Anthropic's own API with the models that have it
bool fallbacksFor(const Endpoint& e);

// ---------- Conversation ----------

struct ToolCall {
    QString id, name;
    QJsonObject input;
    QString error; // the arguments could not be read (cut off, not JSON)
};
struct Image {
    QByteArray base64;
    QString mime = "image/jpeg";
};
struct ToolResult {
    QString id, name;
    QString text;
    QVector<Image> images;
    bool error = false;
};
struct Message {
    enum class Role { User, Assistant };
    Role role = Role::User;
    QString text;                // user: what they wrote (and notes of the app); assistant: its text
    QVector<ToolResult> results; // user: answers to the tool calls of the assistant message before (sent first)
    QVector<ToolCall> calls;     // assistant
    // Assistant, Anthropic: its content exactly as it came (thinking blocks with their signature …), sent back
    // unchanged; the history is only ever appended to
    QJsonArray anthropic;
};

struct Usage {
    qint64 input = 0;  // all input tokens of the request (cache reads and writes included)
    qint64 output = 0;
    qint64 cached = 0; // of the input: read from the provider's cache (cheaper)
    Usage& operator+=(const Usage& o)
    {
        input += o.input;
        output += o.output;
        cached += o.cached;
        return *this;
    }
};

struct Request {
    QString model;
    QString system;
    QJsonArray tools; // MCP tools as tools/list gives them: name, description, inputSchema
    QVector<Message> messages;
    int maxTokens = 32000; // thinking counts too (on by default in the 5.x models): room for long edit calls
    bool stream = true;
    bool fallbacks = false; // Anthropic: fallbacksFor()
    bool autoCache = true;  // Anthropic: cache marks (the system prompt and, top-level, the growing conversation)
    QString provider;       // preset id
    qint64 fixedTokens = 0; // about, the system prompt and the tools: what a too small context is told (0 = the full
                            // request's ~15,000)
};
QJsonObject anthropicBody(const Request& r);
QJsonObject openAiBody(const Request& r);
QJsonObject body(Protocol p, const Request& r);

// ---------- Answers ----------

struct Reply {
    QString text;
    QVector<ToolCall> calls;
    QJsonArray anthropic; // Anthropic: the content blocks as they came (see Message::anthropic)
    QString stop;         // why the answer ended: "end_turn"/"stop", "tool_use"/"tool_calls", "max_tokens"/"length", "refusal" …
    Usage usage;
    QString error;     // the provider reported an error in the stream (or the stream broke off)
    QString errorType; // e.g. "overloaded_error"
    bool refused() const { return stop == "refusal" || stop == "content_filter"; }
    bool cutOff() const { return stop == "max_tokens" || stop == "length" || stop == "model_context_window_exceeded"; }
};

// Reads a streamed answer (server-sent events) piece by piece
class StreamParser {
public:
    explicit StreamParser(Protocol p) : m_protocol(p) {}
    // Received bytes (any split); returns the text that arrived with them (shown while it streams)
    QString feed(const QByteArray& bytes);
    bool finished() const { return m_finished; } // the provider said it is done
    bool started() const { return m_started; }   // anything arrived (text, a tool call …)
    Reply reply() const;                         // what arrived so far, tool arguments read

private:
    QString event(const QJsonObject& o);
    QString anthropicEvent(const QJsonObject& o);
    QString openAiEvent(const QJsonObject& o);

    Protocol m_protocol;
    QByteArray m_buffer;
    QByteArray m_data; // "data:" lines of the event being read
    bool m_finished = false;
    bool m_started = false;
    Reply m_reply;
    // Anthropic: content blocks by index, the tool input's JSON as it streams
    QVector<QJsonObject> m_blocks;
    QHash<int, QString> m_json;
    qint64 m_tokens[3] = {0, 0, 0}; // input: plain, read from the cache, written to it
    // OpenAI: tool calls as they stream (arguments as text)
    struct PartialCall {
        QString id, name, arguments;
    };
    QVector<PartialCall> m_calls;
};

// One answer as a JSON body (not streamed)
Reply parseBody(Protocol p, const QJsonObject& body);

// Tool calls a model wrote into its answer instead of making them (small local models over the OpenAI protocol): the
// whole answer is {"name":…,"arguments":{…}} ("parameters" too, arguments also as a JSON string), a list or lines of
// them, maybe in a ```json fence; or such objects in <tool_call> tags (text around them allowed). None when the answer
// is anything else. Ids: idPrefix + "_1", "_2" …
QVector<ToolCall> textCalls(const QString& text, const QString& idPrefix);
// A final answer a small model wrote as JSON: {"text": "…"} ("answer", "response", "reply", "message", "content",
// "final_answer"; maybe in a ```json fence, with "type"/"role" beside it) -> the text; else the answer as it is
QString textAnswer(const QString& text);

// An error answer: HTTP status, body and Retry-After -> what the user reads, whether trying again may help
struct Failure {
    enum class Kind { Key, RateLimit, Overloaded, NotFound, TooLarge, Request, Server, Network, Timeout, Other };
    Kind kind = Kind::Other;
    QString message; // translated, with the provider's own words
    bool retry = false;
    int retryAfterS = 0;
};
// fixedTokens: see Request
Failure httpFailure(int status, const QByteArray& body, const QByteArray& retryAfter = {}, qint64 fixedTokens = 0);
// The provider's message inside an error body (Anthropic, OpenAI, Ollama shapes); empty = none
QString providerMessage(const QByteArray& body);
// A request too large for the model's context in the words of llama.cpp servers, OpenAI/vLLM or LM Studio: the
// context and the request in tokens (0 = not said); false = no such message
bool contextOverflow(const QString& said, qint64* context, qint64* request);
// What the user reads then: the numbers and what helps (a small context: more of it in the server's settings, else a
// new chat). silent = the server cut the request without saying so (Ollama), request is an estimate then. fixed: the
// tokens of instructions and tools (0 = the full request's ~15,000)
QString contextAdvice(qint64 context, qint64 request, bool silent = false, qint64 fixed = 0);

// Model ids from a model list answer, sorted
QStringList parseModels(const QJsonObject& body);

} // namespace Ai
