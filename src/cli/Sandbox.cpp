// Work folder and path mapping of schneidi-cli, see Sandbox.h
#include "cli/Sandbox.h"

#include "cli/CommandsDetail.h"
#include "cli/Live.h"
#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>

namespace Cli::Sandbox {

namespace {

using detail::fail;

struct Map {
    QString client;    // as given, without a trailing separator ("Z:\Clips")
    QString clientKey; // for comparing: '/' separators ("Z:/Clips")
    bool windows = false; // drive letter or backslashes: separators either way, case does not matter
    QString server;    // absolute, clean ("/data/clips")
};

struct State {
    QString root;      // absolute, clean
    QString canonical; // with symlinks resolved
    QVector<Map> maps;
    bool app = true;
};

State& state()
{
    static State s;
    return s;
}

constexpr Qt::CaseSensitivity fileCase()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

// `path` is `dir` or inside it
bool under(const QString& path, const QString& dir, Qt::CaseSensitivity cs = fileCase())
{
    if (dir.isEmpty()) return false;
    if (path.compare(dir, cs) == 0) return true;
    return path.startsWith(dir.endsWith('/') ? dir : dir + '/', cs);
}

// prefix + rest of a path ("" or starting with a separator, or after a prefix that ends with one)
QString join(const QString& prefix, QString rest, QChar sep = '/')
{
    if (rest.isEmpty()) return prefix;
    if (rest.startsWith(sep)) rest.remove(0, 1);
    return prefix.endsWith(sep) ? prefix + rest : prefix + sep + rest;
}

bool windowsStyle(const QString& p)
{
    return p.contains('\\') || (p.size() >= 2 && p[1] == ':' && p[0].isLetter());
}

QString applyMaps(const QString& path)
{
    const Map* best = nullptr;
    QString rest;
    for (const Map& m : state().maps) {
        const QString p = m.windows ? QString(path).replace('\\', '/') : path;
        if (!under(p, m.clientKey, m.windows ? Qt::CaseInsensitive : fileCase())) continue;
        if (best && best->clientKey.size() >= m.clientKey.size()) continue; // the longest prefix wins
        best = &m;
        rest = p.mid(m.clientKey.size());
    }
    return best ? QDir::cleanPath(join(best->server, rest)) : path;
}

[[noreturn]] void outside(const QString& path, const QString& why = {})
{
    fail("OUTSIDE_ROOT", QString("'%1' is outside the work folder %2%3 (relative paths start there; nothing outside "
                                 "it can be read or written)")
                             .arg(path, state().root, why));
}

} // namespace

void configure(const Settings& in)
{
    State s;
    s.app = in.app && in.root.isEmpty();
    if (!in.root.isEmpty()) {
        const QFileInfo fi(in.root);
        if (!fi.isDir()) fail("BAD_ARGUMENT", "work folder not found: " + in.root);
        s.root = QDir::cleanPath(fi.absoluteFilePath());
        s.canonical = fi.canonicalFilePath();
    }
    for (const QString& spec : in.maps) {
        const int eq = spec.indexOf('=');
        if (eq <= 0 || eq == spec.size() - 1)
            fail("BAD_ARGUMENT", "--map needs 'their path=path here', e.g. 'Z:\\Clips=/data/clips', got: " + spec);
        Map m;
        m.client = spec.left(eq);
        m.windows = windowsStyle(m.client);
        while (m.client.size() > 1 && (m.client.endsWith('/') || m.client.endsWith('\\'))) m.client.chop(1);
        m.clientKey = m.windows ? QString(m.client).replace('\\', '/') : m.client;
        const QString server = spec.mid(eq + 1);
        m.server = QDir::cleanPath(s.root.isEmpty() ? QFileInfo(server).absoluteFilePath()
                                                    : QDir(s.root).absoluteFilePath(server));
        s.maps << m;
    }
    state() = s;
    if (!s.app) Live::disable();
    if (!s.root.isEmpty()) QDir::setCurrent(s.root); // what still resolves against the working directory stays inside
}

bool active() { return !state().root.isEmpty(); }
QString root() { return state().root; }
bool appAllowed() { return state().app; }

QString resolve(const QString& given)
{
    if (given.isEmpty()) return given;
    if (Live::isAppProject(given)) {
        if (!state().app)
            fail("APP_OFF", "the live connection to the schneidi app is off here (server or work folder): give a "
                            "project file");
        return QString(Live::AppProject);
    }
    QString path = applyMaps(given);
#ifdef Q_OS_WIN
    path = QDir::fromNativeSeparators(path);
#endif
    const State& s = state();
    if (s.root.isEmpty()) return QFileInfo(path).absoluteFilePath();
    const QString abs = QDir::cleanPath(QDir(s.root).absoluteFilePath(path));
    if (!under(abs, s.root) && !under(abs, s.canonical)) outside(given);
    // Symlinks: the deepest existing part of the path must really be inside (a link pointing nowhere would create its
    // target when written to, wherever that is)
    for (QString probe = abs;;) {
        const QFileInfo fi(probe);
        if (fi.isSymLink() && !fi.exists()) outside(given, " (a link that points to a missing file)");
        if (fi.exists()) {
            if (!under(fi.canonicalFilePath(), s.canonical)) outside(given, " (through a link)");
            break;
        }
        const QString parent = fi.path();
        if (parent == probe) break;
        probe = parent;
    }
    return abs;
}

QString relative(const QString& absolutePath)
{
    const State& s = state();
    if (s.root.isEmpty()) return {};
    for (const QString& r : {s.root, s.canonical})
        if (under(absolutePath, r)) {
            const QString rest = absolutePath.mid(r.size()).section('/', 0, -1, QString::SectionSkipEmpty);
            return rest.isNull() ? QString("") : rest; // "" = the root, null = outside
        }
    return {};
}

QString mapBack(const QString& path)
{
    const Map* best = nullptr;
    for (const Map& m : state().maps)
        if (under(path, m.server) && (!best || m.server.size() > best->server.size())) best = &m;
    if (!best) return path;
    QString rest = path.mid(best->server.size());
    if (best->windows) return join(best->client, rest.replace('/', '\\'), '\\');
    return join(best->client, rest);
}

QJsonValue mapBack(const QJsonValue& v)
{
    if (state().maps.isEmpty()) return v;
    if (v.isString()) return mapBack(v.toString());
    if (v.isArray()) {
        QJsonArray a = v.toArray();
        for (int i = 0; i < a.size(); ++i) a[i] = mapBack(a[i]);
        return a;
    }
    if (v.isObject()) {
        QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) it.value() = mapBack(it.value());
        return o;
    }
    return v;
}

