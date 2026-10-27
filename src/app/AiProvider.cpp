#include "app/AiProvider.h"

#include "core/I18n.h"

#include <QHostAddress>
#include <QJsonDocument>
#include <QRegularExpression>

#include <algorithm>

namespace Ai {

namespace {

// Anthropic API version of every request (the current one: no temperature/top_p/top_k, which we never send)
const QByteArray kAnthropicVersion = "2026-10-21";
const QByteArray kFallbackBeta = "server-side-fallback-2026-07-01";

QString trimmedAddress(const QString& a)
{
    QString s = a.trimmed();
    while (s.endsWith('/')) s.chop(1);
    return s;
}

QJsonObject parseObject(const QByteArray& data)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    return err.error == QJsonParseError::NoError ? doc.object() : QJsonObject();
}

// Tool arguments as the model wrote them; an empty text is "no arguments"
QJsonObject readArguments(const QString& text, QString* error)
{
    if (text.trimmed().isEmpty()) return {};
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        // In English (the model reads it; Qt's own error text follows the interface language)
        *error = err.error == QJsonParseError::NoError
                     ? QString("the arguments are not a JSON object")
                     : QString("the arguments are no valid JSON (at character %1)").arg(err.offset);
        return {};
    }
    return doc.object();
}

QString compact(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }

// Ids the providers accept (Anthropic: letters, digits, _ and -)
QString safeId(const QString& id, int n)
{
    QString out;
    for (const QChar c : id)
        if (c.isLetterOrNumber() && c.unicode() < 128) out += c;
        else if (c == '_' || c == '-') out += c;
    return out.isEmpty() ? QString("call_%1").arg(n) : out;
}

} // namespace

const QVector<Preset>& presets()
{
    static const QVector<Preset> list{
        {"claude", N_("Claude (Anthropic)"), Protocol::Anthropic, "https://api.anthropic.com", "claude-sonnet-5-5",
         "ANTHROPIC_API_KEY", true, true},
        {"openai", N_("OpenAI"), Protocol::OpenAi, "https://api.openai.com/v1", "gpt-5", "OPENAI_API_KEY", true, true},
        {"openrouter", N_("OpenRouter"), Protocol::OpenAi, "https://openrouter.ai/api/v1", "openrouter/auto",
         "OPENROUTER_API_KEY", true, true},
        {"ollama", N_("Ollama (auf diesem Rechner)"), Protocol::OpenAi, "http://localhost:11434/v1", "qwen3", {}, false,
         true},
        {"custom", N_("Eigene Adresse (OpenAI-kompatibel)"), Protocol::OpenAi, {}, {}, {}, false, true},
    };
    return list;
}

const Preset* preset(const QString& id)
{
    for (const Preset& p : presets())
        if (p.id == id) return &p;
    return nullptr;
}

QString presetName(const QString& id)
{
    const Preset* p = preset(id);
    return p ? T(p->name) : id;
}

bool isLocal(const QString& address)
{
    const QString host = QUrl(trimmedAddress(address)).host().toLower();
    if (host == "localhost" || host.endsWith(".localhost")) return true;
    const QHostAddress ip(host);
    return !ip.isNull() && ip.isLoopback();
}

QUrl chatUrl(const Endpoint& e)
{
    const QString a = trimmedAddress(e.address);
    if (e.protocol == Protocol::Anthropic) return QUrl(a.endsWith("/v1") ? a + "/messages" : a + "/v1/messages");
    return QUrl(a + "/chat/completions");
}

QUrl modelsUrl(const Endpoint& e)
{
    const QString a = trimmedAddress(e.address);
    if (e.protocol == Protocol::Anthropic) return QUrl(a.endsWith("/v1") ? a + "/models?limit=1000" : a + "/v1/models?limit=1000");
    return QUrl(a + "/models");
}

