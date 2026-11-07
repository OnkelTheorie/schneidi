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
    std::shared_lock lock(mutex());
    return trusted().contains(code);
}

void trust(const QString& code)
{
    if (code.trimmed().isEmpty()) return;
    std::unique_lock lock(mutex());
    trusted().insert(code);
}

void trust(const QStringList& codes)
{
    std::unique_lock lock(mutex());
    for (const QString& c : codes)
        if (!c.trimmed().isEmpty()) trusted().insert(c);
}

bool allTrusted(const QStringList& codes)
{
    return std::all_of(codes.cbegin(), codes.cend(), [](const QString& c) { return isTrusted(c); });
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
    return allTrusted(codes);
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
}

} // namespace CodeTrust
