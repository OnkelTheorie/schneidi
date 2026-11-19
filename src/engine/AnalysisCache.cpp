// SPDX-License-Identifier: AGPL-3.0-only
// Disk cache of per-file analysis results, see AnalysisCache.h
#include "engine/AnalysisCache.h"

#include "core/AppPaths.h"
#include "core/PathKey.h"
#include "engine/Background.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QDirIterator>
#include <QStandardPaths>
#include <QThreadPool>
#include <QtEndian>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace AnalysisCache {

namespace {

// Kinds are folder names: plain ids only ("silence", "frame-peaks")
QString cleanKind(const QString& kind)
{
    QString k;
    for (const QChar c : kind)
        if (c.isLetterOrNumber() || c == '-' || c == '_') k += c.toLower();
    return k.isEmpty() ? QStringLiteral("other") : k;
}

std::atomic<int> g_hits{0};
std::once_flag g_housekeeping;

// Once per process, off the caller's thread: the old audio events folder goes, the folder is kept below the cap
void housekeepingOnce()
{
    std::call_once(g_housekeeping, [] {
        QThreadPool::globalInstance()->start([] {
            Background::lowerThisThread(Background::Level::Lowest);
            QDir(AppPaths::writable(QStandardPaths::CacheLocation) + "/audio-events").removeRecursively();
            prune();
        });
    });
}

} // namespace

QString folder() { return AppPaths::writable(QStandardPaths::CacheLocation) + "/analysis"; }

namespace {

template <class T> QString packed(const std::vector<T>& values)
{
    QByteArray raw(qsizetype(values.size() * sizeof(T)), Qt::Uninitialized);
    if constexpr (Q_BYTE_ORDER == Q_LITTLE_ENDIAN) {
        if (!values.empty()) std::memcpy(raw.data(), values.data(), raw.size());
    } else {
        for (size_t i = 0; i < values.size(); ++i) qToLittleEndian(values[i], raw.data() + i * sizeof(T));
    }
    return QString::fromLatin1(raw.toBase64());
}

template <class T> std::vector<T> unpacked(const QJsonValue& v)
{
    if (!v.isString()) return {};
    const QByteArray raw = QByteArray::fromBase64(v.toString().toLatin1());
    if (raw.size() % qsizetype(sizeof(T)) != 0) return {};
    std::vector<T> out(size_t(raw.size()) / sizeof(T));
    if constexpr (Q_BYTE_ORDER == Q_LITTLE_ENDIAN) {
        if (!out.empty()) std::memcpy(out.data(), raw.constData(), raw.size());
    } else {
        for (size_t i = 0; i < out.size(); ++i) out[i] = qFromLittleEndian<T>(raw.constData() + i * sizeof(T));
    }
    return out;
}

} // namespace

QString pack(const std::vector<float>& values) { return packed(values); }
QString pack(const std::vector<double>& values) { return packed(values); }
std::vector<float> unpackFloats(const QJsonValue& v) { return unpacked<float>(v); }
std::vector<double> unpackDoubles(const QJsonValue& v) { return unpacked<double>(v); }

Key key(const QString& kind, const QString& file, const QJsonObject& options, int version)
{
    housekeepingOnce();
    Key k;
    const QFileInfo fi(file);
    if (!fi.isFile()) return k;
    k.file = fi.absoluteFilePath();
    // Size and modification time read now (not FileStamp's settled stamp: a file overwritten a moment ago must not
    // answer with the old file's result)
    k.size = fi.size();
    k.modified = fi.lastModified().toMSecsSinceEpoch();
    k.version = version;
    const QString id = QString("%1|%2|%3|%4|%5|%6")
                           .arg(cleanKind(kind), PathKey::of(k.file))
                           .arg(k.size)
                           .arg(k.modified)
                           .arg(QString::fromUtf8(QJsonDocument(options).toJson(QJsonDocument::Compact)))
                           .arg(version);
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha1).toHex());
    k.entry = folder() + "/" + cleanKind(kind) + "/" + hash + ".json";
    return k;
}

std::optional<QJsonObject> get(const Key& k)
{
    if (!k.valid()) return std::nullopt;
    QFile f(k.entry);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    // The version is in the key already; checked again so a damaged or foreign file is no answer
    if (o["version"].toInt(-1) != k.version || !o["result"].isObject()) return std::nullopt;
    // Used now: prune() keeps it (touched at most once a day, no write per hit)
    const QDateTime now = QDateTime::currentDateTime();
    if (f.fileTime(QFileDevice::FileModificationTime).secsTo(now) > 24 * 3600) {
        f.close();
        QFile touch(k.entry);
        if (touch.open(QIODevice::ReadWrite)) touch.setFileTime(now, QFileDevice::FileModificationTime);
    }
    ++g_hits;
    return o["result"].toObject();
}

void put(const Key& k, const QJsonObject& result)
{
    if (!k.valid()) return;
    // The file changed while it was measured (or is gone): the result is of neither version
    const QFileInfo fi(k.file);
    if (!fi.isFile() || fi.size() != k.size || fi.lastModified().toMSecsSinceEpoch() != k.modified) return;
    QDir().mkpath(QFileInfo(k.entry).absolutePath());
    QSaveFile out(k.entry);
    if (out.open(QIODevice::WriteOnly)
        && out.write(QJsonDocument(QJsonObject{{"version", k.version}, {"file", k.file}, {"result", result}})
                         .toJson(QJsonDocument::Compact))
               >= 0)
        out.commit();
}

int hits() { return g_hits.load(); }

int prune(qint64 capBytes)
{
    struct Entry {
        QString path;
        qint64 size;
        qint64 used;
    };
    std::vector<Entry> entries;
    qint64 total = 0;
    for (QDirIterator it(folder(), {"*.json"}, QDir::Files, QDirIterator::Subdirectories); it.hasNext();) {
        const QFileInfo fi = it.nextFileInfo();
        entries.push_back({fi.absoluteFilePath(), fi.size(), fi.lastModified().toMSecsSinceEpoch()});
        total += fi.size();
    }
    if (total <= capBytes) return 0;
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.used < b.used; });
    int removed = 0;
    const qint64 target = capBytes / 5 * 4;
    for (const Entry& e : entries) {
        if (total <= target) break;
        if (QFile::remove(e.path)) {
            total -= e.size;
            ++removed;
        }
    }
    return removed;
}

} // namespace AnalysisCache
