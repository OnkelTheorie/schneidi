// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Probe results of files, for the timeline's drop preview, the Viewer of a Media Storage file and the import of a
// file dropped from outside (one probe for the drop preview and the import). A file probed while it was still being
// written, copied or held by another program stayed "no picture, no sound" for the whole session when every result
// was kept by path, its drop refused although the thumbnail showed it. So:
// - a result goes when the file's size or time of change differ from when it was probed (the stamp is taken before
//   probing: a file changing meanwhile is not kept with a stale result);
// - a usable result is kept; one that found nothing is kept only briefly (kNegativeMs), only when the file is
//   neither locked nor written to moments ago (repeated drag-enters do not open it again and again) and only with
//   its reason (a quick look does not find it: the import of the drop probes again and can say why); a partial
//   result (a quick look that skipped the long stream analysis) is not kept.
// probe() runs on any thread; the cache itself is used on one thread (the UI's).

#include "core/Types.h"
#include "engine/MediaProbe.h"

#include <QHash>
#include <QString>

#include <optional>

struct ProbeResult {
    MediaInfo info;                 // path and name always; no picture and no sound when unreadable
    qint64 size = -1, modified = 0; // the file's stamp before probing (size -1: missing)
    bool partial = false;           // a quick look that skipped the long stream analysis
    MediaProbe::Busy busy = MediaProbe::Busy::No; // unreadable results only: maybe readable a moment later
    QString why;                    // unreadable results of a full probe: MediaProbe::whyUnreadable
    bool usable() const { return info.length > 0 && (info.hasVideo || info.hasAudio); }
};

class ProbeCache {
public:
    static constexpr qint64 kNegativeMs = 30000;

    // Probes `path` (lengths in `rate`); `longAnalysis` false = a quick look for the UI thread (MediaProbe::ReadOptions)
    static ProbeResult probe(const QString& path, FrameRate rate, bool longAnalysis = true);

    // The kept result of `path` while its file is unchanged (and a negative one is young enough)
    std::optional<ProbeResult> find(const QString& path);
    // Keeps `r` by the rules above (under r.info.path)
    void insert(const ProbeResult& r);
    void remove(const QString& path) { m_entries.remove(path); }
    void clear() { m_entries.clear(); }

private:
    struct Entry {
        ProbeResult result;
        qint64 keptAt = 0; // ms since the epoch
    };
    QHash<QString, Entry> m_entries;
};
