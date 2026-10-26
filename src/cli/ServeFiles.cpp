// `schneidi-cli serve`: files in and out (PUT/GET /files/<path>, tools `files` and `upload`), see Serve.h
#include "cli/ServeDetail.h"

#include "cli/CommandsDetail.h"
#include "cli/Sandbox.h"
#include "core/ProjectFile.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include <algorithm>

namespace Cli::Serve {

using detail::fail;

namespace {

QString fileType(const QFileInfo& fi)
{
    if (fi.isDir()) return "folder";
    if (fi.suffix().compare(ProjectFile::Extension, Qt::CaseInsensitive) == 0) return "project";
    static const QMimeDatabase db;
    const QString mime = db.mimeTypeForFile(fi, QMimeDatabase::MatchExtension).name();
    for (const char* kind : {"video", "audio", "image"})
        if (mime.startsWith(QLatin1String(kind) + '/')) return kind;
    return "file";
}

} // namespace

QJsonObject Server::entryJson(const QFileInfo& fi, const QString& base) const
{
    const QString path = QDir::cleanPath(fi.absoluteFilePath());
    QJsonObject o{{"name", fi.fileName()}, {"path", Sandbox::relative(path)}, {"type", fileType(fi)},
                  {"modified", fi.lastModified().toString(Qt::ISODate)}};
    if (!fi.isDir()) {
        o["size"] = fi.size();
        o["url"] = fileUrl(base, path);
    }
    return o;
}

QJsonObject Server::filesTool(const QJsonObject& args, const QString& base) const
{
    const QString path = args.contains("path") ? args["path"].toString() : Sandbox::root();
    const QFileInfo fi(path);
    if (!fi.exists()) fail("NOT_FOUND", "not found: " + Sandbox::mapBack(path));
    QJsonArray entries;
    bool truncated = false;
    if (fi.isDir()) {
        QVector<QFileInfo> found;
        QDirIterator it(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System,
                        args["recursive"].toBool() ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
        while (it.hasNext()) {
            it.next();
            const QFileInfo e = it.fileInfo();
            // Hidden files and folders (and what is in them) stay out, links out of the work folder too
            if (QDir(path).relativeFilePath(e.absoluteFilePath()).split('/').constFirst().startsWith('.')) continue;
            if (e.fileName().startsWith('.')) continue;
            try {
                Sandbox::resolve(e.absoluteFilePath());
            } catch (const Error&) {
                continue;
            }
            if (found.size() >= kListLimit) {
                truncated = true;
                break;
            }
            found << e;
        }
        std::sort(found.begin(), found.end(), [](const QFileInfo& a, const QFileInfo& b) {
            return a.absoluteFilePath().compare(b.absoluteFilePath(), Qt::CaseInsensitive) < 0;
        });
        for (const QFileInfo& e : found) entries << entryJson(e, base);
    } else {
        entries << entryJson(fi, base);
    }
    QJsonObject o{{"folder", Sandbox::relative(fi.isDir() ? path : fi.absolutePath())}, {"root", Sandbox::root()},
                  {"entries", entries}};
    if (truncated) o["truncated"] = true;
    return o;
}

// `givenPath` as the client wrote it: a trailing separator means a folder (normalizing drops it)
void Server::prepareUpload(Job& job, const QString& givenPath)
{
    const QUrl url(job.args["url"].toString());
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https") || url.host().isEmpty())
        fail("BAD_ARGUMENT", "url must be an http:// or https:// address");
    QString name = QFileInfo(url.path()).fileName().trimmed();
    if (name.isEmpty() || name == "." || name == ".." || name.startsWith('.')) name = "download";
    QString target = job.args["path"].toString();
    if (target.isEmpty()) target = QDir(Sandbox::root()).filePath(name);
    else if (QFileInfo(target).isDir() || givenPath.endsWith('/') || givenPath.endsWith('\\'))
        target = QDir(target).filePath(name);
    target = Sandbox::resolve(target); // the file name must not lead out either
    if (QFileInfo(target).isDir()) fail("BAD_ARGUMENT", "that is a folder: " + Sandbox::mapBack(target));
    if (QFileInfo::exists(target) && !job.args["overwrite"].toBool())
        fail("EXISTS", "file exists (pass overwrite to replace it): " + Sandbox::mapBack(target));
    job.args["path"] = target;
    job.writes = target; // two uploads to the same file: one after the other
    job.runHere = [this](Job* j) {
        const QString id = j->id;
        QTimer::singleShot(0, this, [this, id] { startUpload(id); });
    };
}

void Server::startUpload(const QString& id)
{
    const Job* j = m_jobs->find(id);
    if (!j || j->over()) return;
    checkHost(QUrl(j->args["url"].toString()), [this, id](const QString& error) {
        Job* job = m_jobs->find(id);
        if (!job || job->over()) return;
        if (!error.isEmpty()) m_jobs->end(job, Mcp::toolError("BAD_URL", error));
        else download(id);
    });
}

// A server others reach must not fetch from itself or its own network's link-local services (cloud metadata …) for
// them; on 127.0.0.1 the client is on this computer anyway
void Server::checkHost(const QUrl& url, const std::function<void(const QString& error)>& then)
{
    if (m_opt.listen.isLoopback()) {
        then({});
        return;
    }
    QHostInfo::lookupHost(url.host(), this, [then, host = url.host()](const QHostInfo& info) {
        if (info.error() != QHostInfo::NoError) {
            then("cannot resolve " + host + ": " + info.errorString());
            return;
        }
        for (const QHostAddress& a : info.addresses())
            if (a.isLoopback() || a.isLinkLocal() || a.isMulticast() || a == QHostAddress::AnyIPv4 || a == QHostAddress::AnyIPv6) {
                then("not allowed from this server: " + host + " (" + a.toString() + ")");
                return;
            }
        then({});
    });
}

void Server::download(const QString& id)
{
    Job* j = m_jobs->find(id);
    const QUrl url(j->args["url"].toString());
    const QString target = j->args["path"].toString();
    QDir().mkpath(QFileInfo(target).absolutePath());
    auto file = std::make_unique<QSaveFile>(target);
    if (!file->open(QIODevice::WriteOnly)) {
        m_jobs->end(j, Mcp::toolError("WRITE_FAILED", "cannot write " + Sandbox::mapBack(target)));
        return;
    }
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, "schneidi/" + QCoreApplication::applicationVersion().section(' ', 0, 0));
    QNetworkReply* reply = m_net->get(req);
    QSaveFile* out = file.release();
    out->setParent(reply); // dropped (without its temporary file) with the reply unless committed
    auto problem = std::make_shared<QJsonObject>();
    j->cancelHere = [reply] { reply->abort(); };
    // Redirects: the new address is checked like the first one, never from https down to http
    connect(reply, &QNetworkReply::redirected, reply, [this, reply, url, problem](const QUrl& to) {
        if (url.scheme() == "https" && to.scheme() != "https") {
            *problem = Mcp::toolError("BAD_URL", "redirect from https to " + to.scheme() + " refused");
            reply->abort();
            return;
        }
        checkHost(to, [reply = QPointer<QNetworkReply>(reply), problem](const QString& error) {
            if (!reply) return;
            if (error.isEmpty()) {
                emit reply->redirectAllowed();
                return;
            }
            *problem = Mcp::toolError("BAD_URL", error);
            reply->abort();
        });
    });
    connect(reply, &QNetworkReply::readyRead, reply, [this, reply, out, problem] {
        const QByteArray data = reply->readAll();
        if (out->size() + data.size() > m_opt.maxUpload)
            *problem = Mcp::toolError("TOO_LARGE", QString("larger than the limit of %1 MB (--max-upload)").arg(m_opt.maxUpload >> 20));
        else if (out->write(data) != data.size())
            *problem = Mcp::toolError("WRITE_FAILED", "cannot write the file (disk full?)");
        if (!problem->isEmpty()) reply->abort();
    });
    connect(reply, &QNetworkReply::downloadProgress, reply, [this, id](qint64 got, qint64 total) {
        if (Job* job = m_jobs->find(id); job && total > 0) m_jobs->setProgress(job, int(got * 100 / total));
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, out, id, target, problem] {
        reply->deleteLater();
        Job* job = m_jobs->find(id);
        if (!job || job->over()) return; // cancelled
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (!problem->isEmpty()) {
            m_jobs->end(job, *problem);
        } else if (reply->error() != QNetworkReply::NoError || status >= 400) {
            m_jobs->end(job, Mcp::toolError("DOWNLOAD_FAILED", reply->errorString()));
        } else {
            out->write(reply->readAll());
            if (!out->commit()) {
                m_jobs->end(job, Mcp::toolError("WRITE_FAILED", "cannot write " + Sandbox::mapBack(target)));
                return;
            }
            const QFileInfo fi(target);
            m_jobs->end(job, Mcp::toolResult({{"path", Sandbox::mapBack(target)}, {"size", fi.size()},
                                              {"url", fileUrl(job->baseUrl, target)}}));
        }
    });
}

// ---------- Files over HTTP ----------

QString Server::filePath(const QString& requestPath) const
{
    QString rel = requestPath.mid(int(qstrlen("/files")));
    while (rel.startsWith('/')) rel.remove(0, 1);
    return Sandbox::resolve(rel.isEmpty() ? QString(".") : rel);
}

void Server::filesHead(Http::Connection* c)
{
    const Http::Request& r = c->request();
    if (r.method == "GET" || r.method == "HEAD") return; // answered in onRequest
    if (r.method != "PUT") {
        Http::Headers h = cors(r);
        h << QPair<QByteArray, QByteArray>{"Allow", "GET, HEAD, PUT"};
        c->respondJson(405, errorJson("METHOD", "GET, HEAD or PUT"), h);
        return;
    }
    QString target;
    try {
        target = filePath(r.path);
    } catch (const Error& e) {
        c->respondJson(403, errorJson(e.code, e.message), cors(r));
        return;
    }
    const QFileInfo fi(target);
    if (target == Sandbox::root() || fi.isDir()) {
        c->respondJson(409, errorJson("IS_FOLDER", "that is a folder; PUT a file path"), cors(r));
        return;
    }
    if (r.header("if-none-match").trimmed() == "*" && fi.exists()) {
        c->respondJson(412, errorJson("EXISTS", "file exists"), cors(r));
        return;
    }
    if (r.length > m_opt.maxUpload) {
        c->respondJson(413, errorJson("TOO_LARGE", QString("larger than the limit of %1 MB (--max-upload)").arg(m_opt.maxUpload >> 20)),
                       cors(r));
        return;
    }
    QDir().mkpath(fi.absolutePath());
    Upload up{std::make_unique<QSaveFile>(target), fi.exists()};
    if (!up.file->open(QIODevice::WriteOnly)) {
        c->respondJson(500, errorJson("WRITE_FAILED", "cannot write " + Sandbox::relative(target)), cors(r));
        return;
    }
    c->streamBody(up.file.get());
    c->sendContinue();
    m_uploads[c] = std::move(up);
}

void Server::filesPutDone(Http::Connection* c)
{
    const Http::Request& r = c->request();
    auto it = m_uploads.find(c);
    if (it == m_uploads.end()) {
        c->respondJson(500, errorJson("INTERNAL", "upload lost"), cors(r));
        return;
    }
    Upload up = std::move(it->second);
    m_uploads.erase(it);
    const QString target = up.file->fileName();
    if (!up.file->commit()) {
        c->respondJson(500, errorJson("WRITE_FAILED", "cannot write " + Sandbox::relative(target)), cors(r));
        return;
    }
    const QFileInfo fi(target);
    c->respondJson(up.existed ? 200 : 201,
                   {{"ok", true}, {"path", Sandbox::relative(target)}, {"size", fi.size()},
                    {"url", fileUrl(baseUrl(r), target)}},
                   cors(r));
}

void Server::filesGet(Http::Connection* c)
{
    const Http::Request& r = c->request();
    QString target;
    try {
        target = filePath(r.path);
    } catch (const Error& e) {
        c->respondJson(403, errorJson(e.code, e.message), cors(r));
        return;
    }
    const QFileInfo fi(target);
    if (!fi.exists()) {
        c->respondJson(404, errorJson("NOT_FOUND", "not found: " + Sandbox::relative(target)), cors(r));
        return;
    }
    if (fi.isDir()) {
        c->respondJson(200, filesTool({{"path", target}}, baseUrl(r)), cors(r));
        return;
    }
    auto file = std::make_unique<QFile>(target);
    if (!file->open(QIODevice::ReadOnly)) {
        c->respondJson(500, errorJson("READ_FAILED", "cannot read " + Sandbox::relative(target)), cors(r));
        return;
    }
    const qint64 size = file->size();
    Http::Headers h = cors(r);
    h << QPair<QByteArray, QByteArray>{"Accept-Ranges", "bytes"};
    static const QMimeDatabase db;
    const QByteArray type = db.mimeTypeForFile(fi, QMimeDatabase::MatchExtension).name().toLatin1();
    // One byte range ("bytes=a-b", "bytes=a-", "bytes=-n"); several ranges: the whole file
    const QByteArray range = r.header("range").trimmed();
    if (range.startsWith("bytes=") && !range.contains(',')) {
        const QByteArray spec = range.mid(6).trimmed();
        const qsizetype dash = spec.indexOf('-');
        bool okA = true, okB = true;
        const QByteArray a = spec.left(dash).trimmed(), b = spec.mid(dash + 1).trimmed();
        qint64 from = -1, to = size - 1;
        if (dash < 0) {
            okA = false;
        } else if (a.isEmpty()) {
            const qint64 n = b.toLongLong(&okB);
            if (okB && n > 0) from = std::max<qint64>(0, size - n);
        } else {
            from = a.toLongLong(&okA);
            if (!b.isEmpty()) to = std::min(b.toLongLong(&okB), size - 1);
        }
        if (!okA || !okB || from < 0 || from >= size || to < from) {
            h << QPair<QByteArray, QByteArray>{"Content-Range", "bytes */" + QByteArray::number(size)};
            c->respondJson(416, errorJson("RANGE", "range not satisfiable"), h);
            return;
        }
        h << QPair<QByteArray, QByteArray>{"Content-Range", QString("bytes %1-%2/%3").arg(from).arg(to).arg(size).toLatin1()};
        c->sendFile(206, std::move(file), from, to - from + 1, type, h);
        return;
    }
    c->sendFile(200, std::move(file), 0, size, type, h);
}

} // namespace Cli::Serve
