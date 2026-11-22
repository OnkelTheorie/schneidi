// SPDX-License-Identifier: AGPL-3.0-only
#include "core/Relink.h"

#include "core/ProjectFile.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <utility>

namespace Relink {

namespace {

// The folder a path names its file in, of either system's spelling (empty = none)
QString folderOf(const QString& path)
{
    const qsizetype sep = std::max(path.lastIndexOf('/'), path.lastIndexOf('\\'));
    if (sep <= 0) return {};
    const QString folder = path.left(sep);
    return folder.endsWith(':') ? folder + '/' : folder; // "C:" alone would be the drive's current folder
}

// Too much to walk and nothing "near": the filesystem's root (a drive) and the home folder itself
bool tooWide(const QString& dir)
{
    const QString canonical = QFileInfo(dir).canonicalFilePath();
    if (canonical.isEmpty() || QDir(canonical).isRoot()) return true;
    const QString home = QFileInfo(QDir::homePath()).canonicalFilePath();
    return !home.isEmpty() && QDir::cleanPath(canonical) == QDir::cleanPath(home);
}

enum class Kind { Unknown, Still, Picture, Sound };

Kind kindOf(const MediaInfo& m)
{
    if (m.isImage) return Kind::Still;
    if (m.hasVideo) return Kind::Picture;
    if (m.hasAudio) return Kind::Sound;
    return Kind::Unknown; // unreadable
}

// Lengths that differ more than a rounding (frame rate counted differently, a container's last frame): another
// recording. Unknown (0) or a still: not different.
bool lengthDiffers(const MediaInfo& old, const MediaInfo* now)
{
    if (!now || old.isImage || now->isImage || old.length <= 0 || now->length <= 0) return false;
    return std::abs(old.length - now->length) > std::max(2, old.length / 200);
}

// How many folders at the end of both paths match (camA/C0001.MP4 belongs to .../camA/C0001.MP4)
int suffixScore(const QString& a, const QString& b)
{
    static const QRegularExpression sep("[/\\\\]");
    const QStringList x = a.split(sep, Qt::SkipEmptyParts), y = b.split(sep, Qt::SkipEmptyParts);
    int n = 0;
    while (n < x.size() && n < y.size() && x[x.size() - 1 - n] == y[y.size() - 1 - n]) ++n;
    return n;
}

} // namespace

QVector<Root> nearRoots(const QStringList& missing)
{
    QVector<Root> roots;
    QSet<QString> seen;
    const auto add = [&](const QString& dir) {
        if (dir.isEmpty() || !QFileInfo(dir).isDir() || tooWide(dir)) return;
        const QString key = QFileInfo(dir).canonicalFilePath();
        if (seen.contains(key)) return;
        seen.insert(key);
        roots << Root{dir, kNearDepth};
    };
    // All old folders first: the closest places are walked before the time runs out
    for (const QString& p : missing) add(folderOf(p));
    for (const QString& p : missing) add(folderOf(folderOf(p)));
    return roots;
}

QVector<Found> find(const QVector<Root>& roots, const QSet<QString>& names, const Limits& limits, Progress* progress,
                    bool* truncated)
{
    QVector<Found> out;
    if (truncated) *truncated = false;
    if (names.isEmpty()) return out;
    QElapsedTimer clock;
    clock.start();
    QSet<QString> visited; // canonical folders: none twice (a root inside another, a link to a folder walked already)
    int dirs = 0;
    const auto stop = [&] {
        if ((progress && progress->cancel.load()) || (limits.maxDirs > 0 && dirs >= limits.maxDirs)
            || (limits.timeMs > 0 && clock.elapsed() > limits.timeMs)) {
            if (truncated) *truncated = true;
            return true;
        }
        return false;
    };
    for (const Root& root : roots) {
        std::deque<std::pair<QString, int>> queue; // breadth first: the closest folders before the deep ones
        const QString start = QFileInfo(root.dir).canonicalFilePath();
        if (start.isEmpty() || visited.contains(start)) continue;
        visited.insert(start);
        queue.emplace_back(start, 0);
        while (!queue.empty()) {
            if (stop()) return out;
            const auto [dir, depth] = queue.front();
            queue.pop_front();
            ++dirs;
            if (progress) progress->dirs.store(dirs);
            // No QDir::Hidden: hidden files and folders (.git, .cache, system folders on Windows) stay out
            const QFileInfoList entries = QDir(dir).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QFileInfo& fi : entries) {
                if (fi.isDir()) {
                    if (depth >= root.depth || fi.isSymLink()) continue; // a linked folder could lead back: loops
#if defined(Q_OS_WIN) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
                    if (fi.isJunction()) continue;
#endif
                    const QString canonical = fi.canonicalFilePath();
                    if (canonical.isEmpty() || visited.contains(canonical)) continue;
                    visited.insert(canonical);
                    queue.emplace_back(canonical, depth + 1);
                } else if (names.contains(fi.fileName())) {
                    out << Found{fi.absoluteFilePath(), fi.size()};
                }
            }
        }
    }
    return out;
}

