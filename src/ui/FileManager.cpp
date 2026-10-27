#include "ui/FileManager.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QUrl>

#ifdef SCHNEIDI_HAVE_DBUS
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#endif

namespace FileManager {

namespace {

void openFolder(const QString& path)
{
    const QFileInfo fi(path);
    QDesktopServices::openUrl(QUrl::fromLocalFile(fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath()));
}

} // namespace

void open(const QString& path)
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void reveal(const QString& path)
{
    const QString file = QFileInfo(path).absoluteFilePath();
#if defined(Q_OS_WIN)
    // One argument "/select,<path>": Explorer reads it itself, no shell in between
    if (!QProcess::startDetached("explorer.exe", {"/select," + QDir::toNativeSeparators(file)})) openFolder(file);
#elif defined(Q_OS_MACOS)
    if (!QProcess::startDetached("open", {"-R", file})) openFolder(file);
#elif defined(SCHNEIDI_HAVE_DBUS)
    // freedesktop file manager interface (Nautilus, Dolphin, Nemo, Thunar, Caja, PCManFM-Qt …). Without a session bus
    // (no desktop session) the call never answers, not even with an error: the folder at once.
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        openFolder(file);
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall("org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
                                                       "org.freedesktop.FileManager1", "ShowItems");
    call << QStringList{QUrl::fromLocalFile(file).toString()} << QString();
    auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(call, 3000));
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, [file](QDBusPendingCallWatcher* w) {
        if (QDBusPendingReply<>(*w).isError()) openFolder(file);
        w->deleteLater();
    });
#else
    openFolder(file);
#endif
}

} // namespace FileManager
