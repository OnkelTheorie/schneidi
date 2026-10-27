#include "engine/ProxyCleanup.h"

#include "core/I18n.h"
#include "engine/ProxyManager.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <algorithm>

namespace ProxyCleanup {

namespace {

// The names ProxyManager gives: <sha1>.mp4, <sha1>.part.mp4 while writing, <sha1>.json (note). Nothing else is touched.
const QRegularExpression& ownName()
{
    static const QRegularExpression re("^([0-9a-f]{40})(\\.mp4|\\.part\\.mp4|\\.json)$");
    return re;
}

struct Note {
    QString original;
    int maxSize = 0;
};

Note readNote(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    return {o.value("original").toString(), o.value("maxSize").toInt()};
}

// Proxy names (<hash>) of these originals: compared by name, the proxy of an original is <hash>.mp4 in whatever folder
// holds the proxies
QSet<QString> proxyNames(const QStringList& originals)
{
    QSet<QString> out;
    for (const QString& original : originals)
        if (!original.isEmpty()) out.insert(QFileInfo(ProxyManager::proxyPath(original)).completeBaseName());
    return out;
}

QSet<QString> fileNames(const QStringList& files)
{
    QSet<QString> out;
    for (const QString& f : files)
        if (!f.isEmpty()) out.insert(QFileInfo(f).fileName());
    return out;
}

void addClips(const Timeline& tl, QStringList* out)
{
    for (const QVector<Track>* tracks : {&tl.video, &tl.audio})
        for (const Track& t : *tracks)
            for (const Clip& c : t.clips)
                if (!c.mediaPath.isEmpty()) *out << c.mediaPath;
}

} // namespace

Plan plan(const QStringList& keep, const QStringList& busy, const QString& dirPath)
{
    Plan p;
    const QDir dir(dirPath.isEmpty() ? ProxyManager::cacheDir() : dirPath);
    const QSet<QString> kept = proxyNames(keep);
    const QSet<QString> writing = fileNames(busy);

    const QFileInfoList files = dir.entryInfoList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
    QSet<QString> names;
    for (const QFileInfo& f : files) names.insert(f.fileName());
    for (const QFileInfo& f : files) {
        const QString name = f.fileName();
        const QRegularExpressionMatch m = ownName().match(name);
        if (!m.hasMatch() || writing.contains(name)) continue;
        const QString hash = m.captured(1), kind = m.captured(2);
        Item item;
        item.file = f.absoluteFilePath();
        item.bytes = f.size();
        if (kind == ".json") {
            if (names.contains(hash + ".mp4")) continue; // goes with its proxy
            item.reason = Reason::Leftover;              // its proxy is gone
        } else if (kind == ".part.mp4") {
            if (kept.contains(hash)) continue; // a proxy of an open/recent project's file may be on its way
            item.reason = Reason::Leftover;
        } else {
            const QString note = dir.filePath(hash + ".json");
            if (names.contains(hash + ".json")) item.bytes += QFileInfo(note).size();
            if (kept.contains(hash)) {
                ++p.kept;
                p.keptBytes += item.bytes;
                continue;
            }
            const Note n = readNote(note);
            item.original = n.original;
            if (n.original.isEmpty()) item.reason = Reason::Unknown;
            else if (!QFileInfo::exists(n.original)) item.reason = Reason::Missing;
            else if (n.maxSize > 0 && n.maxSize != ProxyManager::kMaxSize) item.reason = Reason::OtherSize;
            else if (QFileInfo(ProxyManager::proxyPath(n.original)).completeBaseName() != hash) item.reason = Reason::Changed;
            else item.reason = Reason::Unused;
        }
        p.items << item;
        p.bytes += item.bytes;
    }
    std::sort(p.items.begin(), p.items.end(), [](const Item& a, const Item& b) {
        return a.bytes != b.bytes ? a.bytes > b.bytes : a.file < b.file;
    });
    return p;
}

qint64 apply(const Plan& plan, const QStringList& busy, const QStringList& keep)
{
    const QSet<QString> writing = fileNames(busy);
    const QSet<QString> kept = proxyNames(keep);
    qint64 freed = 0;
    for (const Item& item : plan.items) {
        const QFileInfo f(item.file);
        if (writing.contains(f.fileName())) continue; // started meanwhile
        const QString hash = f.fileName().left(40);
        if (kept.contains(hash) && !item.file.endsWith(".json")) continue; // needed by now (a note goes with its proxy)
        const qint64 size = f.size();
        if (!QFile::remove(item.file)) continue; // gone already, or open elsewhere (Windows)
        freed += size;
        if (item.file.endsWith(".mp4") && !item.file.endsWith(".part.mp4")) {
            const QString note = notePath(item.file);
            const qint64 noteSize = QFileInfo(note).size();
            if (QFile::remove(note)) freed += noteSize;
        }
    }
    return freed;
}

QStringList projectMedia(const ProjectData& data)
{
    QStringList out;
    for (const MediaInfo& m : data.media) out << m.path;
    addClips(data.timeline, &out);
    for (const Sequence& s : data.sequences) addClips(s.timeline, &out);
    out.removeDuplicates();
    return out;
}

QStringList recentProjects()
{
    return QSettings().value("recentProjects").toStringList();
}

QStringList mediaOfProjects(const QStringList& projectFiles)
{
    QStringList out;
    for (const QString& file : projectFiles) {
        ProjectData data;
        QString error;
        if (QFileInfo::exists(file) && ProjectFile::load(file, &data, &error)) out << projectMedia(data);
    }
    out.removeDuplicates();
    return out;
}

QString notePath(const QString& proxy)
{
    const QFileInfo f(proxy);
    return f.dir().filePath(f.completeBaseName() + ".json");
}

void writeNote(const QString& proxy, const QString& original, int maxSize)
{
    const QFileInfo fi(original);
    const QJsonObject o{{"original", fi.absoluteFilePath()},
                        {"size", fi.size()},
                        {"modified", fi.lastModified().toMSecsSinceEpoch()},
                        {"maxSize", maxSize}};
    QFile f(notePath(proxy));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QString reasonText(Reason reason)
{
    switch (reason) {
    case Reason::Missing: return T("Original fehlt (gelöscht oder verschoben)");
    case Reason::Changed: return T("Original seitdem geändert");
    case Reason::OtherSize: return T("Andere Proxy-Größe");
    case Reason::Unused: return T("In keinem offenen oder zuletzt geöffneten Projekt");
    case Reason::Unknown: return T("Original unbekannt (ältere Version), in keinem Projekt");
    case Reason::Leftover: return T("Unfertige Datei");
    }
    return {};
}

const char* reasonId(Reason reason)
{
    switch (reason) {
    case Reason::Missing: return "missing";
    case Reason::Changed: return "changed";
    case Reason::OtherSize: return "other_size";
    case Reason::Unused: return "unused";
    case Reason::Unknown: return "unknown";
    case Reason::Leftover: return "leftover";
    }
    return "";
}

QString sizeText(qint64 bytes)
{
    const double mb = bytes / (1024.0 * 1024.0);
    if (mb >= 1024) return T("%1 GB").arg(QString::number(mb / 1024, 'f', 1).replace('.', I18n::decimalPoint()));
    return T("%1 MB").arg(mb >= 10 ? QString::number(qRound64(mb))
                                   : QString::number(mb, 'f', 1).replace('.', I18n::decimalPoint()));
}

} // namespace ProxyCleanup
