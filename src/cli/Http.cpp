// Small HTTP/1.1 server side, see Http.h
#include "cli/Http.h"

#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

namespace Cli::Http {

namespace {
constexpr qint64 kDefaultBody = 1 << 20;
constexpr qint64 kFilePiece = 256 * 1024;
constexpr int kMaxHeaders = 100;
constexpr int kIdleMs = 120 * 1000;   // keep-alive without a request
constexpr int kStalledMs = 60 * 1000; // half a head or body without new data
constexpr int kHeadMs = 30 * 1000;    // a whole head, however slowly it trickles in (connections are limited)
constexpr qint64 kDiscardMax = 4 << 20; // body of a request answered early (error): read and dropped up to this size
} // namespace

QByteArray reason(int status)
{
    switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Content Too Large";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Status";
    }
}

Connection::Connection(QTcpSocket* socket, QObject* parent) : QObject(parent), m_socket(socket), m_timer(new QTimer(this))
{
    m_socket->setParent(this);
    // While a request is handled, unread data stays in the system (TCP flow control) instead of piling up here
    m_socket->setReadBufferSize(1 << 20);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &Connection::close);
    connect(m_socket, &QTcpSocket::readyRead, this, &Connection::onReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &Connection::close);
    connect(m_socket, &QTcpSocket::bytesWritten, this, [this] {
        if (m_file) pumpFile();
    });
    armTimer();
}

Connection::~Connection() = default;

QString Connection::peer() const { return m_socket->peerAddress().toString(); }

bool Connection::isOpen() const { return m_state != State::Closed; }

void Connection::onReadyRead()
{
    if (m_state != State::Head && m_state != State::Body) return; // read on after the answer
    m_buf += m_socket->readAll();
    process();
}

void Connection::process()
{
    while (m_state == State::Head) {
        const qsizetype end = m_buf.indexOf("\r\n\r\n");
        if (end < 0) {
            if (!m_buf.isEmpty() && !m_headClock.isValid()) m_headClock.start();
            if (m_buf.size() > kMaxHead) fail(431, "request head too large");
            else if (m_headClock.isValid() && m_headClock.hasExpired(kHeadMs)) fail(408, "request head too slow");
            else armTimer();
            return;
        }
        m_headClock.invalidate();
        const QByteArray text = m_buf.left(end);
        m_buf.remove(0, end + 4);
        if (!parseHead(text)) return;
        m_received = 0;
        m_bodyMax = -1;
        m_sink = nullptr;
        m_state = State::Body;
        const quint64 request = ++m_requests;
        emit headReceived(this);
        // Answered right away (an error): the connection closes, or finishAnswer drops the rest of a small body and
        // may already be on the next request – nothing more to do for this one
        if (m_state != State::Body || m_discard || m_requests != request) return;
        if (!m_sink && m_bodyMax < 0) m_bodyMax = kDefaultBody;
        if (!m_sink && m_req.length > m_bodyMax) {
            fail(413, "request body too large");
            return;
        }
        consumeBody();
    }
    if (m_state == State::Body) consumeBody();
}