namespace {

// A file named in a project, as the engine will open it: MLT and FFmpeg take the string as written and read more than
// a plain path ("avformat:" or "http:" prefixes, "file.mp4?…" options, "%d" sequences, ".all.png" folders), and the
// system resolves ".." after links. So: a plain absolute path, inside the work folder (or `own`, the server's effects
// folder) also when nothing is there yet, and the deepest existing part of it really inside (links).
bool allowedFile(const QString& given, const QStringList& own)
{
    static const QRegularExpression scheme("^[A-Za-z][A-Za-z0-9+.-]+:"); // "avformat:", "file:" – not a drive "C:"
    const QString f = QDir::fromNativeSeparators(given);
    if (!QDir::isAbsolutePath(f) || scheme.match(f).hasMatch() || f.startsWith("//") || f.contains('?')
        || f.split('/').contains(".."))
        return false;
    const State& s = state();
    const QString clean = QDir::cleanPath(f);
    const auto inside = [&](const QString& path) {
        return under(path, s.root) || under(path, s.canonical)
               || std::any_of(own.begin(), own.end(), [&](const QString& o) { return under(path, o); });
    };
    if (!inside(clean)) return false;
    for (QString probe = clean;;) {
        const QFileInfo fi(probe);
        if (fi.isSymLink() && !fi.exists()) return false; // a link pointing nowhere
        if (fi.exists()) return inside(fi.canonicalFilePath());
        const QString parent = fi.path();
        if (parent == probe) return false;
        probe = parent;
    }
}

} // namespace

void checkProject(const Project& p)
{
    const State& s = state();
    if (s.root.isEmpty()) return;
    QStringList files;
    for (const MediaInfo& m : p.media()) files << m.path;
    for (const Sequence& q : p.sequences())
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (const Track& t : q.timeline.tracks(k))
                for (const Clip& c : t.clips) {
                    files << c.mediaPath << c.transInStyle.luma << c.transOutStyle.luma;
                    for (const EffectInstance& e : c.effects)
                        if (const EffectDescriptor* d = EffectRegistry::find(e.effectId))
                            for (const EffectParam& param : d->params)
                                if (param.type == EffectParam::Path) files << e.params.value(param.key).toString();
                }
    // The server's own LUTs and transition images (`effects` lists them) are allowed
    QStringList own{QDir::cleanPath(EffectFolders::root())};
    if (const QString c = QFileInfo(EffectFolders::root()).canonicalFilePath(); !c.isEmpty()) own << c;
    files.removeDuplicates();
    for (const QString& f : files) {
        if (f.isEmpty() || EffectFolders::isBuiltin(f) || allowedFile(f, own)) continue;
        // Also offline ones: the engine would still try to open them
        fail("OUTSIDE_ROOT", QString("the project uses a file outside the work folder %1: %2 (put it into the work "
                                     "folder – next to the project as it was on the other computer, then it is found "
                                     "there – or use a copy there)")
                                 .arg(s.root, f));
    }
}

} // namespace Cli::Sandbox
