// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/ContentUri.h"

#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QUrl>

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <unistd.h>
#endif

namespace ContentUri {

bool is(const QString& path) { return path.startsWith(QLatin1String("content://")); }

#ifdef Q_OS_ANDROID
namespace {

QJniObject resolver()
{
    QJniObject context = QNativeInterface::QAndroidApplication::context();
    return context.isValid() ? context.callObjectMethod("getContentResolver", "()Landroid/content/ContentResolver;")
                             : QJniObject();
}

QJniObject parse(const QString& uri)
{
    return QJniObject::callStaticObjectMethod("android/net/Uri", "parse", "(Ljava/lang/String;)Landroid/net/Uri;",
                                              QJniObject::fromString(uri).object<jstring>());
}

// A new descriptor for the address (-1 = refused); exceptions are cleared (and logged by QJniEnvironment)
int openFd(const QString& uri)
{
    QJniObject r = resolver();
    QJniObject u = parse(uri);
    if (!r.isValid() || !u.isValid()) return -1;
    QJniObject pfd = r.callObjectMethod("openFileDescriptor",
                                        "(Landroid/net/Uri;Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;",
                                        u.object(), QJniObject::fromString("r").object<jstring>());
    QJniEnvironment env;
    if (env.checkAndClearExceptions() || !pfd.isValid()) return -1;
    return pfd.callMethod<jint>("detachFd");
}

QMutex g_lock;
QHash<QString, int> g_fds; // address -> descriptor kept open for the run

} // namespace

QString openable(const QString& path)
{
    if (!is(path)) return path;
    QMutexLocker lock(&g_lock);
    int fd = g_fds.value(path, -1);
    if (fd < 0) {
        fd = openFd(path);
        if (fd < 0) return {};
        g_fds.insert(path, fd);
    }
    return QStringLiteral("/proc/self/fd/%1").arg(fd);
}

QString localPath(const QString& uri)
{
    if (!is(uri)) return uri;
    const int fd = openFd(uri);
    if (fd < 0) return {};
    char buf[4096];
    const ssize_t n = readlink(QByteArray("/proc/self/fd/" + QByteArray::number(fd)).constData(), buf, sizeof buf - 1);
    ::close(fd);
    if (n <= 0) return {};
    const QString path = QString::fromUtf8(buf, int(n));
    // Readable by path for this app (the media permission), a regular file
    const QFileInfo fi(path);
    return path.startsWith('/') && fi.isFile() && fi.isReadable() ? path : QString();
}

QString displayName(const QString& uri)
{
    if (!is(uri)) return QFileInfo(uri).fileName();
    static QMutex lock;
    static QHash<QString, QString> known; // (asked often: names, suffixes)
    {
        QMutexLocker l(&lock);
        if (const auto it = known.constFind(uri); it != known.cend()) return *it;
    }
    QJniObject r = resolver();
    QJniObject u = parse(uri);
    QString name;
    if (r.isValid() && u.isValid()) {
        QJniEnvironment env;
        QJniObject cursor = r.callObjectMethod(
            "query",
            "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;",
            u.object(), nullptr, nullptr, nullptr, nullptr);
        if (!env.checkAndClearExceptions() && cursor.isValid()) {
            if (cursor.callMethod<jboolean>("moveToFirst")) {
                const jint col = cursor.callMethod<jint>("getColumnIndex", "(Ljava/lang/String;)I",
                                                         QJniObject::fromString("_display_name").object<jstring>());
                if (col >= 0) name = cursor.callObjectMethod("getString", "(I)Ljava/lang/String;", col).toString();
            }
            cursor.callMethod<void>("close");
        }
        env.checkAndClearExceptions();
    }
    if (name.isEmpty()) name = QUrl::fromPercentEncoding(uri.section('/', -1).toUtf8()).section(':', -1);
    QMutexLocker l(&lock);
    known.insert(uri, name);
    return name;
}

QStringList forImport(const QStringList& paths)
{
    QStringList out;
    for (const QString& p : paths) {
        if (!is(p)) {
            out << p;
            continue;
        }
        if (const QString local = localPath(p); !local.isEmpty()) {
            out << local;
            continue;
        }
        // Kept as the address: its read permission for later runs (only for a picker's "open document" grant)
        QJniObject r = resolver();
        QJniObject u = parse(p);
        if (r.isValid() && u.isValid()) {
            r.callMethod<void>("takePersistableUriPermission", "(Landroid/net/Uri;I)V", u.object(), jint(1));
            QJniEnvironment env;
            env.checkAndClearExceptions();
        }
        out << p;
    }
    return out;
}

#else

QString openable(const QString& path) { return is(path) ? QString() : path; }
QString localPath(const QString& uri) { return is(uri) ? QString() : uri; }
QString displayName(const QString& uri) { return QFileInfo(uri).fileName(); }
QStringList forImport(const QStringList& paths) { return paths; }

#endif

} // namespace ContentUri