QList<QPair<QByteArray, QByteArray>> headers(const Endpoint& e, bool fallbacks)
{
    QList<QPair<QByteArray, QByteArray>> h{{"content-type", "application/json"}};
    if (e.protocol == Protocol::Anthropic) {
        if (!e.key.isEmpty()) h.append({"x-api-key", e.key.toUtf8()});
        h.append({"anthropic-version", kAnthropicVersion});
        if (fallbacks) h.append({"anthropic-beta", kFallbackBeta});
        return h;
    }
    if (!e.key.isEmpty()) h.append({"authorization", "Bearer " + e.key.toUtf8()});
    if (e.provider == "openrouter") { // OpenRouter's app attribution (optional)
        h.append(QPair<QByteArray, QByteArray>("http-referer", "https://github.com/OnkelTheorie/schneidi"));
        h.append(QPair<QByteArray, QByteArray>("x-title", "schneidi"));
    }
    return h;
}

bool fallbacksFor(const Endpoint& e)
{
    static const QStringList models{"claude-sonnet-5-5", "claude-opus-5-5", "claude-opus-5", "claude-fable-5-1",
                                    "claude-fable-5"};
    return e.protocol == Protocol::Anthropic && QUrl(trimmedAddress(e.address)).host() == "api.anthropic.com"
           && models.contains(e.model.trimmed());
}

// ---------- Requests ----------

QJsonObject anthropicBody(const Request& r)
{
    QJsonArray tools;
    for (const QJsonValue& v : r.tools) {
        const QJsonObject t = v.toObject();
        tools << QJsonObject{{"name", t["name"]}, {"description", t["description"]}, {"input_schema", t["inputSchema"]}};
    }
    QJsonArray messages;
    for (const Message& m : r.messages) {
        if (m.role == Message::Role::Assistant) {
            QJsonArray content = m.anthropic;
            if (content.isEmpty()) { // from another provider earlier in the chat
                if (!m.text.isEmpty()) content << QJsonObject{{"type", "text"}, {"text", m.text}};
                for (const ToolCall& c : m.calls)
                    content << QJsonObject{{"type", "tool_use"}, {"id", c.id}, {"name", c.name}, {"input", c.input}};
                if (content.isEmpty()) content << QJsonObject{{"type", "text"}, {"text", "…"}};
            }
            messages << QJsonObject{{"role", "assistant"}, {"content", content}};
            continue;
        }
        QJsonArray content;
        for (const ToolResult& t : m.results) {
            QJsonArray parts;
            if (!t.text.isEmpty() || t.images.isEmpty()) parts << QJsonObject{{"type", "text"}, {"text", t.text.isEmpty() ? "(empty)" : t.text}};
            for (const Image& img : t.images)
                parts << QJsonObject{{"type", "image"},
                                     {"source", QJsonObject{{"type", "base64"}, {"media_type", img.mime},
                                                            {"data", QString::fromLatin1(img.base64)}}}};
            QJsonObject block{{"type", "tool_result"}, {"tool_use_id", t.id}, {"content", parts}};
            if (t.error) block["is_error"] = true;
            content << block;
        }
        if (!m.text.isEmpty()) content << QJsonObject{{"type", "text"}, {"text", m.text}};
        if (content.isEmpty()) continue;
        messages << QJsonObject{{"role", "user"}, {"content", content}};
    }
    QJsonObject o{{"model", r.model}, {"max_tokens", r.maxTokens}, {"messages", messages}};
    // Cache: the guide and the tools (system comes after the tools: one mark at its end covers both), and the
    // growing conversation by the automatic mark
    if (!r.system.isEmpty()) {
        QJsonObject system{{"type", "text"}, {"text", r.system}};
        if (r.autoCache) system["cache_control"] = QJsonObject{{"type", "ephemeral"}};
        o["system"] = QJsonArray{system};
    }
    if (!tools.isEmpty()) o["tools"] = tools;
    if (r.autoCache) o["cache_control"] = QJsonObject{{"type", "ephemeral"}};
    if (r.stream) o["stream"] = true;
    if (r.fallbacks) o["fallbacks"] = "default";
    return o;
}

