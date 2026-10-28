// SPDX-License-Identifier: AGPL-3.0-only
// HTTP side of `schneidi-cli serve` (cli/Http.h) without a network: a Cli::Http::Connection on a socket that is only a
// buffer (fuzz/FakeSocket.h). Pipelined requests, early answers whose bodies are dropped, thousands of them at once
// (fuzzing found the stack overflowing), and what the server reads from a request (Range, Host).
#include "check.h"

#include "cli/Http.h"
#include "cli/ServeDetail.h"
#include "fuzz/FakeSocket.h"

#include <QCoreApplication>
#include <QPointer>
#include <tuple>

namespace {

using namespace Cli;

// One connection; `early` answers every request as soon as its head is in (like 401 without a token), else the
// answer comes once the body is in
struct Client {
    FakeSocket* socket = new FakeSocket;
    QPointer<Http::Connection> c = new Http::Connection(socket);
    int heads = 0, requests = 0;
    QByteArrayList bodies;

    explicit Client(bool early)
    {
        QObject::connect(c, &Http::Connection::headReceived, [this, early](Http::Connection* x) {
            ++heads;
            if (early) x->respond(404, "text/plain", "no");
        });
        QObject::connect(c, &Http::Connection::requestReceived, [this](Http::Connection* x) {
            ++requests;
            bodies << x->request().body;
            x->respond(200, "text/plain", x->request().body);
        });
    }
    ~Client()
    {
        delete c.data();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void send(const QByteArray& bytes)
    {
        socket->feed(bytes);
        settle([this] { return std::make_tuple(heads, requests, c ? socket->out.size() : -1); });
    }
    qsizetype answers(const QByteArray& status) const { return c ? socket->out.count("HTTP/1.1 " + status) : -1; }
};

void testPipelining()
{
    // Requests one after another on a connection, sent at once: each answered, in order
    Client a(false);
    a.send("POST /a HTTP/1.1\r\nContent-Length: 3\r\n\r\nabcGET /b HTTP/1.1\r\n\r\nPOST /c HTTP/1.1\r\nContent-Length: 2\r\n\r\nxy");
    CHECK_EQ(a.requests, 3);
    CHECK_EQ(a.bodies, QByteArrayList({"abc", "", "xy"}));
    CHECK_EQ(a.answers("200"), qsizetype(3));
    CHECK(a.c && a.socket->out.endsWith("xy"));

    // Answered before the body: the body is read and dropped, the next request still comes
    Client b(true);
    b.send("PUT /a HTTP/1.1\r\nContent-Length: 5\r\n\r\nabcdeGET /b HTTP/1.1\r\n\r\n");
    CHECK_EQ(b.heads, 2);
    CHECK_EQ(b.requests, 0);
    CHECK_EQ(b.answers("404"), qsizetype(2));
    CHECK(b.c);

    // Thousands of such requests at once (1.2 MB, as fast as a client can send them, no token needed for the
    // answer): each body used to be dropped inside the answer before, one call deeper per request, until the
    // stack overflowed and the server crashed
    Client deep(true);
    QByteArray many;
    for (int i = 0; i < 30000; ++i) many += "GET /x HTTP/1.1\r\nContent-Length: 1\r\n\r\nx";
    deep.send(many);
    CHECK_EQ(deep.heads, 30000);
    CHECK_EQ(deep.answers("404"), qsizetype(30000));
    CHECK(deep.c);

    // A body too big to drop: the connection closes after the answer
    Client big(true);
    big.send("PUT /a HTTP/1.1\r\nContent-Length: 100000000\r\n\r\nabc");
    CHECK_EQ(big.heads, 1);
    CHECK(!big.c || !big.c->isOpen());
}

void testRequestParts()
{
    using Serve::ByteRange;
    const auto range = [](const QByteArray& header, qint64 size) {
        const ByteRange r = Serve::byteRange(header, size);
        if (r.kind == ByteRange::Whole) return QString("whole");
        if (r.kind == ByteRange::Unsatisfiable) return QString("416");
        return QString("%1-%2").arg(r.from).arg(r.to);
    };
    CHECK_EQ(range("", 100), QString("whole"));
    CHECK_EQ(range("bytes=0-9", 100), QString("0-9"));
    CHECK_EQ(range(" bytes=95-200 ", 100), QString("95-99"));
    CHECK_EQ(range("bytes=90-", 100), QString("90-99"));
    CHECK_EQ(range("bytes=-10", 100), QString("90-99"));
    CHECK_EQ(range("bytes=-1000", 100), QString("0-99"));
    CHECK_EQ(range("bytes=0-1,5-6", 100), QString("whole"));
    CHECK_EQ(range("bytes=100-", 100), QString("416"));
    CHECK_EQ(range("bytes=5-3", 100), QString("416"));
    CHECK_EQ(range("bytes=-0", 100), QString("416"));
    CHECK_EQ(range("bytes=x-", 100), QString("416"));
    CHECK_EQ(range("bytes=0-", 0), QString("416"));
    CHECK_EQ(range("bytes=99999999999999999999-", 100), QString("416"));

    CHECK_EQ(Serve::hostOf("localhost:8642"), QString("localhost"));
    CHECK_EQ(Serve::hostOf("[::1]:8642"), QString("[::1]"));
    CHECK_EQ(Serve::hostOf("[::1"), QString(""));
    CHECK(Serve::loopbackName("LOCALHOST") && Serve::loopbackName("app.localhost") && Serve::loopbackName("[::1]")
          && Serve::loopbackName("127.0.0.2"));
    CHECK(!Serve::loopbackName("example.com") && !Serve::loopbackName("") && !Serve::loopbackName("192.168.1.1"));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("http");
    testPipelining();
    testRequestParts();
    return Check::result();
}
