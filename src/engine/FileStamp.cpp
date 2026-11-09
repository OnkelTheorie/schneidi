// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/FileStamp.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <chrono>
#include <mutex>

namespace FileStamp {

namespace {

struct Entry {
    Stamp stamp;
    std::chrono::steady_clock::time_point checked;
};

struct State {
    std::mutex mutex;
    QHash<QString, Entry> entries;
    std::mutex observerMutex;
    QMap<int, std::function<void(const QString&)>> observers;
    int nextId = 1;
};

State& state()
{
    static State s;
    return s;
}

Stamp read(const QString& path)
{
    const QFileInfo fi(path);
    if (!fi.exists()) return {};
    return {fi.size(), fi.lastModified().toMSecsSinceEpoch()};
}

// Under the observer lock: removeObserver() (a cache's destructor) waits until a running call is done
void notify(const QString& path)
{
    std::lock_guard lock(state().observerMutex);
    for (const auto& fn : std::as_const(state().observers)) fn(path);
}

// Stores `stamp` for `path`; true = it differs from a stamp seen before
bool store(const QString& path, const Stamp& stamp, std::chrono::steady_clock::time_point at)
{
    std::lock_guard lock(state().mutex);
    auto it = state().entries.find(path);
    if (it == state().entries.end()) {
        state().entries.insert(path, {stamp, at});
        return false;
    }
    const bool changed = !(it->stamp == stamp);
    it->stamp = stamp;
    it->checked = at;
    return changed;
}

} // namespace

Stamp now(const QString& path)
{
    const Stamp s = read(path);
    if (store(path, s, std::chrono::steady_clock::now())) notify(path);
    return s;
}

Stamp recent(const QString& path)
{
    const auto t = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(state().mutex);
        const auto it = state().entries.constFind(path);
        if (it != state().entries.cend() && t - it->checked < std::chrono::milliseconds(kRecheckMs)) return it->stamp;
    }
    return now(path);
}

int addObserver(std::function<void(const QString&)> fn)
{
    std::lock_guard lock(state().observerMutex);
    const int id = state().nextId++;
    state().observers.insert(id, std::move(fn));
    return id;
}

void removeObserver(int id)
{
    std::lock_guard lock(state().observerMutex);
    state().observers.remove(id);
}

void forget()
{
    std::lock_guard lock(state().mutex);
    state().entries.clear();
}

} // namespace FileStamp
