// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Small pictures for the Direct page's lists (material clips, suggestions): decoded on an own pool of at most two
// low-priority threads (never the global pool: the threads are lowered for good), each request belongs to a
// generation of its list - a request of an older generation is skipped before it decodes and its result dropped.

#include <QImage>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QThreadPool>
#include <atomic>
#include <functional>
#include <utility>
#include <memory>

class DirectThumbs : public QObject {
    Q_OBJECT
public:
    using Decode = std::function<QImage(const QString& file, double seconds, const QSize& size)>;
    explicit DirectThumbs(QObject* parent = nullptr);
    ~DirectThumbs() override;

    void setDecoder(Decode decode) { m_decode = std::move(decode); }
    // A key that is no file ("@current", "@timeline:<id>": a time of a timeline) as the file shown there and its
    // time in it; called in the UI thread when asked (MainWindow reads its project). Empty file = no picture.
    using Resolve = std::function<std::pair<QString, double>(const QString& key, double seconds)>;
    void setResolver(Resolve resolve) { m_resolve = std::move(resolve); }
    bool hasDecoder() const { return bool(m_decode); }

    // A list's generation counter: next() starts a new one (older requests are skipped)
    struct Generation {
        std::shared_ptr<std::atomic<int>> value = std::make_shared<std::atomic<int>>(0);
        int next() { return ++*value; }
        int now() const { return value->load(); }
    };

    // The picture of `file` at `seconds` for `context` (done runs in its thread, not after it is gone or the
    // generation moved on); a key of the resolver in place of a file
    void request(const QString& file, double seconds, const QSize& size, const Generation& gen, QObject* context,
                 std::function<void(const QImage&)> done);
    void waitForDone() { m_pool.waitForDone(); } // tests, the destructor

private:
    QThreadPool m_pool;
    Decode m_decode;
    Resolve m_resolve;
};
