// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/FileStamp.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

namespace FileStamp {

namespace {

using Clock = std::chrono::steady_clock;

struct Entry {
    Stamp stamp; // confirmed: what readers compare with
    Clock::time_point checked;
    bool changing = false;       // the disk shows another stamp that has not settled yet
    Stamp next;                  // that stamp
    Clock::time_point nextSince; // first seen then
};

struct Observer {
    std::function<void(const QString&)> fn;
    std::recursive_mutex call; // held while it runs: removeObserver waits for a running call (its own thread may enter)
    bool alive = true;
};

struct State {
    std::mutex mutex;
    QHash<QString, Entry> entries;
    std::mutex observerMutex;
    QMap<int, std::shared_ptr<Observer>> observers;
    int nextId = 1;
    std::atomic<int> settleMs{kSettleMs};
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

// Called with no FileStamp lock held (the list is copied first)
void notify(const QString& path)
{
    std::vector<std::shared_ptr<Observer>> list;
    {
        std::lock_guard lock(state().observerMutex);
        for (const auto& o : std::as_const(state().observers)) list.push_back(o);
    }
    for (const auto& o : list) {
        std::lock_guard call(o->call);
        if (o->alive) o->fn(path);
    }
}

// Takes the disk's `stamp` for `path`; true = the confirmed stamp changed now (a change that has settled)
bool store(const QString& path, const Stamp& stamp, Clock::time_point at)
{
    std::lock_guard lock(state().mutex);
    auto it = state().entries.find(path);
    if (it == state().entries.end()) {
        state().entries.insert(path, {stamp, at});
        return false;
    }
    it->checked = at;
    if (stamp == it->stamp) { // as it was (or back to it)
        it->changing = false;
        return false;
    }
    if (!it->changing || !(it->next == stamp)) { // a new change, or still growing: wait until it holds still
        it->changing = true;
        it->next = stamp;
        it->nextSince = at;
        return false;
    }
    if (at - it->nextSince < std::chrono::milliseconds(state().settleMs.load())) return false;
    it->stamp = stamp;
    it->changing = false;
    return true;
}

} // namespace

Stamp now(const QString& path)
{
    if (store(path, read(path), Clock::now())) notify(path);
    std::lock_guard lock(state().mutex);
    return state().entries.value(path).stamp;
}

Stamp recent(const QString& path)
{
    const auto t = Clock::now();
    {
        std::lock_guard lock(state().mutex);
        const auto it = state().entries.constFind(path);
        if (it != state().entries.cend() && t - it->checked < std::chrono::milliseconds(kRecheckMs)) return it->stamp;
    }
    return now(path);
}

bool changing(const QString& path)
{
    std::lock_guard lock(state().mutex);
    return state().entries.value(path).changing;
}

int addObserver(std::function<void(const QString&)> fn)
{
    auto o = std::make_shared<Observer>();
    o->fn = std::move(fn);
    std::lock_guard lock(state().observerMutex);
    const int id = state().nextId++;
    state().observers.insert(id, std::move(o));
    return id;
}

void removeObserver(int id)
{
    std::shared_ptr<Observer> o;
    {
        std::lock_guard lock(state().observerMutex);
        o = state().observers.take(id);
    }
    if (!o) return;
    std::lock_guard call(o->call); // a call running on another thread ends first
    o->alive = false;
}

void forget()
{
    std::lock_guard lock(state().mutex);
    state().entries.clear();
}

void setSettleMsForTest(int ms) { state().settleMs = ms; }

} // namespace FileStamp
