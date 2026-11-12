// SPDX-License-Identifier: AGPL-3.0-only
#include "app/AiContext.h"

#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>

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
const QStringList kRareTools{"new",  "templates", "backups", "restore", "export_timeline", "watch", "sync",
                             "extensions", "scopes", "brand"};

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

namespace {

// A time parameter takes frames or a text ("4.5s"): in short only the text (seconds are the safest, the guide says)
void textTimes(QJsonObject& p)
{
    const QJsonArray both{"integer", "string"};
    if (p.value("type").toArray() == both) p["type"] = "string";
    if (QJsonObject items = p.value("items").toObject(); items.value("type").toArray() == both) {
        items["type"] = "string";
        p["items"] = items;
    }
}

} // namespace

// An op signature in short: the values and defaults of optional keys left out ("mode?:'peak'|'lufs'" -> "mode?"),
// `help` with the op has them; written as a call: "{op:'split', at, clip?}" -> "split(at, clip?)", "{op:'enable'|
// 'disable', clips}" -> "enable|disable(clips)" (the ops' text says how to read it)
QString shortUsage(const QString& usage)
{
    static const QRegularExpression values(R"(\?:(\[[^\]]*\]|'[^']*'(\|'[^']*')*|[A-Za-z0-9_.]+))");
    static const QRegularExpression head(R"(\{op:'([a-z_]+)'((?:\|'[a-z_]+')*)(, )?)");
    const QString s = QString(usage).replace(values, "?");
    QString out;
    qsizetype i = 0;
    while (i < s.size()) {
        const QRegularExpressionMatch m = head.match(s, i, QRegularExpression::NormalMatch,
                                                     QRegularExpression::AnchorAtOffsetMatchOption);
        if (!m.hasMatch()) {
            out += s[i++];
            continue;
        }
        // The closing brace of this signature (brackets inside it nest)
        qsizetype j = m.capturedEnd();
        int depth = 0;
        for (; j < s.size(); ++j) {
            const QChar c = s[j];
            if (c == '{' || c == '[' || c == '(') ++depth;
            else if (c == ']' || c == ')') --depth;
            else if (c == '}' && depth-- == 0) break;
        }
        if (j >= s.size()) return s; // not one we know how to read: as it is
        out += m.captured(1) + QString(m.captured(2)).remove('\'') + '(' + s.mid(m.capturedEnd(), j - m.capturedEnd()) + ')';
        i = j + 1;
    }
    return out;
}

namespace {

// The keys of a call's argument list at the top level ("a, b:[x, y], c?" -> "a", "b:[x, y]", "c?")
QStringList topLevelKeys(const QString& args)
{
    QStringList keys;
    int depth = 0;
    bool quoted = false;
    qsizetype from = 0;
    for (qsizetype i = 0; i < args.size(); ++i) {
        const QChar c = args[i];
        if (c == '\'') quoted = !quoted;
        if (quoted) continue;
        if (c == '{' || c == '[' || c == '(') ++depth;
        else if (c == '}' || c == ']' || c == ')') depth = std::max(0, depth - 1);
        else if (c == ',' && depth == 0) {
            keys << args.mid(from, i - from).trimmed();
            from = i + 1;
        }
    }
    if (const QString last = args.mid(from).trimmed(); !last.isEmpty()) keys << last;
    return keys;
}

// A key the op can do without: "track?", "out?|duration?", "<style>" (a group of keys)
bool optionalKey(const QString& key)
{
    const QString name = key.section(':', 0, 0);
    return name.contains('?') || name.startsWith('<');
}

} // namespace

