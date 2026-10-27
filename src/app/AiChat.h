#pragma once
// The app's own AI chat (the "AI" panel, app/AiChatPanel.h): a conversation with a model (app/AiProvider.h,
// app/AiClient.h) whose tools are schneidi's own, served by `schneidi-cli mcp` (app/McpClient.h). One request of the
// user is one turn: the model answers, calls tools, gets their results … until it answers without a tool call.
// Edits reach the app through the live connection (cli/Live.h), one undo step each, like those of any AI.
// The history lives only in memory (never in the project, never on disk) and is only ever appended to (providers
// tie their thinking blocks to it).

#include "app/AiProvider.h"

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QObject>
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
        enum class Kind { User, Assistant, Tool, Notice, Error };
        enum class State { Running, Done, Failed, Stopped };
        Kind kind = Kind::Notice;
        QString text;    // what the user or the model wrote, a notice, an error
        QString tool;    // Tool: the command
        QString summary; // Tool: one line ("Sucht Stille · a.mp4")
        QString details; // Tool: arguments and the whole answer (the panel shows its start when opened)
        QVector<QImage> images; // Tool: pictures it returned (frames)
        State state = State::Done;
        int percent = -1; // Tool: progress while it runs
    };

    explicit AiChat(QObject* parent = nullptr);
    ~AiChat() override;

    // The tool server: program + arguments, extra environment (the live connection to this app)
    void setTools(const QStringList& command, const QHash<QString, QString>& environment);
    void setLanguage(const QString& englishName); // the model answers in it ("German")
    // Test runs (--screenshot, --demo, --actions, offscreen): nothing is ever sent
    void setOffline(bool on) { m_offline = on; }
    bool offline() const { return m_offline; }

    // A request of the user, to `endpoint`; review = the user accepts or discards the turn's changes at its end.
    // false = not now (a turn runs, or a test run)
    bool send(const QString& text, const Ai::Endpoint& endpoint, bool review);
    void stop();  // ends the running turn now (what the model wrote so far stays)
    void clear(); // a new chat
    // Goes to the model with the next request ("the user discarded your changes")
    void addNote(const QString& text) { m_notes << text; }
    void addNotice(const QString& text); // shown only
    void addError(const QString& text);

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
    bool m_review = false;
    QString m_system; // fixed for the chat (providers cache it with the tools)
    QString m_guideText; // the editing guide (the `guide` tool), read once
    QStringList m_guideTopics; // its task guides ("social" …)
    enum class Guide { No, Asking, Read } m_guide = Guide::No;
    Ai::Usage m_usage;
    int m_rounds = 0;
    bool m_nudged = false; // an empty answer was answered with "go on" in this request
    QHash<QString, QString> m_failedCalls; // calls that failed in this request (name + arguments) -> their error
    int m_repeats = 0; // failed calls sent again in this request
    qint64 m_sentChars = 0;
    QElapsedTimer m_waiting; // since the request to the model went out, until its first word // text of the last request to the model (system, tools, history; no pictures)
    int m_streaming = -1; // entry of the answer that streams in
    QVector<Ai::ToolCall> m_calls; // of the last answer
    QVector<Ai::ToolResult> m_results;
};