QJsonObject openAiBody(const Request& r)
{
    QJsonArray messages;
    if (!r.system.isEmpty()) messages << QJsonObject{{"role", "system"}, {"content", r.system}};
    for (const Message& m : r.messages) {
        if (m.role == Message::Role::Assistant) {
            QJsonObject o{{"role", "assistant"}};
            o["content"] = m.text.isEmpty() && !m.calls.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(m.text);
            QJsonArray calls;
            for (const ToolCall& c : m.calls)
                calls << QJsonObject{{"id", c.id}, {"type", "function"},
                                     {"function", QJsonObject{{"name", c.name}, {"arguments", compact(c.input)}}}};
            if (!calls.isEmpty()) o["tool_calls"] = calls;
            messages << o;
            continue;
        }
        // Tool messages cannot carry pictures here: they follow as a user message right after the tool answers
        QJsonArray pictures;
        for (const ToolResult& t : m.results) {
            QString text = t.text.isEmpty() ? QString("(empty)") : t.text;
            if (t.error) text = "ERROR: " + text;
            if (!t.images.isEmpty())
                text += QString("\n(%1 image(s) of this result follow in the next message)").arg(t.images.size());
            messages << QJsonObject{{"role", "tool"}, {"tool_call_id", t.id}, {"content", text}};
            if (t.images.isEmpty()) continue;
            pictures << QJsonObject{{"type", "text"}, {"text", QString("Images from the %1 call %2:").arg(t.name, t.id)}};
            for (const Image& img : t.images)
                pictures << QJsonObject{{"type", "image_url"},
                                        {"image_url", QJsonObject{{"url", QString("data:%1;base64,%2")
                                                                              .arg(img.mime, QString::fromLatin1(img.base64))}}}};
        }
        if (!pictures.isEmpty()) messages << QJsonObject{{"role", "user"}, {"content", pictures}};
        if (!m.text.isEmpty()) messages << QJsonObject{{"role", "user"}, {"content", m.text}};
    }
    QJsonArray tools;
    for (const QJsonValue& v : r.tools) {
        const QJsonObject t = v.toObject();
        tools << QJsonObject{{"type", "function"},
                             {"function", QJsonObject{{"name", t["name"]}, {"description", t["description"]},
                                                      {"parameters", t["inputSchema"]}}}};
    }
    QJsonObject o{{"model", r.model}, {"messages", messages}};
    if (!tools.isEmpty()) o["tools"] = tools;
    if (r.stream) {
        o["stream"] = true;
        // Token counts at the end of the stream; only where it is known to work (strict servers refuse unknown fields)
        if (r.provider == "openai" || r.provider == "openrouter" || r.provider == "ollama")
            o["stream_options"] = QJsonObject{{"include_usage", true}};
    }
    return o;
}

QJsonObject body(Protocol p, const Request& r) { return p == Protocol::Anthropic ? anthropicBody(r) : openAiBody(r); }

// ---------- Streamed answers ----------

QString StreamParser::feed(const QByteArray& bytes)
{
    m_buffer += bytes;
    QString text;
    while (true) {
        const qsizetype nl = m_buffer.indexOf('\n');
        if (nl < 0) break;
        QByteArray line = m_buffer.left(nl);
        m_buffer.remove(0, nl + 1);
        if (line.endsWith('\r')) line.chop(1);
        if (line.isEmpty()) { // end of an event
            if (!m_data.isEmpty()) {
                if (m_data.trimmed() == "[DONE]") m_finished = true;
                else text += event(parseObject(m_data));
                m_data.clear();
            }
            continue;
        }
        if (!line.startsWith("data:")) continue; // "event:" repeats the type that is in the data; comments, ids
        QByteArray data = line.mid(5);
        if (data.startsWith(' ')) data.remove(0, 1);
        // Servers that leave out the blank line between events: a complete JSON object is an event of its own
        if (m_data.isEmpty() && data.trimmed() != "[DONE]") {
            const QJsonObject o = parseObject(data);
            if (!o.isEmpty()) {
                text += event(o);
                continue;
            }
        }
        if (!m_data.isEmpty()) m_data += '\n';
        m_data += data;
        if (m_data.trimmed() == "[DONE]") {
            m_finished = true;
            m_data.clear();
        }
    }
    return text;
}

