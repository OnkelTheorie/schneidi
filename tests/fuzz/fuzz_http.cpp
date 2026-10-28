// SPDX-License-Identifier: AGPL-3.0-only
// Fuzz target: requests to `schneidi-cli serve` (cli/Http.h, cli/ServeDetail.h) without a network. What a client
// sends goes into a real Http::Connection through a socket that is only a buffer; a small server answers like
// Serve.cpp does (buffered bodies for /mcp, PUT streamed into a buffer, files with a byte range, early errors), and
// every request goes through the server's own checks: work folder and hidden files (path traversal), Range, Host.
//
// Input: byte 0 = how the bytes arrive (at once or in pieces of 1..128), byte 1 = how the server answers, the rest
// = the bytes of the client (several requests on one connection, pipelined or not).
#include "FakeSocket.h"
#include "FuzzCommon.h"

#include "cli/Commands.h"
#include "cli/Http.h"
#include "cli/Sandbox.h"
#include "cli/ServeDetail.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include <memory>
#include <tuple>
#include <vector>

namespace {

using namespace Cli;

Fuzz::WorkFolder& world()
{
    static Fuzz::WorkFolder w;
    return w;
}

// A file path the server accepted for /files: inside the work folder (also through links) and not hidden
void checkServedPath(const QString& path)
{
    world().checkInside(path);
    const QString rel = Sandbox::relative(path);
    FUZZ_ASSERT(!rel.isNull());
    for (const QString& part : rel.split('/')) FUZZ_ASSERT(!part.startsWith('.'));
}

// What the server reads from every head (Serve.cpp onHead / filesHead / filesGet)
bool checkHead(const Http::Request& r)
{
    (void)Serve::loopbackName(Serve::hostOf(r.header("host")));
    const QByteArray origin = r.header("origin");
    if (!origin.isEmpty()) (void)Serve::loopbackName(QUrl(QString::fromLatin1(origin)).host());
    for (const qint64 size : {qint64(0), qint64(1), qint64(1000), qint64(1) << 40}) {
        const Serve::ByteRange br = Serve::byteRange(r.header("range"), size);
        if (br.kind == Serve::ByteRange::Part) {
            FUZZ_ASSERT(br.from >= 0 && br.from <= br.to && br.to < size);
        }
    }
    if (r.path != "/files" && !r.path.startsWith("/files/")) return true;
    try {
        checkServedPath(Serve::filePath(r.path));
    } catch (const Error&) {
        return false;
    }
    return true;
}

struct Server {
    int heads = 0, requests = 0;
    uint8_t mode = 0;
    std::vector<std::unique_ptr<QBuffer>> sinks; // PUT bodies (kept until the connection is gone)
    QBuffer* sink = nullptr;

    void onHead(Http::Connection* c)
    {
        ++heads;
        sink = nullptr;
        const Http::Request& r = c->request();
        const bool allowed = checkHead(r);
        if (mode & 0x10 && heads % 3 == 0) { // an early error, before the body (the rest is read and dropped)
            c->respondJson(403, Serve::errorJson("FORBIDDEN", "no"));
            return;
        }
        if (r.path == "/mcp") {
            if (r.method == "POST") c->bufferBody(mode & 0x20 ? 64 : 16 << 20);
            return;
        }
        if (r.path == "/files" || r.path.startsWith("/files/")) {
            if (!allowed) {
                c->respondJson(403, Serve::errorJson("OUTSIDE_ROOT", "no"));
            } else if (r.method == "PUT") {
                auto buffer = std::make_unique<QBuffer>();
                buffer->open(QIODevice::WriteOnly);
                sink = buffer.get();
                sinks.push_back(std::move(buffer));
                c->streamBody(sink);
                c->sendContinue();
            } else if (r.method != "GET" && r.method != "HEAD") {
                c->respondJson(405, Serve::errorJson("METHOD", "GET, HEAD or PUT"));
            }
            return;
        }
        c->respondJson(404, Serve::errorJson("NOT_FOUND", "nothing here"));
    }

    void onRequest(Http::Connection* c)
    {
        ++requests;
        const Http::Request& r = c->request();
        FUZZ_ASSERT((sink ? sink->data().size() : r.body.size()) == r.length); // the whole body, nothing more
        switch ((mode >> 2) & 3) {
        case 0: c->respondJson(200, {{"ok", true}, {"path", r.path}}); break;
        case 1:
            c->beginStream(200, "text/event-stream");
            c->streamData("event: message\ndata: {}\n\n");
            c->streamData({});
            c->endStream();
            break;
        case 2: {
            auto file = std::make_unique<QFile>(QDir(world().root).filePath("clip.mp4"));
            if (!file->open(QIODevice::ReadOnly)) std::abort();
            const qint64 size = file->size();
            const Serve::ByteRange br = Serve::byteRange(r.header("range"), size);
            if (br.kind == Serve::ByteRange::Unsatisfiable) c->respondJson(416, Serve::errorJson("RANGE", "no"));
            else if (br.kind == Serve::ByteRange::Part)
                c->sendFile(206, std::move(file), br.from, br.to - br.from + 1, "video/mp4");
            else c->sendFile(200, std::move(file), 0, size, "video/mp4");
            break;
        }
        default: // answered later (a job)
            QTimer::singleShot(0, c, [c] { c->respond(200, "text/plain", "later"); });
            break;
        }
    }
};

} // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    Fuzz::init("http");
    Sandbox::configure({world().root, {"Z:\\Clips=sub", "/mnt/clips=../outside"}, false});
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    Fuzz::Choices in{data, size};
    const uint8_t arrive = in.take();
    Server server;
    server.mode = in.take();
    const QByteArray bytes(reinterpret_cast<const char*>(in.data), qsizetype(in.size));

    auto* socket = new FakeSocket;
    QPointer<Http::Connection> c = new Http::Connection(socket);
    QObject::connect(c, &Http::Connection::headReceived, [&server](Http::Connection* x) { server.onHead(x); });
    QObject::connect(c, &Http::Connection::requestReceived, [&server](Http::Connection* x) { server.onRequest(x); });
    const QPointer<FakeSocket> s = socket;

    const qsizetype piece = arrive & 1 ? bytes.size() : (arrive >> 1) + 1;
    for (qsizetype at = 0; at < bytes.size() && c && s; at += piece) {
        s->feed(bytes.mid(at, piece));
        QCoreApplication::processEvents();
    }
    settle([&] { return std::make_tuple(server.heads, server.requests, s ? s->out.size() : -1, bool(c)); }, 100000);

    if (s) {
        // Every answer starts with a status line
        FUZZ_ASSERT(s->out.isEmpty() || s->out.startsWith("HTTP/1.1 "));
        FUZZ_ASSERT(server.requests <= server.heads);
        s->hangUp();
    }
    delete c.data(); // still open (or closed and waiting for deleteLater)
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return 0;
}
