// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The app's own AI chat (the "AI" panel, app/AiChatPanel.h): a conversation with a model (app/AiProvider.h,
// app/AiClient.h) whose tools are schneidi's own, served by `schneidi-cli mcp` (app/McpClient.h). One request of the
// user is one turn: the model answers, calls tools, gets their results … until it answers without a tool call.
// Edits reach the app through the live connection (cli/Live.h), one undo step each, like those of any AI.
// The history lives only in memory (never in the project, never on disk) and is only ever appended to (providers
// tie their thinking blocks to it) - except for a model with a small context its server named (a local one): there
// the oldest tool results give way to a placeholder when the next request would not fit (fitHistory, in steps so
// that a server's cached prefix stays valid for a few rounds).

#include "app/AiContext.h"
#include "app/AiProvider.h"

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <functional>

class AiClient;
class McpClient;

class AiChat : public QObject {
    Q_OBJECT
public:
    // What the panel shows, in order
    struct Entry {
        // Plan: a plan the AI made (plan mode): its steps with reasons, the user runs the ticked ones or discards it
        enum class Kind { User, Assistant, Tool, Notice, Error, Plan };
        // Plan: Running = waits for the user, Done = run, Stopped = discarded, Failed = could not run (text: why)
        enum class State { Running, Done, Failed, Stopped };
        Kind kind = Kind::Notice;
        QString text;    // what the user or the model wrote, a notice, an error
        QString tool;    // Tool: the command
        QString summary; // Tool: one line ("Sucht Stille · a.mp4")
        QString details; // Tool: arguments and the whole answer (the panel shows its start when opened)
        QVector<QImage> images; // Tool: pictures it returned (frames)
        State state = State::Done;
        int percent = -1; // Tool: progress while it runs
        QJsonObject plan; // Plan: the plan as `edit` answered it {id, steps:[{n, op, why, summary, changes}], summary}
        QVector<int> ran; // Plan: the steps that were run (Done)
    };
    // How the user wants the AI's changes: right away, reviewed at the end of a request, or as a plan first
    enum class Mode { Auto, Review, Plan };

    explicit AiChat(QObject* parent = nullptr);
    ~AiChat() override;

    // The tool server: program + arguments, extra environment (the live connection to this app)
    void setTools(const QStringList& command, const QHash<QString, QString>& environment);
    void setLanguage(const QString& englishName); // the model answers in it ("German")
    // The short request for small or local models (app/AiContext.h); chosen at the first request of a chat
    void setCompact(Ai::Compact mode) { m_compactMode = mode; }
    bool compact() const { return m_compact; } // the running chat has the short request
    // The context of the endpoint's model as its server said it (asked once per chat, address and model; 0 = not known)
    // When the last probe found none, the last one named for it (Ollama names none for a model not loaded with
    // OLLAMA_CONTEXT_LENGTH set; it did not change for that)
    qint64 context(const Ai::Endpoint& e) const
    {
        const qint64 n = m_contexts.value(contextKey(e), 0);
        return n > 0 ? n : m_lastContexts.value(contextKey(e), 0);
    }
    // Test runs (--screenshot, --demo, --actions, offscreen): nothing is ever sent
    void setOffline(bool on) { m_offline = on; }
    bool offline() const { return m_offline; }

    // A request of the user, to `endpoint`; mode: Review = the user accepts or discards the turn's changes at its end,
    // Plan = the AI makes a plan (`edit` plan:true) the user runs from its card. false = not now (a turn runs, a test run)
    bool send(const QString& text, const Ai::Endpoint& endpoint, Mode mode = Mode::Auto);
    void stop();  // ends the running turn now (what the model wrote so far stays)
    void clear(); // a new chat
    // Goes to the model with the next request ("the user discarded your changes")
    void addNote(const QString& text) { m_notes << text; }
    void addNotice(const QString& text); // shown only
    void addError(const QString& text);
    // Plans (shown as a card): a new one, and what became of it (state Done with the steps run, Stopped, Failed with
    // the reason as text). -1 / nothing when there is no card of that id.
    int addPlan(const QJsonObject& plan);
    int planEntry(const QString& id) const;
    void setPlanState(const QString& id, Entry::State state, const QVector<int>& ran = {}, const QString& text = {});
    // Every plan card that can still run is void (another project was opened: plan ids start anew per project,
    // a card would run the new project's plan of the same id)
    void stopPlans(const QString& text);

    bool busy() const { return m_busy; }
    QString request() const { return m_request; } // what the user asked for in the running/last turn
    const QVector<Entry>& entries() const { return m_entries; }
    const QVector<Ai::Message>& history() const { return m_history; }
    Ai::Usage usage() const { return m_usage; }
    QString transcript() const; // the whole chat as text ("Copy")
    // Waiting for the model's first word of its answer: since when (ms), -1 = not waiting (tools run, it writes, idle)
    qint64 waitingMs() const { return m_busy && m_waiting.isValid() ? m_waiting.elapsed() : -1; }
    bool localModel() const { return Ai::isLocal(m_endpoint.address); } // a server on this computer
    qint64 requestTokens() const { return m_sentChars / 4; }             // about, of the last request
    McpClient* tools() const { return m_mcp; }
    AiClient* client() const { return m_client; }