QString StreamParser::event(const QJsonObject& o)
{
    if (o.isEmpty()) return {};
    // Errors look alike in both: {"type":"error","error":{…}} (Anthropic), {"error":{…}} (OpenAI; OpenRouter also next
    // to "choices" with finish_reason "error"); an "error": null of some servers is none
    const QJsonValue err = o["error"];
    if ((err.isObject() && !err.toObject().isEmpty()) || (err.isString() && !err.toString().isEmpty())
        || o["type"].toString() == "error") {
        const QJsonValue e = err;
        m_reply.error = e.isObject() ? e["message"].toString() : e.toString();
        if (m_reply.error.isEmpty()) m_reply.error = compact(o);
        m_reply.errorType = e.isObject() ? e["type"].toString() : QString();
        m_finished = true;
        return {};
    }
    return m_protocol == Protocol::Anthropic ? anthropicEvent(o) : openAiEvent(o);
}

QString StreamParser::anthropicEvent(const QJsonObject& o)
{
    const QString type = o["type"].toString();
    // Counts come in message_start and again (cumulative, maybe only some of them) in message_delta
    const auto usage = [this](const QJsonObject& u) {
        if (u.contains("input_tokens")) m_tokens[0] = u["input_tokens"].toInteger();
        if (u.contains("cache_read_input_tokens")) m_tokens[1] = u["cache_read_input_tokens"].toInteger();
        if (u.contains("cache_creation_input_tokens")) m_tokens[2] = u["cache_creation_input_tokens"].toInteger();
        if (u.contains("output_tokens")) m_reply.usage.output = u["output_tokens"].toInteger();
        m_reply.usage.input = m_tokens[0] + m_tokens[1] + m_tokens[2];
        m_reply.usage.cached = m_tokens[1];
    };
    if (type == "message_start") {
        usage(o["message"]["usage"].toObject());
        return {};
    }
    if (type == "content_block_start") {
        const int i = o["index"].toInt();
        if (i < 0 || i > 1000) return {};
        if (m_blocks.size() <= i) m_blocks.resize(i + 1);
        m_blocks[i] = o["content_block"].toObject();
        m_started = true;
        if (m_blocks[i]["type"].toString() == "tool_use") m_json[i].clear();
        const QString text = m_blocks[i]["type"].toString() == "text" ? m_blocks[i]["text"].toString() : QString();
        m_reply.text += text;
        return text;
    }
    if (type == "content_block_delta") {
        const int i = o["index"].toInt();
        if (i < 0 || i >= m_blocks.size()) return {};
        const QJsonObject d = o["delta"].toObject();
        const QString dt = d["type"].toString();
        QJsonObject& b = m_blocks[i];
        if (dt == "text_delta") {
            const QString t = d["text"].toString();
            b["text"] = b["text"].toString() + t;
            m_reply.text += t;
            return t;
        }
        if (dt == "input_json_delta") m_json[i] += d["partial_json"].toString();
        else if (dt == "thinking_delta") b["thinking"] = b["thinking"].toString() + d["thinking"].toString();
        else if (dt == "signature_delta") b["signature"] = b["signature"].toString() + d["signature"].toString();
        return {};
    }
    if (type == "message_delta") {
        if (const QString stop = o["delta"]["stop_reason"].toString(); !stop.isEmpty()) m_reply.stop = stop;
        usage(o["usage"].toObject());
        return {};
    }
    if (type == "message_stop") m_finished = true;
    return {}; // ping, content_block_stop, unknown events
}

