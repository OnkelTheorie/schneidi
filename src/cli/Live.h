#pragma once
// Live connection between schneidi-cli (and its MCP server) and the running app: if the project is open in the
// app, commands work on what the app shows instead of the file. Reading commands get a copy of the open project
// (unsaved changes included), changing commands (edit, import, app) run inside the app, one undo step each.
//
// Transport: QLocalSocket (Unix socket / Windows named pipe), one JSON object per line. Every app writes a small
// file into registryDir() with its socket name and a random token; only the user can read it, the token proves
// the client is that user. Messages from the client:
//   {"type":"hello","token":…}            -> {"ok":true,"project":path,"busy":bool,"allowed":bool,"pid":…,
//                                              "waiting":text (only while the app waits, see Server::setWaiting)}
//   {"type":"attach","client":name}       -> {"ok":true} (one attached client at a time; shown in the app)
//   {"type":"snapshot"}                    -> {"ok":true,"data":project file JSON,"app":state}
//   {"type":"run","command":…,"args":{…}} -> {"ok":true,"result":{…}} or {"ok":false,"error":{code,message}}
// attach, snapshot and run while the app waits -> {"ok":false,"error":{"code":"APP_WAITING","message":text}}
// hello of an app that refuses AIs has "allowed":false and "refusal": why (the user disconnected, or the setting)

#include "cli/Commands.h"

#include <QHash>
#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QPointer>
#include <QString>

class LiveHost;
class QLocalServer;

namespace Cli::Live {

// "@app" as project = the project open in the (only) running app
inline constexpr const char* AppProject = "@app";
bool isAppProject(const QString& project);

QString registryDir();

// The app the current command runs in (set by the server while it runs a command), nullptr in schneidi-cli
LiveHost* host();
// State the AI sees: playhead, selection, open timeline, playing, marks
QJsonObject appState(LiveHost* host);

// ---------- App side ----------

class Server : public QObject {
    Q_OBJECT
public:
    explicit Server(LiveHost* host, QObject* parent = nullptr);
    ~Server() override;
    bool start(QString* error = nullptr);

    bool attached() const { return m_attached; }
    QString client() const { return m_client; }
    // The user disconnects the AI ("Trennen") or turned AI connections off in the settings: the session ends and
    // new ones are refused until allowed again; `reason` tells the AI why (empty = "the user disconnected the AI")
    void disconnectClient(const QString& reason = {});
    bool allowed() const { return m_allowed; }
    void setAllowed(bool on);
    // The app cannot take commands yet (its restore dialog at start waits for the user, a project is being opened):
    // `message` tells the AI why (empty = ready). The server runs from the start, so AIs hear "waiting" instead of
    // "the app is not running"; nothing runs meanwhile (a modal dialog's event loop still delivers requests).
    void setWaiting(const QString& message);
    QString waiting() const { return m_waiting; }

signals:
    void stateChanged(); // attached/allowed changed

private:
    struct Peer {
        QByteArray buffer;
        bool authed = false;
    };
    void onConnection();
    void onReadyRead(QLocalSocket* s);
    void handle(QLocalSocket* s, const QJsonObject& msg);
    QJsonObject runCommand(const QJsonObject& msg);
    void detach(QLocalSocket* s);

    LiveHost* m_host;
    QLocalServer* m_server = nullptr;
    QString m_token;
    QString m_file; // registry entry
    QHash<QLocalSocket*, Peer> m_peers;
    QPointer<QLocalSocket> m_attached;
    QString m_client;
    bool m_allowed = true;
    QString m_refusal; // why new sessions are refused (empty = the user disconnected the AI)
    QString m_waiting;
    bool m_running = false; // a command runs (it may spin the event loop: no second one meanwhile)
};

// ---------- schneidi-cli side ----------

// Connection to the app that has `project` open; nullptr = no app has it open (work on the file). Throws
// APP_BUSY (another AI session is connected), APP_DISCONNECTED (the user disconnected the AI), APP_WAITING (the
// app waits for the user, e.g. its restore dialog at start) and, for "@app", NO_APP / AMBIGUOUS. The connection
// stays open for the process (the MCP server keeps its session).
class Connection;
Connection* connectionFor(const QString& project);
// Project file open in the app (empty = never saved)
QString projectPath(Connection* c);
// The open project as a project file (JSON) plus the app state; for reading commands
struct Snapshot {
    QString path; // project file in the app (empty = never saved)
    QByteArray data;
    QJsonObject app;
};
Snapshot snapshot(Connection* c);
// Runs a command inside the app; throws its Error
QJsonObject run(Connection* c, const QString& command, const QJsonObject& args);
// Name shown in the app ("schneidi-cli" or "schneidi-cli mcp"); set by main()
void setClientName(const QString& name);

} // namespace Cli::Live
