// SPDX-License-Identifier: AGPL-3.0-only
// Shown names of compositing recipes in the UI language (core/CompRecipes.h shownName/shownDescription)
#include "core/CompRecipes.h"

#include "core/I18n.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QMutex>

namespace {

struct Cached {
    QDateTime stamp;
    QJsonObject json;
};

// The recipe file's JSON (built-in ones are resources), read again only when the file changed
QJsonObject recipeJson(const QString& file)
{
    if (file.isEmpty()) return {};
    static QMutex lock;
    static QHash<QString, Cached> cache;
    const QDateTime stamp = QFileInfo(file).lastModified();
    const QMutexLocker guard(&lock);
    auto it = cache.find(file);
    if (it != cache.end() && it->stamp == stamp) return it->json;
    QFile f(file);
    QJsonObject o;
    if (f.open(QIODevice::ReadOnly)) o = QJsonDocument::fromJson(f.readAll()).object();
    cache.insert(file, {stamp, o});
    return o;
}

QString shown(const CompRecipe& r, const char* field, const QString& fallback, const QString& lang)
{
    const QString code = lang.isEmpty() ? I18n::language() : lang;
    const QString v = recipeJson(r.file).value(QStringLiteral("%1_%2").arg(QLatin1String(field), code)).toString().trimmed();
    return v.isEmpty() ? fallback : v;
}

} // namespace

namespace CompRecipes {

QString shownName(const CompRecipe& r, const QString& lang) { return shown(r, "name", r.name, lang); }
QString shownDescription(const CompRecipe& r, const QString& lang) { return shown(r, "description", r.description, lang); }

} // namespace CompRecipes