bool Connection::parseHead(const QByteArray& head)
{
    m_req = {};
    const QList<QByteArray> lines = head.split('\n');
    const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
    if (first.size() != 3 || !first[2].startsWith("HTTP/1.")) {
        fail(400, "bad request line");
        return false;
    }
    m_req.method = first[0];
    m_req.target = first[1];
    m_req.http10 = first[2] == "HTTP/1.0";
    if (lines.size() > kMaxHeaders) {
        fail(431, "too many headers");
        return false;
    }
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines[i].trimmed();
        if (line.isEmpty()) continue;
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0) {
            fail(400, "bad header line");
            return false;
        }
        const QByteArray name = line.left(colon).trimmed().toLower();
        const QByteArray value = line.mid(colon + 1).trimmed();
        if (name == "content-length" && m_req.headers.contains(name) && m_req.headers[name] != value) {
            fail(400, "conflicting Content-Length");
            return false;
        }
        m_req.headers[name] = m_req.headers.contains(name) && name != "content-length"
                                  ? m_req.headers[name] + ", " + value
                                  : value;
    }
    if (m_req.headers.contains("transfer-encoding")) {
        fail(411, "chunked request bodies are not supported: send Content-Length");
        return false;
    }
    bool ok = true;
    if (m_req.headers.contains("content-length")) m_req.length = m_req.headers["content-length"].toLongLong(&ok);
    if (!ok || m_req.length < 0) {
        fail(400, "bad Content-Length");
        return false;
    }
    // Absolute form ("http://host/path", proxies) -> path
    QByteArray target = m_req.target;
    if (target.startsWith("http://") || target.startsWith("https://")) {
        const qsizetype slash = target.indexOf('/', target.indexOf("//") + 2);
        target = slash < 0 ? QByteArray("/") : target.mid(slash);
    }
    if (!target.startsWith('/')) {
        fail(400, "bad request target");
        return false;
    }
    const qsizetype q = target.indexOf('?');
    m_req.query = q < 0 ? QByteArray() : target.mid(q + 1);
    m_req.path = QUrl::fromPercentEncoding(q < 0 ? target : target.left(q));
    if (m_req.path.contains(QChar(0))) {
        fail(400, "bad request target");
        return false;
    }
    const QByteArray connection = m_req.header("connection").toLower();
    m_keepAlive = m_req.http10 ? connection.contains("keep-alive") : !connection.contains("close");
    return true;
}

void Connection::bufferBody(qint64 max)
{
    m_bodyMax = max;
    m_sink = nullptr;
}

void Connection::streamBody(QIODevice* sink)
{
    m_sink = sink;
    m_bodyMax = -1;
}

void Connection::sendContinue()
{
    if (m_state == State::Body && !m_req.http10 && m_req.header("expect").toLower() == "100-continue")
        m_socket->write("HTTP/1.1 100 Continue\r\n\r\n");
}

void Connection::consumeBody()
{
    if (m_state != State::Body) return;
    const QByteArray piece = m_buf.left(m_req.length - m_received);
    m_buf.remove(0, piece.size());
    if (!piece.isEmpty() && !m_discard) {
        if (m_sink) {
            if (m_sink->write(piece) != piece.size()) {
                m_received += piece.size();
                fail(500, "cannot write the file (disk full?)");
                return;
            }
        } else {
            m_req.body += piece;
        }
        m_received += piece.size();
    } else {
        m_received += piece.size();
    }
    if (m_received < m_req.length) {
        armTimer();
        return;
    }
    if (m_discard) { // the rest of a body that was already answered: on to the next request
        m_discard = false;
        m_req = {};
        m_state = State::Head;
        armTimer();
        process();
        return;
    }
    m_state = State::Handling;
    armTimer();
    emit requestReceived(this);
}

void Connection::writeHead(int status, const Headers& headers)
{
    // Answered before the body is in (an error): a small rest is read and dropped (clients that send the whole
    // body first see the answer then), for a big one the connection closes after the answer
    if (m_state == State::Body && m_received < m_req.length) {
        // A client waiting for "100 Continue" sends no body after this answer: dropping would swallow its next request
        const bool waits = m_received == 0 && m_buf.isEmpty() && m_req.header("expect").toLower() == "100-continue";
        if (m_keepAlive && !waits && m_req.length - m_received <= kDiscardMax) m_discard = true;
        else m_keepAlive = false;
    }
    QByteArray h = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason(status) + "\r\n";
    for (const auto& [name, value] : headers) h += name + ": " + value + "\r\n";
    h += m_keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    h += "\r\n";
    m_socket->write(h);
}

void Connection::respond(int status, const QByteArray& type, const QByteArray& body, const Headers& extra)
{
    if (m_state == State::Closed || answered()) return;
    Headers h = extra;
    if (!type.isEmpty()) h.prepend({"Content-Type", type});
    h << QPair<QByteArray, QByteArray>{"Content-Length", QByteArray::number(body.size())};
    writeHead(status, h);
    if (m_req.method != "HEAD") m_socket->write(body);
    m_state = State::Answering;
    finishAnswer();
}

void Connection::respondJson(int status, const QJsonObject& o, const Headers& extra)
{
    respond(status, "application/json", QJsonDocument(o).toJson(QJsonDocument::Compact), extra);
}