QString StreamParser::openAiEvent(const QJsonObject& o)
{
    if (const QJsonObject u = o["usage"].toObject(); !u.isEmpty()) {
        m_reply.usage.input = u["prompt_tokens"].toInteger();
        m_reply.usage.output = u["completion_tokens"].toInteger();
        m_reply.usage.cached = u["prompt_tokens_details"]["cached_tokens"].toInteger();
    }
    const QJsonArray choices = o["choices"].toArray();
    if (choices.isEmpty()) return {};
    const QJsonObject c = choices.first().toObject();
    if (const QString f = c["finish_reason"].toString(); !f.isEmpty()) m_reply.stop = f;
    if (m_reply.stop == "error" && m_reply.error.isEmpty()) { // OpenRouter: the model failed mid-answer
        m_reply.error = "the provider ended the answer with an error";
        m_finished = true;
    }
    const QJsonObject d = c["delta"].toObject();
    const QJsonArray calls = d["tool_calls"].toArray();
    for (int k = 0; k < calls.size(); ++k) {
        const QJsonObject tc = calls[k].toObject();
        const QString id = tc["id"].toString();
        // Which call: by index; servers without one (Ollama sends whole calls) by id, else in order
        int at = -1;
        if (tc.contains("index")) at = tc["index"].toInt();
        else if (!id.isEmpty())
            for (int j = 0; j < m_calls.size(); ++j)
                if (m_calls[j].id == id) at = j;
        if (at < 0) at = int(m_calls.size());
        if (at > 1000) continue;
        if (m_calls.size() <= at) m_calls.resize(at + 1);
        PartialCall& p = m_calls[at];
        if (!id.isEmpty()) p.id = id;
        const QJsonObject fn = tc["function"].toObject();
        if (const QString n = fn["name"].toString(); !n.isEmpty()) p.name = n;
        const QJsonValue args = fn["arguments"];
        p.arguments += args.isObject() ? compact(args.toObject()) : args.toString();
        m_started = true;
    }
    const QString text = d["content"].toString();
    if (!text.isEmpty()) {
        m_reply.text += text;
        m_started = true;
    }
    return text;
}

Reply StreamParser::reply() const
{
    Reply r = m_reply;
    if (m_protocol == Protocol::Anthropic) {
        // A server-side fallback in the middle of the answer: the thinking and tool calls before the last "fallback"
        // block belong to the model that gave up – neither run nor sent back
        int fallback = -1;
        for (int i = 0; i < m_blocks.size(); ++i)
            if (m_blocks[i]["type"].toString() == "fallback") fallback = i;
        static const QStringList dropped{"thinking", "redacted_thinking", "tool_use"};
        QJsonArray content;
        for (int i = 0; i < m_blocks.size(); ++i) {
            QJsonObject b = m_blocks[i];
            if (b.isEmpty() || (i < fallback && dropped.contains(b["type"].toString()))) continue;
            if (b["type"].toString() == "tool_use") {
                ToolCall call{b["id"].toString(), b["name"].toString(), {}, {}};
                call.input = m_json.contains(i) ? readArguments(m_json.value(i), &call.error) : b["input"].toObject();
                b["input"] = call.input;
                r.calls << call;
            }
            content << b;
        }
        r.anthropic = content;
        return r;
    }
    for (int i = 0; i < m_calls.size(); ++i) {
        const PartialCall& p = m_calls[i];
        if (p.name.isEmpty()) continue;
        ToolCall call{safeId(p.id, i), p.name, {}, {}};
        call.input = readArguments(p.arguments, &call.error);
        r.calls << call;
    }
    return r;
}

