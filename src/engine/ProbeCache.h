// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Probe results of files not in the Media Pool (the timeline's drop preview, the Viewer for a file of Media Storage),
// so dragging over the timeline does not open the file on every enter. Only usable results are kept (a length and a
// picture or sound): a file probed while it was still being written, copied or held by another program stayed "no
// picture, no sound" for the whole session, its drop refused although the thumbnail showed it. A kept result goes
// when the file's size or time of change differ from when it was probed (one stat per lookup).

#include "core/Types.h"

#include <QFileInfo>
#include <QHash>
#include <QString>

#include <functional>
#include <optional>

class ProbeCache {
public:
    // A result worth keeping: something the timeline can place
    static bool usable(const MediaInfo& m) { return m.length > 0 && (m.hasVideo || m.hasAudio); }

    // The kept result of `path` while its file is unchanged
    std::optional<MediaInfo> find(const QString& path)
    {
        const auto it = m_entries.constFind(path);
        if (it == m_entries.cend()) return std::nullopt;
        if (it->stamp != stampOf(path)) {
            m_entries.erase(it);
            return std::nullopt;
        }
        return it->info;
    }
    // Keeps `info` (under info.path) when it is usable, with the file's stamp now
    void insert(const MediaInfo& info)
    {
        if (!usable(info)) {
            m_entries.remove(info.path);
            return;
        }
        m_entries.insert(info.path, Entry{info, stampOf(info.path)});
    }
    // The kept result, else `probe(path)` (kept when usable)
    MediaInfo get(const QString& path, const std::function<MediaInfo(const QString&)>& probe)
    {
        if (std::optional<MediaInfo> known = find(path)) return *std::move(known);
        MediaInfo info = probe(path);
        info.path = path;
        insert(info);
        return info;
    }
    void remove(const QString& path) { m_entries.remove(path); }
    void clear() { m_entries.clear(); }
    bool contains(const QString& path) const { return m_entries.contains(path); }

private:
    struct Stamp {
        qint64 size = -1;
        qint64 modified = 0;
        bool operator==(const Stamp&) const = default;
    };
    struct Entry {
        MediaInfo info;
        Stamp stamp;
    };
    static Stamp stampOf(const QString& path)
    {
        const QFileInfo fi(path);
        if (!fi.exists()) return {};
        return {fi.size(), fi.lastModified().toMSecsSinceEpoch()};
    }
    QHash<QString, Entry> m_entries;
};
