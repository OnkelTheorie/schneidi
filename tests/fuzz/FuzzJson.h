// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Structure-aware mutation for targets that read JSON (project files, OTIO, MCP messages): byte changes mostly break
// the syntax and end in the JSON reader, so most of the time one value of the document is changed instead - numbers
// to edge values, keys removed or copied from elsewhere in the document, array elements dropped or repeated, types
// swapped - and the document written again. Only in libFuzzer builds (SCHNEIDI_LIBFUZZER); the replay runs without it.

#ifdef SCHNEIDI_LIBFUZZER

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>

#include <cmath>
#include <cstring>
#include <random>

extern "C" size_t LLVMFuzzerMutate(uint8_t* data, size_t size, size_t maxSize);

namespace Fuzz::Json {

struct Pools {
    QStringList keys;
    QVector<QJsonValue> values; // scalars and small containers found in the document
};

inline void collect(const QJsonValue& v, Pools& p)
{
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (!p.keys.contains(it.key())) p.keys << it.key();
            collect(it.value(), p);
        }
        if (o.size() < 8) p.values << v;
    } else if (v.isArray()) {
        for (const QJsonValue& x : v.toArray()) collect(x, p);
        if (v.toArray().size() < 8) p.values << v;
    } else {
        p.values << v;
    }
}

inline QJsonValue edgeNumber(std::minstd_rand& rng, double now)
{
    static const double edges[] = {0, -1, 1, 2, 0.5, -0.5, 100, 1e-9, 1e9, 2e9, -2e9, 2147483647, -2147483648.0,
                                   2147483648.0, 4294967296.0, 9007199254740993.0, 1e18, 1e308, -1e308, 1e-300};
    switch (rng() % 5) {
    case 0: return now + 1;
    case 1: return now - 1;
    case 2: return -now;
    case 3: return now * double(1 + rng() % 1000);
    default: return edges[rng() % std::size(edges)];
    }
}

inline QJsonValue newValue(std::minstd_rand& rng, const Pools& p, const QJsonValue& now)
{
    switch (rng() % 8) {
    case 0: return QJsonValue::Null;
    case 1: return now.isDouble() ? edgeNumber(rng, now.toDouble()) : edgeNumber(rng, 1);
    case 2: return now.isBool() ? QJsonValue(!now.toBool()) : QJsonValue(bool(rng() % 2));
    case 3: {
        static const char* strings[] = {"", "..", "../..", "/", "a/../../b", "@app", "-1", "1e9", "NaN", "\\", "C:\\x",
                                        "file:///x", "%", "00:00:00:00", "99:99:99:99", "4.5s", "#ff0000", "x"};
        if (now.isString() && rng() % 2) return now.toString().repeated(int(1 + rng() % 64));
        return QString::fromUtf8(strings[rng() % std::size(strings)]);
    }
    case 4: return QJsonObject{};
    case 5: return QJsonArray{};
    default: return p.values.isEmpty() ? QJsonValue(0) : p.values[int(rng() % p.values.size())];
    }
}

inline QJsonValue mutate(const QJsonValue& v, std::minstd_rand& rng, const Pools& p, int depth = 0)
{
    // Down into a child most of the time, so that deep values change as often as top-level ones
    if (v.isObject() && !v.toObject().isEmpty() && rng() % 5 != 0 && depth < 64) {
        QJsonObject o = v.toObject();
        const QString key = o.keys()[int(rng() % o.size())];
        o[key] = mutate(o[key], rng, p, depth + 1);
        return o;
    }
    if (v.isArray() && !v.toArray().isEmpty() && rng() % 5 != 0 && depth < 64) {
        QJsonArray a = v.toArray();
        const int i = int(rng() % a.size());
        a[i] = mutate(a[i], rng, p, depth + 1);
        return a;
    }
    if (v.isObject()) {
        QJsonObject o = v.toObject();
        if (!o.isEmpty() && rng() % 2) o.remove(o.keys()[int(rng() % o.size())]);
        else if (!p.keys.isEmpty()) o[p.keys[int(rng() % p.keys.size())]] = newValue(rng, p, QJsonValue::Null);
        return o;
    }
    if (v.isArray()) {
        QJsonArray a = v.toArray();
        switch (a.isEmpty() ? 3 : rng() % 4) {
        case 0: a.removeAt(int(rng() % a.size())); break;
        case 1: a.append(a[int(rng() % a.size())]); break;
        case 2: {
            const int i = int(rng() % a.size()), j = int(rng() % a.size());
            const QJsonValue x = a[i];
            a[i] = a[j];
            a[j] = x;
            break;
        }
        default: a.append(newValue(rng, p, QJsonValue::Null)); break;
        }
        return a;
    }
    return newValue(rng, p, v);
}

// LLVMFuzzerCustomMutator for a JSON target
inline size_t mutateJson(uint8_t* data, size_t size, size_t maxSize, unsigned seed)
{
    std::minstd_rand rng(seed);
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromRawData(reinterpret_cast<const char*>(data), qsizetype(size)));
    if (doc.isNull() || rng() % 5 == 0) return LLVMFuzzerMutate(data, size, maxSize);
    const QJsonValue root = doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object());
    Pools pools;
    collect(root, pools);
    QJsonValue changed = root;
    for (int n = 1 + int(rng() % 3); n > 0; --n) changed = mutate(changed, rng, pools);
    const QByteArray out = changed.isArray() ? QJsonDocument(changed.toArray()).toJson(QJsonDocument::Compact)
                           : changed.isObject() ? QJsonDocument(changed.toObject()).toJson(QJsonDocument::Compact)
                                                : QByteArray("{}");
    if (size_t(out.size()) > maxSize) return LLVMFuzzerMutate(data, size, maxSize);
    std::memcpy(data, out.constData(), size_t(out.size()));
    return size_t(out.size());
}

} // namespace Fuzz::Json

#define FUZZ_JSON_MUTATOR                                                                                             \
    extern "C" size_t LLVMFuzzerCustomMutator(uint8_t* data, size_t size, size_t maxSize, unsigned seed)               \
    {                                                                                                                 \
        return Fuzz::Json::mutateJson(data, size, maxSize, seed);                                                     \
    }

#else
#define FUZZ_JSON_MUTATOR
#endif