Reply parseBody(Protocol p, const QJsonObject& body)
{
    Reply r;
    if (body.contains("error")) {
        const QJsonValue e = body["error"];
        r.error = e.isObject() ? e["message"].toString() : e.toString();
        return r;
    }
    if (p == Protocol::Anthropic) {
        r.anthropic = body["content"].toArray();
        for (const QJsonValue& v : r.anthropic) {
            const QJsonObject b = v.toObject();
            if (b["type"].toString() == "text") r.text += b["text"].toString();
            if (b["type"].toString() == "tool_use") r.calls << ToolCall{b["id"].toString(), b["name"].toString(), b["input"].toObject(), {}};
        }
        r.stop = body["stop_reason"].toString();
        const QJsonObject u = body["usage"].toObject();
        r.usage.cached = u["cache_read_input_tokens"].toInteger();
        r.usage.input = u["input_tokens"].toInteger() + r.usage.cached + u["cache_creation_input_tokens"].toInteger();
        r.usage.output = u["output_tokens"].toInteger();
        return r;
    }
    const QJsonArray choices = body["choices"].toArray();
    const QJsonObject c = choices.isEmpty() ? QJsonObject() : choices.first().toObject();
    const QJsonObject m = c["message"].toObject();
    r.text = m["content"].toString();
    r.stop = c["finish_reason"].toString();
    const QJsonArray calls = m["tool_calls"].toArray();
    for (int i = 0; i < calls.size(); ++i) {
        const QJsonObject fn = calls[i]["function"].toObject();
        ToolCall call{safeId(calls[i]["id"].toString(), i), fn["name"].toString(), {}, {}};
        call.input = fn["arguments"].isObject() ? fn["arguments"].toObject() : readArguments(fn["arguments"].toString(), &call.error);
        r.calls << call;
    }
    const QJsonObject u = body["usage"].toObject();
    r.usage.input = u["prompt_tokens"].toInteger();
    r.usage.output = u["completion_tokens"].toInteger();
    r.usage.cached = u["prompt_tokens_details"]["cached_tokens"].toInteger();
    return r;
}

// ---------- Errors ----------

QString providerMessage(const QByteArray& body)
{
    const QJsonObject o = parseObject(body);
    const QJsonValue e = o["error"];
    QString m = e.isObject() ? e["message"].toString() : e.toString();
    if (m.isEmpty()) m = o["message"].toString(); // some proxies
    if (m.isEmpty() && o.isEmpty()) m = QString::fromUtf8(body.left(300)).simplified(); // HTML of a proxy
    return m.trimmed();
}

bool contextOverflow(const QString& said, qint64* context, qint64* request)
{
    *context = *request = 0;
    const auto match = [&said](const char* pattern) {
        return QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption
                                               | QRegularExpression::DotMatchesEverythingOption)
            .match(said);
    };
    // llama.cpp: "request (15768 tokens) exceeds the available context size (4096 tokens), try increasing it"
    if (const auto m = match(R"(request \((\d+) tokens\) exceeds the available context size \((\d+) tokens\))");
        m.hasMatch()) {
        *request = m.captured(1).toLongLong();
        *context = m.captured(2).toLongLong();
        return true;
    }
    // OpenAI, vLLM: "This model's maximum context length is 8192 tokens. However, your messages resulted in 15768
    // tokens"
    if (const auto m = match(R"(maximum context length is (\d+) tokens.*?(?:resulted in|requested) (\d+) tokens)");
        m.hasMatch()) {
        *context = m.captured(1).toLongLong();
        *request = m.captured(2).toLongLong();
        return true;
    }
    // LM Studio: "… the model is loaded with context length of only 4096 tokens, which is not enough"
    if (const auto m = match(R"(context length of only (\d+) tokens)"); m.hasMatch()) {
        *context = m.captured(1).toLongLong();
        return true;
    }
    return match(R"(context_length_exceeded|exceeds? the (?:available )?context (?:size|length|window))").hasMatch();
}

QString contextAdvice(qint64 context, qint64 request, bool silent)
{
    QString what;
    if (silent)
        what = T("Der KI-Server hat die Anfrage gekürzt, ohne es zu melden: das Modell nimmt %1 Token, die Anfrage hat "
                 "etwa %2.")
                   .arg(context)
                   .arg(request);
    else if (context > 0 && request > 0)
        what = T("Die Anfrage passt nicht in den Kontext des Modells: es nimmt %1 Token, die Anfrage hat %2.")
                   .arg(context)
                   .arg(request);
    else what = T("Die Anfrage passt nicht in den Kontext des Modells.");
    // A large context is full: the chat grew too long; a small one cannot even take the tools
    if (context >= 32768) return what + " " + T("„Neuer Chat“ fängt frisch an.");
    return what + " "
           + T("In den Einstellungen des KI-Servers die Kontextlänge auf mindestens 32 768 Token stellen – schneidis "
               "Werkzeuge und Anleitung allein brauchen etwa 15 000 (Ollama: OLLAMA_CONTEXT_LENGTH, LM Studio, AtomicChat "
               "und andere: Kontextlänge des Modells).");
}