QHash<QString, QString> match(const QVector<MediaInfo>& missing, const QVector<Found>& found, Mode mode,
                              const QHash<QString, MediaInfo>& probed, const QSet<QString>& taken)
{
    QHash<QString, QVector<const MediaInfo*>> wanted; // file name -> missing media (camera names like C0001.MP4 repeat)
    for (const MediaInfo& m : missing) wanted[ProjectFile::fileNameAnyOs(m.path)] << &m;
    QHash<QString, QVector<const Found*>> byName;
    QSet<QString> seen;
    for (const Found& f : found) {
        if (taken.contains(f.path) || seen.contains(f.path)) continue; // in the pool already / found twice
        seen.insert(f.path);
        byName[QFileInfo(f.path).fileName()] << &f;
    }

    QHash<QString, QString> moved; // old path -> new path
    for (auto w = wanted.cbegin(); w != wanted.cend(); ++w) {
        const QVector<const Found*> candidates = byName.value(w.key());
        if (candidates.isEmpty()) continue;
        // rank = how sure (2 = the same size, 1 = size not known and length not different, 0 = size or length
        // differ), then the folders that match
        struct Pair {
            int sure, score;
            QString old, now;
        };
        QVector<Pair> pairs;
        for (const MediaInfo* old : w.value())
            for (const Found* now : candidates) {
                const auto p = probed.constFind(now->path);
                const MediaInfo* info = p == probed.cend() ? nullptr : &*p;
                if (info) {
                    const Kind a = kindOf(*old), b = kindOf(*info);
                    if (a != Kind::Unknown && b != Kind::Unknown && a != b) continue; // never another kind of media
                    if (mode == Mode::Near && b == Kind::Unknown) continue;           // unreadable: not by itself
                }
                int sure = 1;
                if (old->fileSize > 0 && now->size >= 0) sure = old->fileSize == now->size ? 2 : 0;
                else if (lengthDiffers(*old, info)) sure = 0;
                if (mode == Mode::Near && sure == 0) continue; // by itself only what clearly is the file
                pairs << Pair{sure, suffixScore(old->path, now->path), old->path, now->path};
            }
        const auto rank = [](const Pair& p) { return p.sure * 10000 + p.score; };
        std::stable_sort(pairs.begin(), pairs.end(), [&](const Pair& a, const Pair& b) { return rank(a) > rank(b); });
        // Assign pairs with the best match first, each file only once
        QSet<QString> used, offline;
        for (const Pair& pr : pairs) {
            if (moved.contains(pr.old) || used.contains(pr.now) || offline.contains(pr.old)) continue;
            // Ambiguous (another file fits just as well, e.g. C0001.MP4 on two camera cards, or another old file of the
            // same name fits this one just as well): better leave offline than to show wrong material - also no worse
            // match later
            const bool ambiguous = std::any_of(pairs.begin(), pairs.end(), [&](const Pair& o) {
                return rank(o) == rank(pr) && !moved.contains(o.old) && !used.contains(o.now)
                       && !offline.contains(o.old) && ((o.old == pr.old) != (o.now == pr.now));
            });
            if (ambiguous) {
                offline.insert(pr.old);
                continue;
            }
            moved.insert(pr.old, pr.now);
            used.insert(pr.now);
        }
    }
    return moved;
}

Result search(const QVector<MediaInfo>& missing, const QSet<QString>& taken, const QVector<Root>& roots, Mode mode,
              const Limits& limits, const std::function<MediaInfo(const QString&)>& probe, Progress* progress)
{
    Result r;
    QSet<QString> names;
    for (const MediaInfo& m : missing) names.insert(ProjectFile::fileNameAnyOs(m.path));
    const QVector<Found> found = find(roots, names, limits, progress, &r.truncated);
    if (progress) r.dirs = progress->dirs.load();
    QHash<QString, MediaInfo> probed;
    if (probe)
        for (const Found& f : found) {
            if (progress && progress->cancel.load()) {
                r.truncated = true;
                return r; // cancelled: nothing half-checked is applied
            }
            if (!taken.contains(f.path)) probed.insert(f.path, probe(f.path));
        }
    r.moved = match(missing, found, mode, probed, taken);
    for (const QString& now : std::as_const(r.moved))
        if (probed.contains(now)) r.probed.insert(now, probed.value(now));
    return r;
}

} // namespace Relink