    // One line for a tool step: "Sucht Stille · a.mp4", "Bearbeitet: split, delete"
    static QString toolLine(const QString& tool, const QJsonObject& args);
    // What the model gets back from a tool: long answers cut with a note, at most kMaxImages pictures of at most
    // kMaxImageEdge
    static Ai::ToolResult capped(Ai::ToolResult r);
    static constexpr int kMaxToolText = 24000;
    static constexpr int kMaxImages = 8;
    static constexpr int kMaxImageEdge = 2000; // pixels, longer edges are scaled down
    static constexpr int kMaxRounds = 40; // tool rounds per request
    static constexpr int kMaxRepeats = 2; // a failed call sent again this often ends the request (OpenAI protocol)

    // Fits the history into a context of `context` tokens (one the server named) with room for the chat's next steps
    // (Ai::kChatRoom), estimated with kCharsPerToken (conservative: Gemma 4 has 3.56 in our requests) and kImageTokens
    // per picture; fixedChars = the system prompt and the tools. Once over, down to kFitShare % of that (the history
    // changes only every few rounds: a local server re-reads its cached prefix from the first change on). What gives
    // way, oldest first: the results of earlier turns (messages up to turnStart, the running turn's first one), then
    // those of the running turn's earlier rounds; pictures before texts (kRemovedResult instead). Never the user's
    // words, the model's calls, nor the results in the last message while it is a round of the running turn (after
    // turnStart): every tool call keeps its answer of the same id, so the history stays valid in both protocols.
    // Returns how many results or pictures were removed.
    static int fitHistory(QVector<Ai::Message>& history, qint64 fixedChars, qint64 context, qsizetype turnStart);
    // The tokens of a request with this history, as fitHistory estimates them
    static qint64 estimatedTokens(const QVector<Ai::Message>& history, qint64 fixedChars);
    static constexpr double kCharsPerToken = 3.5;
    static constexpr qint64 kImageTokens = 1000;
    static constexpr qint64 kFitShare = 75;
    static constexpr int kMaxRecalls = 3; // calls of a removed result again per request, then it ends
    static const QString kRemovedResult;

signals:
    void entryAdded(int index);
    void entryChanged(int index);
    void cleared();
    void busyChanged(bool busy);
    void usageChanged();
    void turnFinished(); // the model is done, failed or was stopped
    void stopped();      // the user stopped a request (its tool server was ended if a tool ran)

private:
    int add(Entry e);
    void change(int index, const std::function<void(Entry&)>& fn);
    void setBusy(bool on);
    void toolsReady();
    void prepare(); // what the first request of a turn needs (context, guide, ops of `edit`), then the request
    static QString contextKey(const Ai::Endpoint& e) { return e.address.trimmed() + '|' + e.model.trimmed(); }
    void requestModel();
    void onText(const QString& delta);
    void onReply(const Ai::Reply& reply);
    void onFailed(const QString& message);
    void nextTool();
    void toolDone(int entry, const Ai::ToolCall& call, const QString& text, const QVector<Ai::Image>& images, bool error);
    void finishTurn();
    QString systemPrompt() const;
    void appendUser(const QString& text, const QVector<Ai::ToolResult>& results = {});

    AiClient* m_client;
    McpClient* m_mcp;
    QString m_language = "English";
    bool m_offline = false;
    bool m_busy = false;
    bool m_stopped = false;
    QVector<Entry> m_entries;
    QVector<Ai::Message> m_history;
    QStringList m_notes;
    QString m_request;
    Ai::Endpoint m_endpoint;
    Mode m_mode = Mode::Auto;
    QString m_system; // fixed for the chat (providers cache it with the tools)
    QString m_guideText; // the editing guide (the `guide` tool), read once
    QString m_guideTopicList; // its task guides, one "- social: Short, Reel …" line each
    QStringList m_guideTopics; // their names ("social" …)
    enum class Guide { No, Read } m_guide = Guide::No;
    bool m_asking = false;     // the guide or the ops of `edit` are being read
    bool m_guideAsked = false; // in this turn (failed: asked again with the next one)
    QJsonArray m_editOps;      // `help` with command 'edit': the ops with their signatures, read once (short request)
    bool m_opsAsked = false;   // in this turn
    Ai::Compact m_compactMode = Ai::Compact::Auto;
    bool m_compact = false;    // the chat's request is the short one
    QJsonArray m_chatTools;    // the tools of the chat (fixed with m_system)
    QHash<QString, qint64> m_contexts; // contextKey -> tokens (0 = the server did not say)
    bool m_probing = false;    // the context is being asked
    QString m_probeKey;
    bool m_warned = false;     // a tight context was said in this chat
    bool m_trimmed = false;    // tool results were removed to fit the context (said once per chat)
    QHash<QString, qint64> m_lastContexts; // contextKey -> the last context named (kept across chats)
    qsizetype m_turnStart = 0; // the running turn's first message in m_history (the user's words)
    QSet<QString> m_trimmedCalls; // calls whose answers gave way to fit the context (callKey)
    int m_recalls = 0;            // of them, made again in this request
    Ai::Usage m_usage;
    int m_rounds = 0;
    bool m_nudged = false; // an empty answer was answered with "go on" in this request
    QHash<QString, QString> m_failedCalls; // calls that failed in this request (name + arguments) -> their error
    int m_repeats = 0; // failed calls sent again in this request
    qint64 m_sentChars = 0; // text of the last request to the model (system, tools, history; no pictures)
    QElapsedTimer m_waiting; // since the request to the model went out, until its first word
    int m_streaming = -1; // entry of the answer that streams in
    QVector<Ai::ToolCall> m_calls; // of the last answer
    QVector<Ai::ToolResult> m_results;
};
