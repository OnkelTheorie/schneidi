// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/MemoryRelease.h"

#include "core/SystemMemory.h"
#include "engine/picture/Renderer.h"

#include <QFile>
#include <QMutex>
#include <QThread>
#include <QVector>

#include <algorithm>

#ifdef __GLIBC__
#include <malloc.h>
#endif

namespace {

constexpr int kDelayMs = 4000;

std::atomic<int> g_releases{0};
std::atomic<int> g_pressure{-1}; // setPressureForTests
QMutex g_mutex;                 // guards g_idle
QVector<IdleRelease*> g_idle;   // living IdleRelease objects, newest last

} // namespace

namespace MemoryRelease {

bool allowed(const Activity& a)
{
    return !a.playing && !a.exporting && !a.caching;
}

void release()
{
    Picture::releasePools();
    trimHeap();
    ++g_releases;
}

void trimHeap()
{
#ifdef __GLIBC__
    // glibc raises its mmap threshold after the first freed frame buffer (several MB): from then on such buffers come
    // from the heap and stay in the process when freed, until malloc_trim hands the free pages back
    malloc_trim(0);
#endif
}

qint64 residentBytes()
{
#ifdef Q_OS_LINUX
    QFile f("/proc/self/status");
    if (!f.open(QIODevice::ReadOnly)) return -1;
    // readAll, not atEnd()/readLine(): /proc files report size 0, so atEnd() is true before the first line
    for (const QByteArray& line : f.readAll().split('\n'))
        if (line.startsWith("VmRSS:")) return line.mid(6).trimmed().split(' ').value(0).toLongLong() * 1024;
#endif
    return -1;
}

bool underPressure()
{
    if (const int forced = g_pressure.load(); forced >= 0) return forced == 1;
    const SystemMemory::Info m = SystemMemory::read(); // cgroup limits (container, systemd scope) count
    // In a cgroup with a limit (container, systemd scope) that limit is the whole: available is the room it leaves
    const qint64 total = m.limit > 0 && (m.total <= 0 || m.limit < m.total) ? m.limit : m.total;
    if (total <= 0 || m.available <= 0) return false;
    const qint64 keep = std::clamp<qint64>(total / 10, qint64(256) << 20, qint64(2) << 30);
    return m.available < keep;
}

void setPressureForTests(int state)
{
    g_pressure = state;
}

int releases()
{
    return g_releases;
}

void requestRelease()
{
    QMutexLocker lock(&g_mutex);
    if (!g_idle.isEmpty()) QMetaObject::invokeMethod(g_idle.last(), &IdleRelease::request, Qt::AutoConnection);
}

} // namespace MemoryRelease

IdleRelease::IdleRelease(std::function<MemoryRelease::Activity()> activity, QObject* parent)
    : QObject(parent), m_activity(std::move(activity))
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(kDelayMs);
    // SCHNEIDI_MEMORY_RELEASE_MS: another delay (tests, measurements), 0 = never give memory back
    if (qEnvironmentVariableIsSet("SCHNEIDI_MEMORY_RELEASE_MS")) {
        const int ms = qEnvironmentVariableIntValue("SCHNEIDI_MEMORY_RELEASE_MS");
        m_off = ms <= 0;
        if (ms > 0) m_timer.setInterval(ms);
    }
    connect(&m_timer, &QTimer::timeout, this, &IdleRelease::check);
    QMutexLocker lock(&g_mutex);
    g_idle << this;
}

IdleRelease::~IdleRelease()
{
    {
        QMutexLocker lock(&g_mutex);
        g_idle.removeAll(this);
    }
    if (m_worker) {
        m_worker->wait();
        delete m_worker;
    }
}

void IdleRelease::request()
{
    if (!m_off) m_timer.start();
}

void IdleRelease::check()
{
    const MemoryRelease::Activity now = m_activity ? m_activity() : MemoryRelease::Activity{};
    if (m_worker || !MemoryRelease::allowed(now)) {
        m_timer.start(); // something renders: once more after the wait
        return;
    }
    // Freeing GBs (munmap of every buffer, trimming the heap) takes a moment: not on the GUI thread
    m_before = MemoryRelease::residentBytes();
    emit releasing();
    m_worker = QThread::create([] { MemoryRelease::release(); });
    connect(m_worker, &QThread::finished, this, [this] {
        m_worker->wait();
        delete m_worker;
        m_worker = nullptr;
        const qint64 after = MemoryRelease::residentBytes();
        if (m_before > 0 && after > 0 && m_before - after >= 32 * 1048576) // only what is worth a line in the log
            qInfo("Memory given back while idle: %lld -> %lld MB resident", m_before / 1048576, after / 1048576);
        emit released(m_before, after);
    });
    m_worker->start(QThread::LowPriority);
}
