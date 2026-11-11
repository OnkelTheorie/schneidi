// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/StorageThumbCache.h"

#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QImageReader>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <vector>

namespace StorageThumbCache {

namespace {
constexpr int kQuality = 85;
constexpr qint64 kMaxBytes = 64ll << 20;
constexpr int kUnusedDays = 60;
} // namespace

QString dir()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/storage-thumbs");
}

QString fileFor(const QFileInfo& file)
{
    const QByteArray key = (file.absoluteFilePath() + '|' + QString::number(file.size()) + '|'
                            + QString::number(file.lastModified().toMSecsSinceEpoch()))
                               .toUtf8();
    return dir() + '/' + QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex()) + ".jpg";
}

QImage load(const QFileInfo& file)
{
    const QString path = fileFor(file);
    QImageReader reader(path, "jpeg");
    QImage img = reader.read();
    if (img.isNull()) return {};
    // Used: kept by prune() (touched at most once a day, no write per look)
    QFile f(path);
    const QDateTime now = QDateTime::currentDateTime();
    if (QFileInfo(path).lastModified().daysTo(now) >= 1 && f.open(QIODevice::ReadWrite))
        f.setFileTime(now, QFileDevice::FileModificationTime);
    return img;
}

bool store(const QFileInfo& file, const QImage& picture)
{
    if (picture.isNull() || !QDir().mkpath(dir())) return false;
    QSaveFile out(fileFor(file));
    if (!out.open(QIODevice::WriteOnly)) return false;
    if (!picture.save(&out, "JPEG", kQuality)) {
        out.cancelWriting();
        return false;
    }
    return out.commit();
}

int prune(bool force)
{
    QSettings s;
    const QDate today = QDate::currentDate();
    if (!force && s.value(QStringLiteral("storageThumbs/pruned")).toDate() == today) return 0;
    s.setValue(QStringLiteral("storageThumbs/pruned"), today);
    struct Entry {
        QString path;
        QDateTime used;
        qint64 bytes;
    };
    std::vector<Entry> entries;
    QDirIterator it(dir(), {QStringLiteral("*.jpg")}, QDir::Files);
    while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        entries.push_back({fi.absoluteFilePath(), fi.lastModified(), fi.size()});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.used > b.used; }); // newest first
    const QDateTime oldest = QDateTime::currentDateTime().addDays(-kUnusedDays);
    qint64 total = 0;
    int removed = 0;
    for (const Entry& e : entries) {
        total += e.bytes;
        if ((e.used < oldest || total > kMaxBytes) && QFile::remove(e.path)) ++removed;
    }
    return removed;
}

} // namespace StorageThumbCache