void Connection::beginStream(int status, const QByteArray& type, const Headers& extra)
{
    if (m_state == State::Closed || answered()) return;
    Headers h = extra;
    h.prepend({"Content-Type", type});
    h << QPair<QByteArray, QByteArray>{"Cache-Control", "no-cache"};
    // HTTP/1.0 has no chunks: the end of the connection ends the stream
    m_chunked = !m_req.http10;
    if (m_chunked) h << QPair<QByteArray, QByteArray>{"Transfer-Encoding", "chunked"};
    else m_keepAlive = false;
    writeHead(status, h);
    m_state = State::Streaming;
}

void Connection::streamData(const QByteArray& data)
{
    if (m_state != State::Streaming || data.isEmpty()) return;
    if (m_chunked) m_socket->write(QByteArray::number(data.size(), 16) + "\r\n" + data + "\r\n");
    else m_socket->write(data);
}

void Connection::endStream()
{
    if (m_state != State::Streaming) return;
    if (m_chunked) m_socket->write("0\r\n\r\n");
    m_state = State::Answering;
    finishAnswer();
}

void Connection::sendFile(int status, std::unique_ptr<QFile> file, qint64 from, qint64 length, const QByteArray& type,
                          const Headers& extra)
{
    if (m_state == State::Closed || answered()) return;
    Headers h = extra;
    h.prepend({"Content-Type", type});
    h << QPair<QByteArray, QByteArray>{"Content-Length", QByteArray::number(length)};
    writeHead(status, h);
    m_state = State::Answering;
    if (m_req.method == "HEAD" || length == 0 || !file->seek(from)) {
        finishAnswer();
        return;
    }
    m_file = std::move(file);
    m_fileLeft = length;
    pumpFile();
}

void Connection::pumpFile()
{
    while (m_file && m_fileLeft > 0 && m_socket->bytesToWrite() < 2 * kFilePiece) {
        const QByteArray piece = m_file->read(std::min(kFilePiece, m_fileLeft));
        if (piece.isEmpty()) { // file shrank or broke: the length promised cannot be kept
            m_file.reset();
            m_socket->abort();
            return;
        }
        m_fileLeft -= piece.size();
        m_socket->write(piece);
    }
    if (m_file && m_fileLeft == 0) {
        m_file.reset();
        finishAnswer();
    }
}

void Connection::finishAnswer()
{
    if (m_file) return; // still sending
    if (!m_keepAlive) {
        m_timer->start(kStalledMs);     // a client that does not read its answer
        m_socket->disconnectFromHost(); // after the answer is out; disconnected() -> close()
        if (m_socket->state() == QAbstractSocket::UnconnectedState) close();
        return;
    }
    if (m_discard) {
        m_state = State::Body;
        m_sink = nullptr;
        armTimer();
        // Dropped from the event loop, not here inside the answer: with pipelined requests that are all answered
        // early (401 without a token), each would otherwise go one call deeper until the stack overflows
        QTimer::singleShot(0, this, [this] {
            if (m_state != State::Body || !m_discard) return;
            m_buf += m_socket->readAll();
            consumeBody();
        });
        return;
    }
    m_req = {};
    m_state = State::Head;
    armTimer();
    // The next request may already be here (pipelining)
    QTimer::singleShot(0, this, [this] {
        if (m_state != State::Head) return;
        m_buf += m_socket->readAll();
        process();
    });
}

void Connection::fail(int status, const QByteArray& message)
{
    m_keepAlive = false;
    if (answered() || m_state == State::Closed) {
        close();
        return;
    }
    respondJson(status, {{"ok", false}, {"error", QJsonObject{{"code", QString::number(status)},
                                                              {"message", QString::fromUtf8(message)}}}});
}

void Connection::armTimer()
{
    switch (m_state) {
    case State::Head: m_timer->start(m_buf.isEmpty() ? kIdleMs : kStalledMs); break;
    case State::Body: m_timer->start(kStalledMs); break;
    default: m_timer->stop(); break; // the server decides how long an answer takes
    }
}

void Connection::close()
{
    if (m_state == State::Closed) return;
    m_state = State::Closed;
    m_timer->stop();
    m_file.reset();
    m_socket->abort();
    emit closed(this);
    deleteLater();
}

} // namespace Cli::Http