QString cappedUsage(const QString& usage, int max)
{
    if (usage.size() <= max) return usage;
    // The first form of the call only ("a(x) or a(y)": the others come from `help`)
    const qsizetype open = usage.indexOf('(');
    if (open <= 0) return usage;
    int depth = 0;
    qsizetype close = -1;
    for (qsizetype i = open; i < usage.size() && close < 0; ++i) {
        if (usage[i] == '(' || usage[i] == '[' || usage[i] == '{') ++depth;
        else if ((usage[i] == ')' || usage[i] == ']' || usage[i] == '}') && --depth == 0) close = i;
    }
    if (close < 0) return usage;
    const QString name = usage.left(open);
    const QStringList keys = topLevelKeys(usage.mid(open + 1, close - open - 1));
    // Every required key stays; optional ones in their order (the first are the main ones) while the call fits
    qsizetype length = name.size() + 2;
    for (const QString& k : keys)
        if (!optionalKey(k)) length += k.size() + 2;
    QStringList out;
    bool full = false, dropped = close + 1 < usage.size(); // another form after this one
    for (const QString& k : keys) {
        if (optionalKey(k)) {
            if (!full && length + k.size() + 2 + 3 <= max) {
                length += k.size() + 2;
            } else {
                full = dropped = true;
                continue;
            }
        }
        out << k;
    }
    if (dropped) out << "…";
    return name + '(' + out.join(", ") + ')';
}

// The ops of common edits: always with every key
const QStringList kCoreOps{"append", "place",    "keep",       "insert", "split",    "trim",     "move",
                           "delete", "delete_range", "clear",  "fade",   "transition", "title",  "subtitle",
                           "captions", "speak",  "voiceover",  "volume", "speed",    "effect",   "timeline",
                           "add_track"};

// Ops an editing chat seldom needs (timeline and media pool housekeeping, multicam repairs, templates, node details):
// by name only
const QStringList kRareOps{"slot", "fill", "comp_from", "track_node", "cutout_node", "pick_region", "track_window", "multicam_nudge",
                           "add_angle", "angle_name", "source_marker", "bin", "remove_media", "compound", "decompose",
                           "place_timeline", "rename_timeline", "remove_timeline", "duplicate_timeline",
                           "add_subtitle_track", "copy_attributes", "flatten", "marks", "link"};

qsizetype paramTextFor(const QString& tool)
{
    static const QHash<QString, qsizetype> more{{"render", 540}};
    return more.value(tool, kParamText);
}

QString compactOps(const QJsonArray& editOps)
{
    QStringList ops, names;
    for (const QJsonValue& op : editOps) {
        const QString usage = op["usage"].toString();
        if (usage.isEmpty()) continue;
        const QString name = op["op"].toString();
        if (kRareOps.contains(name)) names << name;
        else if (kCoreOps.contains(name)) ops << shortUsage(usage);
        else ops << cappedUsage(shortUsage(usage), kOpChars);
    }
    QString s = ops.join("; ");
    if (!names.isEmpty()) s += ". Seldom needed (call `help` for their keys first): " + names.join(", ");
    return s;
}

QString compactInstructions(const QString& instructions)
{
    // The app (the only one that asks for the short request) says how its project works live in its own part
    static const QString live = " If the project is open in the schneidi app, all tools work live";
    if (const qsizetype from = instructions.indexOf(live); from >= 0) {
        static const QString end = "the project open in the running app.";
        if (const qsizetype to = instructions.indexOf(end, from); to > from)
            return compactInstructions(instructions.left(from) + instructions.mid(to + end.size()));
    }
    // The list of what each tool is for: the tool descriptions say it as well
    static const QString start = "Edit projects (.schneidi) with these tools:";
    const qsizetype from = instructions.indexOf(start);
    if (from < 0) return instructions;
    int depth = 0;
    for (qsizetype i = from + start.size(); i < instructions.size(); ++i) {
        const QChar c = instructions[i];
        if (c == '(') ++depth;
        else if (c == ')') depth = std::max(0, depth - 1);
        else if (c == '.' && depth == 0 && (i + 1 == instructions.size() || instructions[i + 1] == ' '))
            return instructions.left(from) + "Edit projects (.schneidi) with the tools (each one's description says "
                                              "what it is for)." + instructions.mid(i + 1);
    }
    return instructions;
}

// Only for a request the user asked for in words (trusting someone else's code): never needed by a small model
const QStringList kRareParams{QStringLiteral("trust_custom_code")};