Failure httpFailure(int status, const QByteArray& body, const QByteArray& retryAfter)
{
    Failure f;
    const QString said = providerMessage(body);
    const QString type = parseObject(body).value("error").toObject().value("type").toString();
    const auto with = [&said](const QString& text) { return said.isEmpty() ? text : T("%1 (%2)").arg(text, said); };
    f.retryAfterS = std::clamp(retryAfter.trimmed().toInt(), 0, 600);
    qint64 context = 0, request = 0;
    if ((status == 400 || status == 413 || status >= 500) && contextOverflow(said, &context, &request)) {
        f.kind = Failure::Kind::TooLarge;
        f.message = contextAdvice(context, request);
    } else if (status == 401 || status == 403) {
        f.kind = Failure::Kind::Key;
        f.message = with(status == 401 ? T("Der Schlüssel wurde abgelehnt. Prüfe ihn in Einstellungen › KI.")
                                       : T("Der Anbieter verweigert den Zugriff mit diesem Schlüssel."));
    } else if (status == 429) {
        f.kind = Failure::Kind::RateLimit;
        f.retry = true;
        f.message = with(T("Zu viele Anfragen (Limit des Anbieters erreicht)."));
    } else if (status == 529 || type == "overloaded_error") {
        f.kind = Failure::Kind::Overloaded;
        f.retry = true;
        f.message = with(T("Der Anbieter ist gerade überlastet."));
    } else if (status == 404) {
        f.kind = Failure::Kind::NotFound;
        f.message = with(T("Modell oder Adresse nicht gefunden. Prüfe Modell und Adresse in Einstellungen › KI."));
    } else if (status == 413) {
        f.kind = Failure::Kind::TooLarge;
        f.message = with(T("Die Anfrage ist zu groß (sehr langer Chat oder viele Bilder). Starte einen neuen Chat."));
    } else if (status == 402) {
        f.kind = Failure::Kind::Other;
        f.message = with(T("Der Anbieter meldet ein Problem mit der Abrechnung oder dem Guthaben."));
    } else if (status >= 500) {
        f.kind = Failure::Kind::Server;
        f.retry = true;
        f.message = with(T("Fehler beim Anbieter (HTTP %1).").arg(status));
    } else if (status >= 400) {
        f.kind = Failure::Kind::Request;
        f.message = with(T("Der Anbieter hat die Anfrage abgelehnt (HTTP %1).").arg(status));
    } else {
        f.message = with(T("Unerwartete Antwort (HTTP %1).").arg(status));
    }
    return f;
}

