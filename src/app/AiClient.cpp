#include "app/AiClient.h"

#include "app/AiContext.h"
#include "core/I18n.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <algorithm>

namespace {

// The system's proxy settings (companies often allow the internet only through one); for the chat's requests only.
// A server on this computer or in the local network (Ollama on localhost) is reached directly: a proxy cannot reach it
class SystemProxies : public QNetworkProxyFactory {
public:
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery& query) override
    {
        if (Ai::isNearHost(query.peerHostName())) return {QNetworkProxy(QNetworkProxy::NoProxy)};
        return systemProxyForQuery(query);
    }
};

QNetworkRequest request(const Ai::Endpoint& e, const QUrl& url, bool fallbacks, int timeoutMs)
{
    QNetworkRequest req(url);
    for (const auto& [name, value] : Ai::headers(e, fallbacks)) req.setRawHeader(name, value);
    req.setTransferTimeout(timeoutMs);
    // The key goes with every redirect Qt follows: only within the address the user set (onFinished explains others)
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    return req;
}

int statusOf(QNetworkReply* r) { return r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(); }

// A streamed answer (server-sent events); a server that ignores "stream" sends the whole answer as JSON instead
bool isEventStream(QNetworkReply* r)
{
    return r->header(QNetworkRequest::ContentTypeHeader).toString().contains("event-stream", Qt::CaseInsensitive);
}

// Model list, test: a reply replaced by a newer one of its kind ends without a signal
void dropSide(QPointer<QNetworkReply>& side)
{
    if (!side) return;
    side->setProperty("dropped", true);
    side->abort();
}

} // namespace

AiClient::AiClient(QObject* parent) : QObject(parent), m_nam(new QNetworkAccessManager(this))
{
    m_nam->setProxyFactory(new SystemProxies); // takes ownership
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &AiClient::start);
}

AiClient::~AiClient()
{
    abort();
    dropSide(m_models);
    dropSide(m_test);
    for (QPointer<QNetworkReply>& p : m_probes) dropSide(p);
}

void AiClient::send(const Ai::Endpoint& e, const Ai::Request& r)
{
    abort();
    m_endpoint = e;
    m_request = r;
    m_attempt = 0;
    m_compat = false;
    start();
}

void AiClient::abort()
{
    m_retry.stop();
    if (!m_reply) return;
    QNetworkReply* r = m_reply;
    m_reply = nullptr; // its finished() is not ours anymore
    r->disconnect(this);
    r->abort();
    r->deleteLater();
}