QJsonArray compactTools(const QJsonArray& tools, const QJsonArray& editOps)
{
    // A parameter text several tools share word for word ("project": "project file") says little: left out
    QHash<QString, int> shared;
    for (const QJsonValue& v : tools) {
        const QJsonObject props = v["inputSchema"]["properties"].toObject();
        for (auto it = props.begin(); it != props.end(); ++it)
            if (const QString d = it.value().toObject()["description"].toString(); !d.isEmpty())
                ++shared[it.key() + '\n' + shortParam(d)];
    }
    QJsonArray out;
    for (const QJsonValue& v : tools) {
        QJsonObject t = v.toObject();
        const QString name = t["name"].toString();
        QJsonObject schema = t["inputSchema"].toObject();
        QJsonObject props = schema["properties"].toObject();
        const bool rare = kRareTools.contains(name);
        QString description = firstSentence(t["description"].toString(), kToolText);
        if (name == "edit" && editOps.isEmpty()) { // without its ops the model cannot edit: as it is
            out << t;
            continue;
        }
        // Parameters a small model rarely needs are left out entirely (`help` still lists them)
        for (const QString& r : kRareParams) props.remove(r);
        // help's "all" (every command with every parameter, ~80 KB): the tool list already has them. The MCP server
        // leaves it out too; this covers an older one
        if (name == "help") props.remove("all");
        for (auto it = props.begin(); it != props.end(); ++it) {
            QJsonObject p = it.value().toObject();
            const QString text = shortParam(p["description"].toString());
            if (rare || text.isEmpty() || shared.value(it.key() + '\n' + text) >= kSharedParam) p.remove("description");
            else p["description"] = text;
            textTimes(p);
            it.value() = p;
        }
        // At most kParamText (paramTextFor) characters of parameter texts per tool: the longest of the optional ones go first (the
        // special cases, as a rule; `help` has them), so a new parameter costs its name and type, not more
        if (!rare) {
            const QJsonArray required = schema.value("required").toArray();
            QVector<std::pair<qsizetype, QString>> texts;
            qsizetype sum = 0;
            for (auto it = props.begin(); it != props.end(); ++it)
                if (const qsizetype n = it.value().toObject().value("description").toString().size(); n > 0) {
                    if (!required.contains(it.key())) texts.push_back({n, it.key()});
                    sum += n;
                }
            std::sort(texts.begin(), texts.end(), [](const auto& a, const auto& b) {
                return a.first != b.first ? a.first > b.first : a.second < b.second;
            });
            for (const auto& [n, key] : texts) {
                if (sum <= paramTextFor(name)) break;
                QJsonObject p = props[key].toObject();
                p.remove("description");
                props[key] = p;
                sum -= n;
            }
        }
        if (name == "help")
            description = "The details of a tool: command '<tool>' (all its parameters, also those without a text "
                          "here); of edit ops: command 'edit' and op '<name>' (or a comma list).";
        if (name == "edit") {
            QJsonObject p = props["ops"].toObject();
            p["description"] = "List of operations, applied in order (nothing changes if one fails). Times: frames, "
                               "\"4.5s\" or \"MM:SS\"; ranges [from, to). Tracks: \"V1\", \"A2\". Clip ids come from `info` or the "
                               "answer of the edit before (they change when clips are split). Ops, each written as "
                               "name(keys) for {op:'name', keys}, key? = optional, a|b = one of them, … = more keys; "
                               "every op in detail: `help` with command 'edit' and op '<name>'. "
                               + compactOps(editOps);
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
        "them. Unsure about an op or tool: ask `help` (command 'edit' + op '<name>', or command '<tool>'), never "
        "guess; `effects` lists effects and transitions.\n"
        "4. Check with `check` and `info`, look with `frames`; check rendered files too.\n"
        "5. Times in seconds (\"4.5s\") are the safest. Never overwrite the user's files: renders get new names. Do "
        "what was asked, with good defaults (-14 LUFS for the web, H.264 MP4). Downloads "
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
