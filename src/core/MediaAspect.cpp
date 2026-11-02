// SPDX-License-Identifier: AGPL-3.0-only
#include "core/MediaAspect.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QMutex>

#include <algorithm>

namespace MediaAspect {

namespace {

QMutex g_mutex;
Probe g_probe;
QHash<QString, double> g_cache; // "path|size|mtime" -> aspect

} // namespace

void setProbe(Probe probe)
{
    QMutexLocker lock(&g_mutex);
    g_probe = std::move(probe);
    g_cache.clear();
}

double of(const QString& path)
{
    if (path.isEmpty()) return 0;
    const QFileInfo fi(path); // a missing file asks the probe too (0); found again later, its size/time differ
    const QString key = QString("%1|%2|%3").arg(fi.absoluteFilePath()).arg(fi.size()).arg(fi.lastModified().toMSecsSinceEpoch());
    Probe probe;
    {
        QMutexLocker lock(&g_mutex);
        if (const auto it = g_cache.constFind(key); it != g_cache.cend()) return *it;
        probe = g_probe;
    }
    if (!probe) return 0;
    const double aspect = std::max(0.0, probe(path)); // outside the lock: opening a file takes a while
    QMutexLocker lock(&g_mutex);
    g_cache.insert(key, aspect);
    return aspect;
}

double ofClip(const Clip& c)
{
    if (c.kind != ClipKind::Media) return 0;
    return c.pictureAspect > 0 ? c.pictureAspect : of(c.mediaPath);
}

QSizeF fit(double aspect, QSizeF frame)
{
    if (aspect <= 0 || frame.isEmpty()) return frame;
    if (aspect > frame.width() / frame.height()) return {frame.width(), frame.width() / aspect};
    return {frame.height() * aspect, frame.height()};
}

Reshape reshape(QSizeF frame, double fileAspect, double aspect)
{
    const auto centred = [&](QSizeF s) {
        return QRectF(QPointF((frame.width() - s.width()) / 2, (frame.height() - s.height()) / 2), s);
    };
    return {centred(fit(fileAspect, frame)), centred(fit(aspect, frame))};
}

} // namespace MediaAspect
