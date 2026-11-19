// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/direct/DirectThumbs.h"

#include "engine/Background.h"

#include <QThread>
#include <algorithm>

DirectThumbs::DirectThumbs(QObject* parent) : QObject(parent)
{
    m_pool.setMaxThreadCount(std::clamp(QThread::idealThreadCount() / 4, 1, 2));
    m_pool.setExpiryTimeout(-1); // the threads stay lowered: keep them instead of making new ones
}

DirectThumbs::~DirectThumbs()
{
    m_pool.clear();
    m_pool.waitForDone();
}

void DirectThumbs::request(const QString& file, double seconds, const QSize& size, const Generation& gen,
                           QObject* context, std::function<void(const QImage&)> done)
{
    if (!m_decode) return;
    const int mine = gen.now();
    const auto counter = gen.value;
    const Decode decode = m_decode;
    QPointer<QObject> ctx(context);
    m_pool.start([this, file, seconds, size, mine, counter, decode, ctx, done] {
        thread_local bool lowered = false;
        if (!lowered) {
            Background::lowerThisThread(Background::Level::Low);
            lowered = true;
        }
        if (counter->load() != mine) return; // the list moved on: not decoded at all
        const QImage img = decode(file, seconds, size);
        if (img.isNull() || counter->load() != mine) return;
        // Through this object (alive: its destructor waits for the pool), the context checked in the UI thread
        QMetaObject::invokeMethod(this, [ctx, counter, mine, img, done] {
            if (ctx && counter->load() == mine) done(img);
        });
    });
}
