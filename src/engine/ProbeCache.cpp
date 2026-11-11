// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/ProbeCache.h"

#include <QDateTime>
#include <QFileInfo>

namespace {

void stampOf(const QString& path, qint64* size, qint64* modified)
{
    const QFileInfo fi(path);
    *size = fi.exists() ? fi.size() : -1;
    *modified = fi.exists() ? fi.lastModified().toMSecsSinceEpoch() : 0;
}

} // namespace

ProbeResult ProbeCache::probe(const QString& path, FrameRate rate, bool longAnalysis)
{
    ProbeResult r;
    stampOf(path, &r.size, &r.modified);
    MediaProbe::ReadOptions options;
    options.longAnalysis = longAnalysis;
    options.partial = &r.partial;
    if (std::optional<MediaInfo> m = MediaProbe::read(path, rate, options)) r.info = *std::move(m);
    r.info.path = path;
    r.info.name = QFileInfo(path).fileName();
    if (!r.usable()) {
        r.busy = MediaProbe::busy(path);
        if (longAnalysis) r.why = MediaProbe::whyUnreadable(path);
    }
    return r;
}

std::optional<ProbeResult> ProbeCache::find(const QString& path)
{
    const auto it = m_entries.find(path);
    if (it == m_entries.end()) return std::nullopt;
    qint64 size = 0, modified = 0;
    stampOf(path, &size, &modified);
    const bool stale = size != it->result.size || modified != it->result.modified
                       || (!it->result.usable() && QDateTime::currentMSecsSinceEpoch() - it->keptAt > kNegativeMs);
    if (stale) {
        m_entries.erase(it);
        return std::nullopt;
    }
    return it->result;
}

void ProbeCache::insert(const ProbeResult& r)
{
    const QString& path = r.info.path;
    qint64 size = 0, modified = 0;
    stampOf(path, &size, &modified);
    const bool keep = !r.partial && size == r.size && modified == r.modified
                      && (r.usable() || (r.busy == MediaProbe::Busy::No && !r.why.isEmpty()));
    if (!keep) {
        m_entries.remove(path);
        return;
    }
    m_entries.insert(path, Entry{r, QDateTime::currentMSecsSinceEpoch()});
}