QVector<ToolCall> textCalls(const QString& text, const QString& idPrefix)
{
    // One written call: exactly a name and its arguments (an id or "type" may come along), or OpenAI's own shape
    // {"type":"function","function":{…}}; anything else in the object = not a call
    const auto call = [](QJsonObject o, ToolCall* out) {
        if (o.value("function").isObject() && o.size() <= 3) o = o.value("function").toObject();
        const QString args = o.contains("arguments") ? "arguments" : "parameters";
        static const QStringList known{"name", "arguments", "parameters", "id", "type"};
        const QString name = o.value("name").toString().trimmed();
        if (name.isEmpty() || !o.contains(args)) return false;
        for (const QString& k : o.keys())
            if (!known.contains(k)) return false;
        out->name = name;
        const QJsonValue a = o.value(args);
        if (a.isObject()) out->input = a.toObject();
        else if (a.isString()) out->input = readArguments(a.toString(), &out->error);
        else if (!a.isNull()) out->error = "the arguments are not a JSON object";
        return true;
    };
    // A block of JSON: one object, a list of them, or several one after the other (also over several lines each)
    const auto block = [&call](const QString& json, QVector<ToolCall>* out) {
        QJsonArray objects;
        QByteArray rest = json.toUtf8().trimmed();
        while (!rest.isEmpty()) {
            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(rest, &err);
            qsizetype used = rest.size();
            if (err.error == QJsonParseError::GarbageAtEnd) { // the next value follows: this one ends there
                used = err.offset;
                doc = QJsonDocument::fromJson(rest.left(used), &err);
            }
            if (err.error != QJsonParseError::NoError || used <= 0) return false;
            if (doc.isArray())
                for (const QJsonValue& v : doc.array()) objects << v;
            else objects << doc.object();
            rest = rest.mid(used).trimmed();
        }
        if (objects.isEmpty()) return false;
        for (const QJsonValue& v : objects) {
            ToolCall c;
            if (!v.isObject() || !call(v.toObject(), &c)) return false;
            *out << c;
        }
        return true;
    };
    QVector<ToolCall> calls;
    // Chat template markers that leaked into the text (seen: "<|im_start|>" before a call)
    static const QRegularExpression marker("<\\|[a-z_]+\\|>");
    QString t = QString(text).remove(marker).trimmed();
    // The whole answer in one other tag (seen: a call inside <tool_response>)
    static const QRegularExpression wrapped(R"(^<([a-z_]+)>(.*)</\1>$)", QRegularExpression::DotMatchesEverythingOption);
    if (const auto m = wrapped.match(t); m.hasMatch() && m.captured(1) != "tool_call") t = m.captured(2).trimmed();
    if (t.contains("<tool_call>")) {
        // Hermes/Qwen style: every tag is a call (the closing tag may be missing at the end)
        for (QString part : t.split("<tool_call>").mid(1)) {
            part = part.section("</tool_call>", 0, 0).trimmed();
            if (part.startsWith("```")) part = part.section('\n', 1).section("```", 0, 0);
            if (!block(part.trimmed(), &calls)) return {};
        }
    } else {
        // A ```json fence, also only half of one (seen: a call followed by a lone closing fence)
        QString json = t;
        if (json.startsWith("```")) json = json.section('\n', 1); // the fence line may name the language
        if (json.endsWith("```")) json.chop(3);
        if (!json.trimmed().startsWith('{') && !json.trimmed().startsWith('[')) return {};
        if (!block(json.trimmed(), &calls)) return {};
    }
    for (int i = 0; i < calls.size(); ++i) calls[i].id = QString("%1_%2").arg(idPrefix).arg(i + 1);
    return calls;
}

QString textAnswer(const QString& text)
{
    QString t = text.trimmed();
    if (t.startsWith("```")) {
        t = t.section('\n', 1); // the fence line may name the language
        if (t.trimmed().endsWith("```")) t = t.trimmed().chopped(3);
        t = t.trimmed();
    }
    if (!t.startsWith('{')) return text;
    const QJsonObject o = parseObject(t.toUtf8());
    static const QStringList keys{"text", "answer", "response", "reply", "message", "content", "final_answer"};
    QString found;
    for (auto it = o.begin(); it != o.end(); ++it) {
        if (it.key() == "type" || it.key() == "role") continue;
        if (!keys.contains(it.key()) || !it.value().isString() || !found.isEmpty()) return text; // anything else: as it is
        found = it.value().toString();
    }
    return found.trimmed().isEmpty() ? text : found;
}

QStringList parseModels(const QJsonObject& body)
{
    QStringList ids;
    for (const QJsonValue& v : body["data"].toArray()) // Anthropic, OpenAI, OpenRouter, Ollama's /v1/models
        if (const QString id = v["id"].toString(); !id.isEmpty()) ids << id;
    for (const QJsonValue& v : body["models"].toArray()) // Ollama's own /api/tags
        if (const QString id = v["name"].toString(); !id.isEmpty()) ids << id;
    ids.removeDuplicates();
    std::sort(ids.begin(), ids.end(), [](const QString& a, const QString& b) { return I18n::compareNames(a, b) < 0; });
    return ids;
}

} // namespace Ai
