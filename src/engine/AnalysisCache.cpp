// SPDX-License-Identifier: AGPL-3.0-only
// Disk cache of per-file analysis results, see AnalysisCache.h
#include "engine/AnalysisCache.h"

#include "core/AppPaths.h"
#include "core/PathKey.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtEndian>

#include <cstring>

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

// The entry's file; empty = the media file is gone (nothing to key on)
QString entryFile(const QString& kind, const QString& file, const QJsonObject& options, int version)
{
    const QFileInfo fi(file);
    if (!fi.isFile()) return {};
    // Size and modification time read now (not FileStamp's settled stamp: a file overwritten a moment ago must not
    // answer with the old file's result)
    const QString id = QString("%1|%2|%3|%4|%5|%6")
                           .arg(cleanKind(kind), PathKey::of(fi.absoluteFilePath()))
                           .arg(fi.size())
                           .arg(fi.lastModified().toMSecsSinceEpoch())
                           .arg(QString::fromUtf8(QJsonDocument(options).toJson(QJsonDocument::Compact)))
                           .arg(version);
    const QString key = QString::fromLatin1(QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha1).toHex());
    return folder() + "/" + cleanKind(kind) + "/" + key + ".json";
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

std::optional<QJsonObject> get(const QString& kind, const QString& file, const QJsonObject& options, int version)
{
    const QString path = entryFile(kind, file, options, version);
    if (path.isEmpty()) return std::nullopt;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    // The version is in the key already; checked again so a damaged or foreign file is no answer
    if (o["version"].toInt(-1) != version || !o["result"].isObject()) return std::nullopt;
    return o["result"].toObject();
}

void put(const QString& kind, const QString& file, const QJsonObject& options, int version, const QJsonObject& result)
{
    const QString path = entryFile(kind, file, options, version);
    if (path.isEmpty()) return;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile out(path);
    if (out.open(QIODevice::WriteOnly)
        && out.write(QJsonDocument(QJsonObject{{"version", version}, {"file", QFileInfo(file).absoluteFilePath()},
                                               {"result", result}})
                         .toJson(QJsonDocument::Compact))
               >= 0)
        out.commit();
}

} // namespace AnalysisCache
