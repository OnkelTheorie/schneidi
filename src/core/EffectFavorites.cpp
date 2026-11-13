// SPDX-License-Identifier: AGPL-3.0-only
#include "core/EffectFavorites.h"

#include "core/EffectFolders.h"
#include "core/Presets.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

namespace EffectFavorites {

namespace {

constexpr const char* kFavorites = "effects/favorites";
constexpr const char* kRecent = "effects/recent";
constexpr const char* kInFolder = "@/"; // a path relative to the effects folder

// A path inside the effects folder relative to it, others unchanged
QString stablePath(const QString& path)
{
    const QString root = EffectFolders::root();
    if (!root.isEmpty() && path.startsWith(root + '/')) return kInFolder + path.mid(root.size() + 1);
    return path;
}

QString absolutePath(const QString& stored)
{
    if (stored.startsWith(QLatin1String(kInFolder))) return EffectFolders::root() + '/' + stored.mid(2);
    return stored;
}

const QStringList kFilePrefixes{QString::fromLatin1(EffectFolders::LutPrefix), QStringLiteral("luma:"),
                                QString::fromLatin1(Presets::Prefix)};

} // namespace

QString key(const QString& data)
{
    if (data.isEmpty()) return {};
    for (const QString& prefix : kFilePrefixes)
        if (data.startsWith(prefix)) return data.size() > prefix.size() ? prefix + stablePath(data.mid(prefix.size())) : QString();
    if (data.startsWith(QLatin1String("sound:"))) { // the op's fields; a file kind relative like the others
        QJsonObject o = QJsonDocument::fromJson(data.mid(6).toUtf8()).object();
        if (o.isEmpty()) return {};
        o["kind"] = stablePath(o["kind"].toString());
        return "sound:" + QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
    }
    return data;
}

QString filePath(const QString& key)
{
    for (const QString& prefix : kFilePrefixes)
        if (key.startsWith(prefix)) return absolutePath(key.mid(prefix.size()));
    return {};
}

QStringList favorites() { return QSettings().value(kFavorites).toStringList(); }

bool isFavorite(const QString& key) { return !key.isEmpty() && favorites().contains(key); }

void setFavorite(const QString& key, bool on)
{
    if (key.isEmpty()) return;
    QStringList list = favorites();
    list.removeAll(key);
    if (on) list << key;
    QSettings().setValue(kFavorites, list);
}

bool toggleFavorite(const QString& key)
{
    const bool on = !isFavorite(key);
    setFavorite(key, on);
    return on;
}

QStringList recent() { return QSettings().value(kRecent).toStringList().mid(0, kRecentMax); }

void noteUsed(const QString& key)
{
    if (key.isEmpty()) return;
    QStringList list = recent();
    list.removeAll(key);
    list.prepend(key);
    QSettings().setValue(kRecent, list.mid(0, kRecentMax));
}

} // namespace EffectFavorites
