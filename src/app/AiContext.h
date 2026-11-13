// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// What the AI chat's request takes of a model's context (app/AiChat.h): the size of the context, asked from the
// server before the first request, and the short request for small or local models. The full request (instructions,
// the editing guide, all tools with their whole descriptions) is ~15,000 tokens: a model with 16k tokens of context has
// nearly no room left, Ollama's default of 4096 cuts it silently, and a model on the CPU reads for minutes. The short
// one is under half of it: every tool stays, with shorter texts; the details come from `help` when the model needs
// them. No widgets, no network: tested directly (tests/test_ai_providers.cpp).

#include "app/AiProvider.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

namespace Ai {

// ---------- The short request ----------

// Settings › AI, "Kurze Anfrage für kleine Modelle"
enum class Compact { Auto, On, Off };
// A known context below this takes the short request (the full one would leave less than half of it for the chat)
constexpr qint64 kCompactBelow = 32768;
// Auto: servers on this computer (they read slowly; speed first) and models with a known context below kCompactBelow;
// never Anthropic's protocol (Claude takes the full request). context: tokens, 0 = not known
bool useCompact(Compact mode, const Endpoint& e, qint64 context);

// The tools in short: all of them, each with the first sentence of its description and short parameter texts (the
// first clause; at most kParamText characters of them per tool); rarely needed tools (kRareTools) with parameter names
// and types only; `edit` lists its ops as signatures written as calls
// ("split(at, clip?)", see compactOps; editOps: the 'ops' of `help` with command 'edit'; empty = `edit` stays as it
// is); `help` says it has the details of every tool. The order and the names stay. New ops and parameters cost their
// names and a bounded short text, so the request stays within kCompactBudget without trimming texts by hand.
// In short also: time parameters typed as text only, op signatures without the values of optional keys, and no text
// for a parameter whose text kSharedParam or more tools share word for word ("project": "project file").
QJsonArray compactTools(const QJsonArray& tools, const QJsonArray& editOps);
constexpr int kSharedParam = 3;
// A tool's description in short: its first sentence, at most this long
constexpr int kToolText = 130;
// At most this many characters of parameter texts per tool (the longest are left out first)
constexpr qsizetype kParamText = 400;
// Tools the chat uses for nearly every task: more room for their parameter texts (render: quality, subtitles)
qsizetype paramTextFor(const QString& tool);
// The tool server's instructions in short: without the list of what each tool is for (the tool descriptions say it)
// and without how the app's project works live (the app chat's own part says it)
QString compactInstructions(const QString& instructions);
// What the short request (system prompt and tools, no chat) may take at most, in characters: about 7,500 tokens, so
// it fits an 8192-token local model with room for a short chat (test_ai_chat checks it). Measured with Ollama (0.22,
// 28,762 characters incl. the chat template): Llama 3.2 6,909 tokens (4.16 characters per token), Qwen 2.5 7,060
// (4.07) - the full budget leaves them ~600 tokens of 8192; Gemma 4 8,068 (3.56, its tool template is long) needs a
// 16k context (what the tight-context notice recommends for 8192)
constexpr qint64 kCompactBudget = 31000;
extern const QStringList kRareTools;
// The ops of `edit` in short, so that a new op costs at most about kOpChars characters (the short request no longer
// grows past its budget with every op): the ops of common edits (kCoreOps) with their whole signature, rarely needed
// ones (kRareOps) by name only, every other op with its required keys and as many optional ones as fit in kOpChars,
// then "…" (more keys or forms: `help`). Every op's full text stays one `help` call away.
QString compactOps(const QJsonArray& editOps);
constexpr int kOpChars = 56;
extern const QStringList kCoreOps;
extern const QStringList kRareOps;
// An op's 'usage' from `help` written as a call, without the values of optional keys: "{op:'split', at, clip?}" ->
// "split(at, clip?)" (what kCoreOps show)
QString shortUsage(const QString& usage);
// A signature "name(a, b?, c?)" cut to `max` characters: required keys always, optional ones while they fit, "…"
// for the rest (other forms "… or name(…)" included)
QString cappedUsage(const QString& usage, int max);
// The editing guide's main points (instead of its whole main text), the task guides after it ("- ad: Ad, …" lines)
QString compactGuide(const QString& topics);
// The first sentence of a tool description, at most `max` characters (cut at a clause outside brackets)
QString firstSentence(const QString& text, int max = 180);

// ---------- The context of the model ----------

// No standard says how large a model's context is; the servers say it in their own ways: llama.cpp (GET /props: n_ctx
// of a slot), Ollama (POST /api/show: num_ctx among the model's parameters; GET /api/ps: context_length of the loaded
// model), LM Studio (GET /api/v0/models: loaded_context_length), vLLM (GET …/models: max_model_len). Asked only of
// servers of the OpenAI protocol that are no known cloud provider; all at once, each with a short timeout.
struct ContextProbe {
    QString kind;      // "ollama-show", "ollama-ps", "llama.cpp", "lmstudio", "models"
    QByteArray method; // "GET" or "POST"
    QUrl url;
    QByteArray body; // POST: JSON
};
QVector<ContextProbe> contextProbes(const Endpoint& e); // empty = nothing to ask
// The context (tokens) one answer gives for `model`; 0 = it says none
qint64 contextOf(const QString& kind, const QJsonObject& answer, const QString& model);
// The answers of all probes (kind -> tokens, 0 = none): the one that counts. Ollama: the model's num_ctx before the
// loaded model's context (a request with other options loads it again)
qint64 contextFrom(const QHash<QString, qint64>& found);
// Room the chat needs besides the instructions and tools: the user's words, tool results, the model's answers
constexpr qint64 kChatRoom = 4096;
// The context to set on the server for a request whose fixed part (instructions, tools) has `fixed` tokens: the next
// power of two with room for the chat
qint64 recommendedContext(qint64 fixed);

} // namespace Ai
