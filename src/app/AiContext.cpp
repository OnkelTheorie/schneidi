#include "app/AiContext.h"

#include <QJsonDocument>

#include <algorithm>

namespace Ai {

namespace {

QString trimmedAddress(const QString& a)
{
    QString s = a.trimmed();
    while (s.endsWith('/')) s.chop(1);
    return s;
}

// The server itself: the address without its OpenAI path ("http://localhost:11434/v1" -> "http://localhost:11434")
QString serverRoot(const Endpoint& e)
{
    QString a = trimmedAddress(e.address);
    if (a.endsWith("/v1")) a.chop(3);
    return a;
}

// "qwen3" and "qwen3:latest" are one model to Ollama
bool sameModel(const QString& a, const QString& b)
{
    const auto plain = [](QString s) {
        s = s.trimmed();
        if (s.endsWith(":latest")) s.chop(7);
        return s;
    };
    return !a.trimmed().isEmpty() && plain(a) == plain(b);
}

// Where a text may end early: after a clause outside brackets and quotes, not before `min` characters; strong ends
// (": ", "; ") before weak ones (", ", " (")
int clauseEnd(const QString& s, int min, int max)
{
    int strong = -1, weak = -1, depth = 0;
    bool quoted = false;
    for (int i = 0; i < std::min<int>(int(s.size()), max); ++i) {
        const QChar c = s[i];
        if (c == '"') quoted = !quoted;
        if (quoted) continue;
        if (c == '(' || c == '[' || c == '{') {
            if (depth == 0 && i >= min && i > 0 && s[i - 1] == ' ') weak = i - 1;
            ++depth;
        } else if (c == ')' || c == ']' || c == '}') {
            depth = std::max(0, depth - 1);
        } else if (depth == 0 && i >= min && i + 1 < s.size() && s[i + 1] == ' ') {
            if (c == ':' || c == ';') strong = i;
            else if (c == ',') weak = i;
        }
    }
    return strong >= 0 ? strong : weak;
}

QString shortened(const QString& s, int min, int max)
{
    if (s.size() <= max) return s;
    if (const int end = clauseEnd(s, min, max); end > 0) return s.left(end);
    QString cut = s.left(max);
    if (const qsizetype space = cut.lastIndexOf(' '); space > min) cut = cut.left(space);
    return cut + " …";
}

// A parameter's text in short: its first clause, at most ~60 characters
QString shortParam(const QString& text)
{
    QString s = text.simplified();
    if (const int end = clauseEnd(s, 12, int(s.size())); end > 0) s = s.left(end);
    return shortened(s, 12, 60);
}

} // namespace

bool useCompact(Compact mode, const Endpoint& e, qint64 context)
{
    if (mode != Compact::Auto) return mode == Compact::On;
    if (e.protocol == Protocol::Anthropic) return false;
    if (context > 0 && context < kCompactBelow) return true;
    return isLocal(e.address);
}

// Tools an editing chat in the app seldom needs: their parameters come from `help` when they are used
const QStringList kRareTools{"new", "templates", "backups", "restore", "export_timeline", "watch", "sync", "extensions"};

QString firstSentence(const QString& text, int max)
{
    const QString s = text.simplified();
    QString first = s;
    // ". " before a capital letter, a quote or a code name; "e.g." and "i.e." end nothing
    for (int i = 0; i + 2 < s.size(); ++i) {
        if (s[i] != '.' || s[i + 1] != ' ') continue;
        const QString before = s.left(i + 1);
        if (before.endsWith("e.g.") || before.endsWith("i.e.") || before.endsWith("etc.") || before.endsWith(" vs."))
            continue;
        const QChar next = s[i + 2];
        if (next.isUpper() || next == '`' || next == '\'' || next == '"' || next == '(') {
            first = s.left(i + 1);
            break;
        }
    }
    if (first.size() <= max) return first;
    QString cut = shortened(first, 24, max);
    if (cut.endsWith(" …")) return cut;
    return cut + ".";
}

QJsonArray compactTools(const QJsonArray& tools, const QJsonArray& editOps)
{
    QJsonArray out;
    for (const QJsonValue& v : tools) {
        QJsonObject t = v.toObject();
        const QString name = t["name"].toString();
        QJsonObject schema = t["inputSchema"].toObject();
        QJsonObject props = schema["properties"].toObject();
        const bool rare = kRareTools.contains(name);
        QString description = firstSentence(t["description"].toString());
        if (name == "edit" && editOps.isEmpty()) { // without its ops the model cannot edit: as it is
            out << t;
            continue;
        }
        for (auto it = props.begin(); it != props.end(); ++it) {
            QJsonObject p = it.value().toObject();
            if (rare || p["description"].toString().isEmpty()) p.remove("description");
            else p["description"] = shortParam(p["description"].toString());
            it.value() = p;
        }
        if (rare) description += QString(" Parameters in detail: `help` with command '%1'.").arg(name);
        if (name == "help")
            description = "The details of a tool: command '<tool>' (all its parameters); of edit ops: command 'edit' "
                          "and op '<name>' (or a comma list).";
        if (name == "edit") {
            QStringList ops;
            for (const QJsonValue& op : editOps)
                if (const QString usage = op["usage"].toString(); !usage.isEmpty()) ops << usage;
            QJsonObject p = props["ops"].toObject();
            p["description"] = "List of operations, applied in order (nothing changes if one fails). Times: frames, "
                               "\"4.5s\" or \"HH:MM:SS:FF\"; ranges [from, to). Tracks: \"V1\", \"A2\". Clip ids "
                               "come from `info` or the answer of the edit before (they change when clips are split). "
                               "An op's parameters in detail: `help` with command 'edit' and op '<name>'. Ops: "
                               + ops.join("; ");
            props["ops"] = p;
        }
        schema["properties"] = props;
        t["description"] = description;
        t["inputSchema"] = schema;
        out << t;
    }
    return out;
}

QString compactGuide(const QString& topics)
{
    QString s =
        "# Editing with schneidi (the main points of the guide)\n"
        "1. Look at the material first: `info` (the project: media, clips with ids), `probe` and `overview` (a file), "
        "then what the task needs: `frames` (see it), `transcribe` (what is said), `silence` (pauses), `scenes` "
        "(cuts), `peaks`/`highlights` (loud or exciting moments), `beats` (a song).\n"
        "2. Say the plan in one or two sentences, then do it; ask only when the answer changes the result a lot.\n"
        "3. Edit in small steps with `edit`. Take clip ids from `info` or the answer of the last `edit`, never guess "
        "them. Unsure how an op or a tool works: ask `help` (command 'edit' and op '<name>', or command '<tool>') "
        "instead of guessing; `effects` lists effects and transitions.\n"
        "4. Check with `check` and `info`, look with `frames`; after `render`, check the rendered file too.\n"
        "5. Times in seconds (\"4.5s\") are the safest. Never overwrite the user's files: renders get new names. Do "
        "what was asked, the way it was asked, with good defaults (-14 LUFS for the web, H.264 MP4). Downloads "
        "(`extensions`, e.g. whisper for `transcribe`) need the user's yes.";
    if (!topics.trimmed().isEmpty())
        s += "\n\nTask guides (call `guide` with the topic before such a task):\n" + topics.trimmed();
    return s;
}

QVector<ContextProbe> contextProbes(const Endpoint& e)
{
    // Cloud providers have large contexts (and other ways to say them)
    if (e.protocol != Protocol::OpenAi || e.provider == "openai" || e.provider == "openrouter") return {};
    if (trimmedAddress(e.address).isEmpty() || e.model.trimmed().isEmpty()) return {};
    const QString root = serverRoot(e);
    const QByteArray show = QJsonDocument(QJsonObject{{"model", e.model.trimmed()}}).toJson(QJsonDocument::Compact);
    QVector<ContextProbe> probes{{"ollama-show", "POST", QUrl(root + "/api/show"), show},
                                 {"ollama-ps", "GET", QUrl(root + "/api/ps"), {}}};
    if (e.provider == "ollama") return probes;
    probes << ContextProbe{"llama.cpp", "GET", QUrl(root + "/props"), {}}
           << ContextProbe{"lmstudio", "GET", QUrl(root + "/api/v0/models"), {}}
           << ContextProbe{"models", "GET", QUrl(trimmedAddress(e.address) + "/models"), {}};
    return probes;
}

qint64 contextOf(const QString& kind, const QJsonObject& answer, const QString& model)
{
    if (kind == "ollama-show") {
        // "parameters" is text, one "name value" per line: "num_ctx                        16384"
        for (const QString& line : answer["parameters"].toString().split('\n')) {
            const QStringList parts = line.simplified().split(' ');
            if (parts.size() == 2 && parts[0] == "num_ctx") return std::max<qint64>(0, parts[1].toLongLong());
        }
        return 0;
    }
    if (kind == "ollama-ps") {
        for (const QJsonValue& m : answer["models"].toArray())
            if (sameModel(m["name"].toString(), model) || sameModel(m["model"].toString(), model))
                return std::max<qint64>(0, m["context_length"].toInteger());
        return 0;
    }
    if (kind == "llama.cpp") { // the context of one slot (the whole one divided by the parallel slots)
        const qint64 slot = answer["default_generation_settings"]["n_ctx"].toInteger();
        return std::max<qint64>(0, slot > 0 ? slot : answer["n_ctx"].toInteger());
    }
    if (kind == "lmstudio" || kind == "models") {
        for (const QJsonValue& m : answer["data"].toArray()) {
            if (!sameModel(m["id"].toString(), model)) continue;
            return std::max<qint64>(0, m[kind == "lmstudio" ? "loaded_context_length" : "max_model_len"].toInteger());
        }
    }
    return 0;
}

qint64 contextFrom(const QHash<QString, qint64>& found)
{
    for (const char* kind : {"ollama-show", "ollama-ps", "llama.cpp", "lmstudio", "models"})
        if (const qint64 n = found.value(kind); n > 0) return n;
    return 0;
}

qint64 recommendedContext(qint64 fixed)
{
    qint64 n = 8192;
    while (n < fixed + kChatRoom) n *= 2;
    return n;
}

} // namespace Ai
