// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Favourites and recently used entries of the Effects Library (0.22), shared with the CLI/MCP `effects` command.
// An entry is named by a stable key made from its library data ("fx:blur", "video:dissolve", "title:counter",
// "shape:arrow", "lut:<path>", "sound:{json}", "recipe:<id>" …): a file inside the effects folder is stored relative
// to it ("lut:@/LUTs/Film/x.cube"), so moving the user folder keeps the favourites. Stored in QSettings
// (effects/favorites, effects/recent, string lists). A key whose item no longer exists stays stored and is simply not
// shown (it comes back with the file).

#include <QString>
#include <QStringList>

namespace EffectFavorites {

inline constexpr int kRecentMax = 10;

// Stable key of a library entry's data; empty = the entry has none (not offered as a favourite)
QString key(const QString& data);
// The file a key of a file entry names (absolute, or ":/…" for a bundled one); empty = not a file key
QString filePath(const QString& key);

QStringList favorites();
bool isFavorite(const QString& key);
void setFavorite(const QString& key, bool on);
bool toggleFavorite(const QString& key); // returns the new state

// Most recent first, at most kRecentMax, no duplicates
QStringList recent();
void noteUsed(const QString& key);

} // namespace EffectFavorites