QNetworkReply* AiClient::post(const Ai::Endpoint& e, const QJsonObject& body, bool fallbacks)
{
    return m_nam->post(request(e, Ai::chatUrl(e), fallbacks, m_timeoutMs), QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void AiClient::start()
{
    Ai::Request r = m_request;
    if (m_compat) {
        r.fallbacks = false;
        r.autoCache = false;
    }
    m_parser = Ai::StreamParser(m_endpoint.protocol);
    m_errorBody.clear();
    m_reply = post(m_endpoint, Ai::body(m_endpoint.protocol, r), r.fallbacks);
    connect(m_reply, &QNetworkReply::readyRead, this, &AiClient::onData);
    connect(m_reply, &QNetworkReply::finished, this, &AiClient::onFinished);
}

void AiClient::onData()
{
    if (!m_reply) return;
    const QByteArray data = m_reply->readAll();
    if (statusOf(m_reply) != 200 || !m_request.stream || !isEventStream(m_reply)) {
        m_errorBody += data; // an error answer, or the whole answer at once
        return;
    }
    const QString delta = m_parser.feed(data);
    if (!delta.isEmpty()) emit text(delta);
}

QString AiClient::networkError(QNetworkReply* reply, const Ai::Endpoint& e) const
{
    const QString host = QUrl(e.address.trimmed()).host();
    switch (reply->error()) {
    case QNetworkReply::ConnectionRefusedError:
        return T("Keine Verbindung zu %1 – läuft der Dienst?").arg(host);
    case QNetworkReply::HostNotFoundError:
        return T("Adresse %1 nicht gefunden (Tippfehler oder kein Netz?).").arg(host);
    case QNetworkReply::OperationCanceledError: // the transfer timeout (abort() of the user emits nothing)
    case QNetworkReply::TimeoutError:
        return T("Keine Antwort von %1 (Zeitüberschreitung).").arg(host);
    case QNetworkReply::SslHandshakeFailedError:
        return T("Sichere Verbindung zu %1 fehlgeschlagen: %2").arg(host, reply->errorString());
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::UnknownNetworkError:
        return T("Kein Netz: %1").arg(reply->errorString());
    case QNetworkReply::RemoteHostClosedError:
        return T("Die Verbindung zu %1 wurde unterbrochen.").arg(host);
    default:
        return T("Netzwerkfehler: %1").arg(reply->errorString());
    }
}

QString AiClient::redirectError(QNetworkReply* reply)
{
    const QUrl to = reply->url().resolved(QUrl::fromEncoded(reply->rawHeader("location")));
    return T("Der Server leitet zu %1 weiter. Der Schlüssel geht nur an die eingestellte Adresse: bitte diese Adresse in "
             "den Einstellungen eintragen, wenn sie stimmt.")
        .arg(to.adjusted(QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment).toString());
}

void AiClient::onFinished()
{
    QNetworkReply* reply = m_reply;
    if (!reply || sender() != reply) return;
    m_reply = nullptr;
    reply->deleteLater();
    const int status = statusOf(reply);
    m_errorBody += reply->readAll();
    const bool streamed = m_request.stream && status == 200 && isEventStream(reply);
    if (streamed) {
        const QString delta = m_parser.feed("\n\n"); // an event the server did not close with a blank line
        if (!delta.isEmpty()) emit text(delta);
    }
    const auto again = [this](int delayMs, const QString& why) {
        if (m_attempt >= m_retryDelays.size()) return false;
        ++m_attempt;
        emit retrying(std::max(1, (delayMs + 999) / 1000), why);
        m_retry.start(delayMs);
        return true;
    };

    if (status == 0 && reply->error() != QNetworkReply::InsecureRedirectError) { // no answer at all
        emit failed(networkError(reply, m_endpoint));
        return;
    }
    if ((status >= 300 && status < 400) || reply->error() == QNetworkReply::InsecureRedirectError) {
        // To another address: not followed, the key stays here
        emit failed(redirectError(reply));
        return;
    }
    if (status != 200) {
        const Ai::Failure f = Ai::httpFailure(status, m_errorBody, reply->rawHeader("retry-after"), m_request.fixedTokens);
        // A proxy or older server that does not know the optional parts of the Anthropic request: once without them
        const QString said = Ai::providerMessage(m_errorBody).toLower();
        if (status == 400 && m_endpoint.protocol == Ai::Protocol::Anthropic && !m_compat
            && (said.contains("fallback") || said.contains("cache_control"))) { // (the beta's name has "fallback")
            m_compat = true;
            start();
            return;
        }
        if (f.retry && again(f.retryAfterS > 0 ? std::min(f.retryAfterS, 60) * 1000 : m_retryDelays.value(m_attempt), f.message))
            return;
        emit failed(f.message);
        return;
    }
    Ai::Reply r = streamed ? m_parser.reply()
                           : Ai::parseBody(m_endpoint.protocol, QJsonDocument::fromJson(m_errorBody).object());
    if (!r.error.isEmpty()) {
        // Overloaded before anything came: try again like a busy answer
        if (!m_parser.started() && (r.errorType == "overloaded_error" || r.errorType == "rate_limit_error")
            && again(m_retryDelays.value(m_attempt), r.error))
            return;
        emit failed(T("Der Anbieter hat die Antwort abgebrochen: %1").arg(r.error));
        return;
    }
    if (streamed && !m_parser.finished() && reply->error() != QNetworkReply::NoError) {
        emit failed(networkError(reply, m_endpoint));
        return;
    }
    emit finished(r);
}

void AiClient::listModels(const Ai::Endpoint& e)
{
    dropSide(m_models);
    QNetworkReply* reply = m_nam->get(request(e, Ai::modelsUrl(e), false, 30000));
    m_models = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, e] {
        reply->deleteLater();
        const int status = statusOf(reply);
        const QByteArray body = reply->readAll();
        if (reply->property("dropped").toBool()) return;
        if (status == 200) emit models(Ai::parseModels(QJsonDocument::fromJson(body).object()));
        else if ((status >= 300 && status < 400) || reply->error() == QNetworkReply::InsecureRedirectError)
            emit modelsFailed(redirectError(reply));
        else emit modelsFailed(status ? Ai::httpFailure(status, body).message : networkError(reply, e));
    });
}

void AiClient::test(const Ai::Endpoint& e)
{
    dropSide(m_test);
    Ai::Request r;
    r.model = e.model;
    r.provider = e.provider;
    r.stream = false;
    r.autoCache = false;
    r.maxTokens = 64; // a few tokens: costs next to nothing
    Ai::Message m;
    m.text = "Reply with the single word OK.";
    r.messages << m;
    QNetworkReply* reply = post(e, Ai::body(e.protocol, r), false);
    m_test = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, e] {
        reply->deleteLater();
        const int status = statusOf(reply);
        const QByteArray body = reply->readAll();
        if (reply->property("dropped").toBool()) return;
        if (status == 200) emit tested(true, T("Verbindung steht: %1 antwortet.").arg(e.model));
        else if ((status >= 300 && status < 400) || reply->error() == QNetworkReply::InsecureRedirectError)
            emit tested(false, redirectError(reply));
        else emit tested(false, status ? Ai::httpFailure(status, body).message : networkError(reply, e));
    });
}

void AiClient::probeContext(const Ai::Endpoint& e, int timeoutMs)
{
    for (QPointer<QNetworkReply>& p : m_probes) dropSide(p);
    m_probes.clear();
    m_probed.clear();
    const QVector<Ai::ContextProbe> probes = Ai::contextProbes(e);
    if (probes.isEmpty()) {
        QTimer::singleShot(0, this, [this] { emit contextProbed(0); });
        return;
    }
    for (const Ai::ContextProbe& p : probes) {
        QNetworkRequest req = request(e, p.url, false, timeoutMs);
        QNetworkReply* reply = p.method == "POST" ? m_nam->post(req, p.body) : m_nam->get(req);
        m_probes << reply;
        connect(reply, &QNetworkReply::finished, this, [this, reply, kind = p.kind, model = e.model] {
            reply->deleteLater();
            const QByteArray body = reply->readAll();
            if (reply->property("dropped").toBool()) return;
            m_probed.insert(kind, statusOf(reply) == 200
                                      ? Ai::contextOf(kind, QJsonDocument::fromJson(body).object(), model)
                                      : 0);
            m_probes.removeAll(reply);
            if (m_probes.isEmpty()) emit contextProbed(Ai::contextFrom(m_probed));
        });
    }
}
