#include "core/WatchFolders.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>

namespace WatchFolders {

namespace {

// The same extensions the Media Storage shows as media
const QStringList& videoExt()
{
    static const QStringList ext{"mp4", "mov", "mkv", "avi", "webm", "mts", "m2ts", "m4v", "mpg", "mpeg", "mxf", "wmv", "flv", "3gp"};
    return ext;
}
const QStringList& audioExt()
{
    static const QStringList ext{"mp3", "wav", "flac", "ogg", "opus", "m4a", "aac", "wma", "aiff", "aif"};
    return ext;
}
const QStringList& imageExt()
{
    static const QStringList ext{"png", "jpg", "jpeg", "webp", "bmp", "tif", "tiff", "gif"};
    return ext;
}

constexpr int kMaxDepth = 8;      // subfolders below a watch folder (like importing a folder)
constexpr int kMaxFolders = 2000; // listed per watch folder (a watch on a whole disk must not take forever)

} // namespace

Kind kindOf(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    if (videoExt().contains(ext)) return Kind::Video;
    if (audioExt().contains(ext)) return Kind::Audio;
    if (imageExt().contains(ext)) return Kind::Image;
    return Kind::Other;
}

bool takes(const WatchFolder& w, const QString& path)
{
    switch (kindOf(path)) {
    case Kind::Video: return w.video;
    case Kind::Audio: return w.audio;
    case Kind::Image: return w.images;
    case Kind::Other: break;
    }
    return false;
}

bool isTemporary(const QString& fileName)
{
    if (fileName.isEmpty()) return true;
    // Hidden (rsync, Syncthing and many recorders write ".name" first), Office/backup style "~name", "name~", "$name"
    if (fileName.startsWith('.') || fileName.startsWith('~') || fileName.startsWith('$') || fileName.endsWith('~'))
        return true;
    // Browsers and download tools: "clip.mp4.part", "clip.mp4.crdownload"; some tools put the mark before the
    // extension ("clip.part.mp4", "clip.tmp.mkv")
    static const QStringList marks{"part", "partial", "tmp", "temp", "crdownload", "download", "opdownload",
                                   "filepart", "inprogress", "aria2", "!ut", "!qb"};
    const QStringList parts = fileName.toLower().split('.');
    for (int i = 1; i < parts.size(); ++i)
        if (marks.contains(parts[i])) return true;
    return false;
}

QString pathKey(const QString& path)
{
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
#ifdef Q_OS_WIN
    return clean.toLower();
#else
    return clean;
#endif
}

bool samePath(const QString& a, const QString& b) { return pathKey(a) == pathKey(b); }

int indexOf(const QVector<WatchFolder>& list, const QString& folder)
{
    for (int i = 0; i < list.size(); ++i)
        if (samePath(list[i].path, folder)) return i;
    return -1;
}

QStringList typeNames(const WatchFolder& w)
{
    QStringList out;
    if (w.video) out << "video";
    if (w.audio) out << "audio";
    if (w.images) out << "image";
    return out;
}

bool setTypes(WatchFolder& w, const QStringList& names)
{
    WatchFolder t = w;
    t.video = t.audio = t.images = false;
    for (const QString& n0 : names) {
        const QString n = n0.trimmed().toLower();
        if (n == "video" || n == "videos") t.video = true;
        else if (n == "audio" || n == "sound") t.audio = true;
        else if (n == "image" || n == "images" || n == "picture" || n == "pictures") t.images = true;
        else if (!n.isEmpty()) return false;
    }
    if (!t.video && !t.audio && !t.images) return false;
    w = t;
    return true;
}

QVector<Listed> list(const WatchFolder& w, QStringList* folders)
{
    QVector<Listed> out;
    if (w.path.isEmpty() || !QFileInfo(w.path).isDir()) return out;
    QStringList dirs{QDir::cleanPath(w.path)};
    QVector<int> depth{0};
    for (int d = 0; d < dirs.size(); ++d) {
        if (folders) *folders << dirs[d];
        QDir::Filters filter = QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks; // no hidden entries either
        if (w.subfolders && depth[d] < kMaxDepth) filter |= QDir::Dirs;
        for (const QFileInfo& fi : QDir(dirs[d]).entryInfoList(filter, QDir::Name)) {
            if (fi.isDir()) {
                if (dirs.size() < kMaxFolders && !fi.fileName().startsWith('.')) {
                    dirs << fi.absoluteFilePath();
                    depth << depth[d] + 1;
                }
                continue;
            }
            const QString path = fi.absoluteFilePath();
            if (fi.isHidden() || isTemporary(fi.fileName()) || !takes(w, path)) continue;
            Listed l{path, fi.size(), fi.lastModified().toMSecsSinceEpoch(), 0};
            // A copy keeps its old modification time (but is created now), a file moved in changes its metadata
            for (const QDateTime& t : {fi.birthTime(), fi.metadataChangeTime()})
                if (t.isValid()) l.created = std::max(l.created, t.toMSecsSinceEpoch());
            out << l;
        }
    }
    return out;
}

Scanner::Result Scanner::scan(const QVector<WatchFolder>& watches, const std::function<bool(const QString&)>& inPool,
                              const QDateTime& now)
{
    Result r;
    const qint64 nowMs = now.toMSecsSinceEpoch();
    QSet<QString> present, listed;
    for (int wi = 0; wi < watches.size(); ++wi) {
        const WatchFolder& w = watches[wi];
        const QString wkey = pathKey(w.path);
        // Gone or offline (an unplugged drive, a network share): listed anew when it is back, its files as they are then
        if (w.path.isEmpty() || !QFileInfo(w.path).isDir()) continue;
        const bool firstListing = !m_listed.contains(wkey) && !listed.contains(wkey);
        listed.insert(wkey);
        QSet<QString> ignored;
        for (const QString& f : w.ignored) ignored.insert(pathKey(f));
        for (const Listed& f : list(w, &r.folders)) {
            const QString key = pathKey(f.path);
            if (present.contains(key)) continue; // two watches cover it: the first one takes it
            present.insert(key);
            auto it = m_seen.find(key);
            if (it == m_seen.end()) {
                it = m_seen.insert(key, {});
                it->appeared = firstListing ? 0 : nowMs;
            }
            Seen& s = *it;
            if (s.size != f.size || s.modified != f.modified) {
                s.size = f.size;
                s.modified = f.modified;
                s.unchangedSince = nowMs;
                s.rejected = false;
            }
            // Empty: not even started (a recorder that created it, a placeholder) - looked at once it grows
            if (f.size == 0 || inPool(key) || ignored.contains(key) || s.rejected) continue;
            // When it came: written, created, moved in, or seen appearing while watching
            const qint64 arrived = std::max({f.modified, f.created, s.appeared});
            if (w.since.isValid() && arrived < w.since.toMSecsSinceEpoch()) continue;
            // Complete: nothing written for a while (by its time, or as seen by an earlier scan - the clock of a
            // network share may differ from this one)
            const bool settled = nowMs - f.modified >= m_settleMs
                                 || (s.unchangedSince > 0 && nowMs - s.unchangedSince >= m_settleMs);
            if (settled) r.ready << Found{f.path, wi};
            else r.pending << f.path;
        }
    }
    // Forget files that are gone (memory stays small) and folders not listed now
    for (auto it = m_seen.begin(); it != m_seen.end();)
        it = present.contains(it.key()) ? std::next(it) : m_seen.erase(it);
    m_listed = listed;
    return r;
}

void Scanner::rejected(const QString& path)
{
    const auto it = m_seen.find(pathKey(path));
    if (it != m_seen.end()) it->rejected = true;
}

} // namespace WatchFolders
