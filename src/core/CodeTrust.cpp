// SPDX-License-Identifier: AGPL-3.0-only
#include "core/CodeTrust.h"

#include "core/CompGraph.h"
#include "core/PathKey.h"
#include "core/ProjectFile.h"
#include "core/Types.h"

#include <QCryptographicHash>
#include <QSet>
#include <QSettings>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <shared_mutex>

namespace CodeTrust {

namespace {

// The effect ids and keys of engine/picture/CustomEffects.h (core does not include the engine)
constexpr const char* kFilterId = "ffmpeg_filter";
constexpr const char* kShaderId = "shader";

std::shared_mutex& mutex()
{
    static std::shared_mutex m;
    return m;
}
QSet<QString>& trusted()
{
    static QSet<QString> s;
    return s;
}
// Code seen untrusted in a file opened in this run (admit): never the caller's own, even once the project lost it
QSet<QString>& foreign()
{
    static QSet<QString> s;
    return s;
}

std::atomic<quint64> g_epoch{0};

// Stands in for untrusted code in a frozen timeline (freeze): never trusted, never written by anyone
const QString& frozenMarker()
{
    static const QString m = QStringLiteral("\x01untrusted custom code\x01");
    return m;
}

QString settingsKey(const QString& path)
{
    const QByteArray key = QCryptographicHash::hash(PathKey::of(path).toUtf8(), QCryptographicHash::Sha1).toHex();
    return QStringLiteral("trust/") + QString::fromLatin1(key);
}

} // namespace

QVector<Item> itemsOf(const EffectInstance& e)
{
    QVector<Item> out;
    const auto add = [&out](const QString& effect, const QVariant& v, int node) {
        const QString code = v.toString();
        if (!code.trimmed().isEmpty()) out << Item{effect, code, 0, 0, node};
    };
    if (e.effectId == QLatin1String(kFilterId)) add(e.effectId, e.params.value(QStringLiteral("chain")), 0);
    else if (e.effectId == QLatin1String(kShaderId)) add(e.effectId, e.params.value(QStringLiteral("code")), 0);
    else if (e.effectId == QLatin1String(Comp::kEffectId)) {
        // Node values "n<id>.e_chain" / "n<id>.e_code" (an fx node running a custom effect)
        for (auto it = e.params.cbegin(); it != e.params.cend(); ++it) {
            int id = 0;
            QString key;
            if (!CompGraph::splitParamKey(it.key(), &id, &key)) continue;
            if (key == QLatin1String("e_chain")) add(QString::fromLatin1(kFilterId), it.value(), id);
            else if (key == QLatin1String("e_code")) add(QString::fromLatin1(kShaderId), it.value(), id);
        }
    }
    return out;
}

QVector<Item> items(const ProjectData& d)
{
    QVector<Item> out;
    QSet<QString> seen; // the opened timeline is in `timeline` and in `sequences`
    const auto scan = [&](const Timeline& tl, int sequence) {
        for (const auto* tracks : {&tl.video, &tl.audio})
            for (const Track& t : *tracks)
                for (const Clip& c : t.clips)
                    for (const EffectInstance& e : c.effects)
                        for (Item it : itemsOf(e)) {
                            it.sequence = sequence;
                            it.clip = c.id;
                            const QString key = QStringLiteral("%1|%2|%3|%4").arg(sequence).arg(c.id).arg(it.node).arg(it.effect);
                            if (seen.contains(key)) continue;
                            seen << key;
                            out << it;
                        }
    };
    scan(d.timeline, d.currentSequence);
    for (const Sequence& s : d.sequences)
        if (s.id != d.currentSequence) scan(s.timeline, s.id);
    return out;
}

QStringList codes(const QVector<Item>& items)
{
    QStringList out;
    for (const Item& it : items) out << it.code;
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

QStringList codes(const ProjectData& d) { return codes(items(d)); }

bool isTrusted(const QString& code)
{
    if (code.trimmed().isEmpty()) return true;
    if (code == frozenMarker()) return false;
    std::shared_lock lock(mutex());
    return trusted().contains(code);
}

void trust(const QString& code)
{
    if (code.trimmed().isEmpty() || code == frozenMarker()) return;
    std::unique_lock lock(mutex());
    if (!trusted().contains(code)) ++g_epoch;
    trusted().insert(code);
    foreign().remove(code);
}

void trust(const QStringList& codes)
{
    std::unique_lock lock(mutex());
    for (const QString& c : codes)
        if (!c.trimmed().isEmpty() && c != frozenMarker()) {
            if (!trusted().contains(c)) ++g_epoch;
            trusted().insert(c);
            foreign().remove(c);
        }
}

quint64 epoch() { return g_epoch.load(); }

namespace {

bool freezeClips(Timeline& tl)
{
    bool changed = false;
    for (auto* tracks : {&tl.video, &tl.audio})
        for (Track& t : *tracks)
            for (Clip& c : t.clips)
                for (EffectInstance& e : c.effects)
                    for (const Item& it : itemsOf(e)) {
                        if (isTrusted(it.code)) continue;
                        const QString key = it.effect == QLatin1String(kShaderId) ? QStringLiteral("code") : QStringLiteral("chain");
                        e.params[it.node ? CompGraph::paramKey(it.node, QLatin1String("e_") + key) : key] = frozenMarker();
                        changed = true;
                    }
    return changed;
}

} // namespace

bool freeze(Timeline& tl)
{
    bool changed = freezeClips(tl);
    if (tl.nested) {
        NestedTimelines nested = *tl.nested;
        bool any = false;
        for (Timeline& n : nested) any |= freezeClips(n);
        if (any) tl.nested = std::make_shared<const NestedTimelines>(std::move(nested));
        changed |= any;
    }
    return changed;
}

bool allTrusted(const QStringList& codes)
{
    return std::all_of(codes.cbegin(), codes.cend(), [](const QString& c) { return isTrusted(c); });
}

bool trustWritten(const QString& code, const ProjectData& before)
{
    if (isTrusted(code)) return true;
    {
        std::shared_lock lock(mutex());
        if (foreign().contains(code)) return false;
    }
    for (const Item& it : items(before))
        if (it.code == code) return false; // someone else's code sent back unchanged
    trust(code);
    return true;
}

QVector<Item> untrusted(const ProjectData& d)
{
    QVector<Item> out;
    for (const Item& it : items(d))
        if (!isTrusted(it.code)) out << it;
    return out;
}

QString hashOf(const QStringList& codes)
{
    QStringList sorted = codes;
    std::sort(sorted.begin(), sorted.end());
    QCryptographicHash h(QCryptographicHash::Sha1);
    for (const QString& c : sorted) {
        const QByteArray b = c.toUtf8();
        h.addData(QByteArray::number(b.size()) + ':'); // length first: no two lists give the same bytes
        h.addData(b);
    }
    return QString::fromLatin1(h.result().toHex());
}

bool remembered(const QString& path, const QStringList& codes)
{
    if (path.isEmpty() || codes.isEmpty()) return false;
    return QSettings().value(settingsKey(path)).toString() == hashOf(codes);
}

void remember(const QString& path, const QStringList& codes)
{
    if (path.isEmpty() || codes.isEmpty()) return;
    QSettings().setValue(settingsKey(path), hashOf(codes));
}

bool admit(const QString& path, const QStringList& codes)
{
    if (codes.isEmpty()) return true;
    if (remembered(path, codes)) {
        trust(codes);
        return true;
    }
    std::unique_lock lock(mutex());
    bool all = true;
    for (const QString& c : codes)
        if (!c.trimmed().isEmpty() && !trusted().contains(c)) {
            foreign().insert(c);
            all = false;
        }
    return all;
}

void trustFile(const QString& path, const QStringList& codes)
{
    trust(codes);
    remember(path, codes);
}

void resetForTests()
{
    std::unique_lock lock(mutex());
    trusted().clear();
    foreign().clear();
}

} // namespace CodeTrust
