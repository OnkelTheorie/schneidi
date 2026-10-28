// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// MCP client of the app's AI chat: starts `schneidi-cli mcp` (the same version next to the app; main.cpp hands in
// the command, the app does not link the command layer) and talks MCP with it over stdin/stdout: initialize,
// tools/list, tools/call with progress. The chat thus has exactly the tools and rules of every other AI, analyses and
// renders run outside the GUI, and edits reach the app through the live connection (cli/Live.h) like those of an
// AI from outside – one undo step each, lit up, in the AI log.

#include "app/AiProvider.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QStringList>

#include <functional>

class QProcess;

class McpClient : public QObject {
    Q_OBJECT
public:
    struct Result {
        QString text; // the tool's answer (JSON text)
        QVector<Ai::Image> images;
        bool error = false;
    };
    using Progress = std::function<void(int percent)>;
    using Done = std::function<void(const Result& result)>;

    explicit McpClient(QObject* parent = nullptr);
    ~McpClient() override; // ends the server

    // Program + arguments, and extra environment (the live connection to this app)
    void setCommand(const QStringList& command, const QHash<QString, QString>& environment = {});
    bool configured() const { return !m_command.isEmpty(); }
    // Starts the server when it does not run yet; ready() when it takes calls (or failed())
    void start();
    bool isReady() const { return m_ready; }
    bool isStarting() const { return m_starting; }
    // How long starting (initialize, tools/list) and a call with a time limit may take before the server counts as
    // hung: it is ended and failed() says so
    void setSetupTimeoutMs(int ms) { m_setupTimeoutMs = ms; }
    int setupTimeoutMs() const { return m_setupTimeoutMs; }
    QString instructions() const { return m_instructions; } // the server's own words for the AI
    QJsonArray tools() const { return m_tools; }            // as tools/list gives them

    // A tool call; done() once (also when the server dies or stop() ends it). timeoutMs > 0: a server that has not
    // answered by then is ended (failed() first, then done() with an error)
    void call(const QString& tool, const QJsonObject& arguments, const Progress& progress, const Done& done,
              int timeoutMs = 0);
    // Ends the server now (a running call is answered as stopped); the next start() starts a new one
    void stop();

signals:
    void ready();
    void failed(const QString& message);

private:
    void send(const QJsonObject& message);
    void onOutput();
    void onMessage(const QJsonObject& message);
    void onEnded();
    int request(const QString& method, const QJsonObject& params, const std::function<void(const QJsonObject&)>& reply);
    void finishAll(const Result& result);
    void endProcess();                // kills and forgets the server (no signals of it reach this object anymore)
    void giveUp(const QString& why); // the server is of no use: ended, failed(why), running calls answered

    QStringList m_command;
    QHash<QString, QString> m_environment;
    QProcess* m_process = nullptr;
    QByteArray m_buffer;
    QByteArray m_errors; // the server's last words on stderr (why it did not start)
    bool m_ready = false;
    bool m_starting = false;
    QString m_instructions;
    QJsonArray m_tools;
    int m_next = 1;
    int m_setupTimeoutMs = 60000;
    int m_generation = 0; // counts start(): a time limit of an earlier server does not end a newer one
    struct Pending {
        std::function<void(const QJsonObject&)> reply; // the whole response (result or error)
        Progress progress;
        Done done; // tool calls
    };
    QHash<int, Pending> m_pending;
};
