#pragma once
// Small HTTP/1.1 server side for `schneidi-cli serve` (Serve.cpp) on a QTcpSocket, no extra Qt module: requests with
// Content-Length bodies (buffered, or streamed into a file as they arrive), keep-alive, answers with a length,
// chunked streams (Server-Sent Events) and files (with a byte range). No TLS (a reverse proxy does that), no chunked
// request bodies (411).

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>

#include <memory>

class QFile;
class QIODevice;
class QTcpSocket;
class QTimer;

namespace Cli::Http {

using Headers = QList<QPair<QByteArray, QByteArray>>;

struct Request {
    QByteArray method;   // "GET", "POST" …
    QByteArray target;   // as sent: "/files/a%20b.mp4?x=1"
    QString path;        // decoded, without the query: "/files/a b.mp4"
    QByteArray query;
    bool http10 = false;
    QHash<QByteArray, QByteArray> headers; // names in lower case
    qint64 length = 0;   // Content-Length
    QByteArray body;     // bufferBody()

    QByteArray header(const QByteArray& lowerName) const { return headers.value(lowerName); }
};

QByteArray reason(int status);

class Connection : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxHead = 64 * 1024;

    explicit Connection(QTcpSocket* socket, QObject* parent = nullptr);
    ~Connection() override;

    const Request& request() const { return m_req; }
    QString peer() const;

    // In headReceived(): what happens to the body. Nothing chosen = buffered up to 1 MB.
    void bufferBody(qint64 max);      // into request().body; more -> 413
    void streamBody(QIODevice* sink); // written into `sink` as it arrives (owned by the caller, kept until requestReceived)
    void sendContinue();              // "100 Continue" for clients that wait for it before a big body (curl)

    // One answer per request, then the next request on the connection is read. Without a body read yet the
    // connection closes after the answer.
    void respond(int status, const QByteArray& type, const QByteArray& body, const Headers& extra = {});
    void respondJson(int status, const QJsonObject& o, const Headers& extra = {});
    void beginStream(int status, const QByteArray& type, const Headers& extra = {}); // chunked
    void streamData(const QByteArray& data);
    void endStream();
    // `length` bytes from `from` (status 200 or 206), read as the socket takes them
    void sendFile(int status, std::unique_ptr<QFile> file, qint64 from, qint64 length, const QByteArray& type,
                  const Headers& extra = {});
    bool answered() const { return m_state == State::Answering || m_state == State::Streaming; }
    bool isOpen() const;

signals:
    void headReceived(Cli::Http::Connection* c);    // request line and headers read: choose the body (or answer)
    void requestReceived(Cli::Http::Connection* c); // body complete: answer (now or later)
    void closed(Cli::Http::Connection* c);          // gone; deleted later

private:
    enum class State { Head, Body, Handling, Answering, Streaming, Closed };
    void onReadyRead();
    void process();
    bool parseHead(const QByteArray& head);
    void consumeBody();
    void fail(int status, const QByteArray& message); // answer an error and close
    void writeHead(int status, const Headers& headers);
    void finishAnswer();
    void pumpFile();
    void close();
    void armTimer();

    QTcpSocket* m_socket;
    QTimer* m_timer;
    QByteArray m_buf;
    Request m_req;
    State m_state = State::Head;
    qint64 m_received = 0;
    qint64 m_bodyMax = -1;   // -1 = not chosen
    QIODevice* m_sink = nullptr;
    bool m_bodyChosen = false;
    bool m_keepAlive = true;
    bool m_chunked = false;
    bool m_discard = false; // reading the rest of a body that was answered early
    std::unique_ptr<QFile> m_file;
    qint64 m_fileLeft = 0;
};

} // namespace Cli::Http
