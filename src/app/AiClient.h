#pragma once
// Network side of the AI chat's providers (app/AiProvider.h builds and reads the messages): one request at a time,
// streamed, with a timeout, a few tries more when the provider is busy (429, overloaded, 5xx – only before anything
// arrived) and messages the user understands. Also the model list and the connection test of Settings › AI.
// The key goes only into the request headers, never into a log or a message.

#include "app/AiProvider.h"

#include <QNetworkReply>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>

class QNetworkAccessManager;

class AiClient : public QObject {
    Q_OBJECT
public:
    explicit AiClient(QObject* parent = nullptr);
    ~AiClient() override;

    // One request (Request::stream: streamed); answers once with finished() or failed()
    void send(const Ai::Endpoint& e, const Ai::Request& r);
    void abort(); // the running request ends without a signal
    bool busy() const { return m_reply || m_retry.isActive(); }

    void listModels(const Ai::Endpoint& e); // -> models() or modelsFailed()
    void test(const Ai::Endpoint& e);       // a tiny request -> tested()
    // How large the model's context is, as the server says it (app/AiContext.h): every probe at once, each at most
    // timeoutMs -> contextProbed() once (0 = not said); a newer probe replaces a running one
    void probeContext(const Ai::Endpoint& e, int timeoutMs = 2000);

    // No data for this long: the request counts as hanging (tests make it short)
    void setTimeoutMs(int ms) { m_timeoutMs = ms; }
    // Waits before the tries after a busy answer (Retry-After wins when the provider sends it)
    void setRetryDelays(const QVector<int>& ms) { m_retryDelays = ms; }

signals:
    void text(const QString& delta); // streamed text as it arrives
    void retrying(int seconds, const QString& why);
    void finished(const Ai::Reply& reply);
    void failed(const QString& message);
    void models(const QStringList& ids);
    void modelsFailed(const QString& message);
    void tested(bool ok, const QString& message);
    void contextProbed(qint64 tokens);

private:
    void start();
    void onData();
    void onFinished();
    QNetworkReply* post(const Ai::Endpoint& e, const QJsonObject& body, bool fallbacks);
    QString networkError(QNetworkReply* reply, const Ai::Endpoint& e) const;
    static QString redirectError(QNetworkReply* reply); // a redirect to another address (not followed)

    QNetworkAccessManager* m_nam;
    QPointer<QNetworkReply> m_reply;
    Ai::Endpoint m_endpoint;
    Ai::Request m_request;
    Ai::StreamParser m_parser{Ai::Protocol::Anthropic};
    QByteArray m_errorBody;
    QTimer m_retry;
    int m_attempt = 0;
    bool m_compat = false; // Anthropic: sent again without the optional parts (a proxy refused them)
    int m_timeoutMs = 180000;
    QVector<int> m_retryDelays{3000, 10000};
    QPointer<QNetworkReply> m_models, m_test; // model list, test
    QVector<QPointer<QNetworkReply>> m_probes; // the context probe's requests still running
    QHash<QString, qint64> m_probed;           // their answers so far (kind -> tokens)
};
