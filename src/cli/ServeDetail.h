#pragma once
// Inside of `schneidi-cli serve` (Serve.h): the server object, shared by Serve.cpp (HTTP, MCP, jobs, options) and
// ServeFiles.cpp (files over HTTP, the tools `files` and `upload`).

#include "cli/Commands.h"
#include "cli/Http.h"
#include "cli/Mcp.h"
#include "cli/ServeJobs.h"

#include <QDateTime>
#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QSaveFile>
#include <QStringList>
#include <QTcpServer>

#include <functional>
#include <map>
#include <memory>

class QFileInfo;
class QNetworkAccessManager;
class QUrl;

namespace Cli::Serve {

constexpr int kListLimit = 2000; // entries of `files` / a folder listing

// Command line of `serve` (Serve.cpp parse())
struct Options {
    QHostAddress listen{QHostAddress::LocalHost};
    QString listenText = "127.0.0.1";
    int port = 8642;
    QString root;
    QStringList maps;
    QString token;
    QString tokenFrom;   // where it came from (banner)
    bool tokenShown = false; // generated or stored here: printed at start
    int workers = 0;
    bool extensions = false;
    int jobWaitS = 50;
    qint64 maxUpload = qint64(50) * 1024 * 1024 * 1024;
    QString publicUrl;
    QStringList origins;
    bool verbose = false;
};

QByteArray json(const QJsonObject& o);
QJsonObject errorJson(const QString& code, const QString& message);

// Listens, checks access, answers /mcp (tool calls become jobs on the workers) and /files
class Server : public QObject {
public:
    explicit Server(const Options& opt);
    bool listen(QString* error);
    QString url() const; // where clients reach /mcp (banner)
    quint16 port() const { return m_tcp->serverPort(); }

private:
    struct Session {
        QDateTime used;
        QHash<QString, QString> calls; // JSON-RPC request id (as JSON text) -> job, for notifications/cancelled
    };
    struct Upload {
        std::unique_ptr<QSaveFile> file;
        bool existed = false;
    };

    void onHead(Http::Connection* c);
    void onRequest(Http::Connection* c);
    bool allowedHost(const Http::Request& r) const;
    bool allowedOrigin(const Http::Request& r) const;
    bool authorized(const Http::Request& r) const;
    Http::Headers cors(const Http::Request& r) const;
    QString baseUrl(const Http::Request& r) const;
    QString fileUrl(const QString& base, const QString& path) const;

    // MCP
    void mcpPost(Http::Connection* c);
    void mcpRequest(const QJsonObject& msg, const QString& session, const QString& base, const Mcp::Send& send,
                    QObject* context);
    void mcpNotification(const QString& session, const QJsonObject& msg);
    void callTool(const QJsonObject& params, const QString& session, const QString& requestKey, const QString& base,
                  const Mcp::Send& notify, const Mcp::Done& done, QObject* context);
    void waitFor(const QString& id, double seconds, const QJsonValue& token, const Mcp::Send& notify,
                 const Mcp::Done& done, QObject* context, bool withJob);
    QJsonObject answer(const Job& j, bool withJob) const;
    QJsonObject withLinks(const Job& j, const QJsonObject& result) const;
    QString writtenProject(const Command& cmd, const QJsonObject& args) const;

    // Tools of the server
    void jobsTool(const QJsonObject& args, const QJsonValue& token, const Mcp::Send& notify, const Mcp::Done& done,
                  QObject* context);
    QJsonObject jobList() const;
    QJsonObject filesTool(const QJsonObject& args, const QString& base) const;
    QJsonObject entryJson(const QFileInfo& fi, const QString& base) const;
    void prepareUpload(Job& job, const QString& givenPath);
    void startUpload(const QString& id);
    void checkHost(const QUrl& url, const std::function<void(const QString& error)>& then);
    void download(const QString& id);

    // Files over HTTP
    void filesHead(Http::Connection* c);
    void filesGet(Http::Connection* c);
    void filesPutDone(Http::Connection* c);
    QString filePath(const QString& requestPath) const; // throws Error

    Options m_opt;
    Mcp::Options m_mcp;
    QTcpServer* m_tcp;
    Jobs* m_jobs;
    QNetworkAccessManager* m_net;
    QHash<QString, Session> m_sessions;
    std::map<Http::Connection*, Upload> m_uploads;
    int m_connections = 0;
    QHash<QString, int> m_perAddress; // open connections per client address (not this computer's)
};

} // namespace Cli::Serve
