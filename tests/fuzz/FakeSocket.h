#pragma once
// A connected TCP socket without a network for Cli::Http::Connection (fuzz_http, test_http): the bytes of the client
// come from feed(), what the server writes is collected in `out`.

#include <QCoreApplication>
#include <QTcpSocket>

#include <algorithm>
#include <cstring>

class FakeSocket : public QTcpSocket {
public:
    FakeSocket()
    {
        setSocketState(ConnectedState);
        setOpenMode(QIODevice::ReadWrite | QIODevice::Unbuffered);
    }
    void feed(const QByteArray& bytes)
    {
        m_in += bytes;
        emit readyRead();
    }
    void hangUp()
    {
        if (state() == UnconnectedState) return;
        setSocketState(UnconnectedState);
        emit disconnected();
    }
    qint64 bytesAvailable() const override { return m_in.size(); }
    qint64 bytesToWrite() const override { return 0; } // the client reads everything at once
    void disconnectFromHost() override { hangUp(); }
    QByteArray out;

protected:
    qint64 readData(char* data, qint64 max) override
    {
        const qint64 n = std::min<qint64>(max, m_in.size());
        std::memcpy(data, m_in.constData(), size_t(n));
        m_in.remove(0, n);
        return n;
    }
    qint64 writeData(const char* data, qint64 len) override
    {
        if (state() != ConnectedState) return -1;
        out.append(data, len);
        return len;
    }

private:
    QByteArray m_in;
};

// Lets the connection work through what it has (answers from the event loop, pipelined requests) until nothing
// changes any more; `progress` tells whether something happened
template <typename Progress>
void settle(Progress progress, int maxRounds = 1000000)
{
    auto last = progress();
    for (int idle = 0, round = 0; idle < 3 && round < maxRounds; ++round) {
        QCoreApplication::processEvents();
        const auto now = progress();
        idle = now == last ? idle + 1 : 0;
        last = now;
    }
}
