// SPDX-License-Identifier: AGPL-3.0-only
// FileManager (ui/FileManager.h): "Show in file manager" and "Open with default program" (Media Pool, Media Storage,
// Render Queue) reach the system even without a D-Bus session on Linux (no file manager to ask: the folder opens).
// The system's opener is replaced by a URL handler that only records: no window opens, no real session is asked.
#include "check.h"

#include "ui/FileManager.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

namespace {

class Opened : public QObject {
    Q_OBJECT
public:
    QList<QUrl> urls;
public slots:
    void open(const QUrl& url) { urls << url; }
};

bool waitFor(const Opened& opened, int count, int ms = 5000)
{
    QElapsedTimer t;
    t.start();
    while (opened.urls.size() < count && t.elapsed() < ms) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return opened.urls.size() >= count;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    // No session bus: an address nothing listens on (the user's real session is never asked)
    qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/schneidi-test-bus");
    QApplication app(argc, argv);
    Check::initApp("file-manager");

    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    const QString file = QDir(tmp.path()).filePath("Timeline 1.mp4");
    QFile f(file);
    CHECK(f.open(QIODevice::WriteOnly) && f.write("x") == 1);
    f.close();

    Opened opened;
    QDesktopServices::setUrlHandler("file", &opened, "open");

    // Open with the default program: the file itself
    FileManager::open(file);
    if (CHECK(waitFor(opened, 1))) CHECK_EQ(opened.urls.first(), QUrl::fromLocalFile(file));

    // Show in the file manager: no file manager answers -> its folder opens (before: nothing happened)
    opened.urls.clear();
    FileManager::reveal(file);
    if (CHECK(waitFor(opened, 1)))
        CHECK_EQ(QFileInfo(opened.urls.first().toLocalFile()).canonicalFilePath(), QFileInfo(tmp.path()).canonicalFilePath());

    QDesktopServices::unsetUrlHandler("file");
    return Check::result();
}

#include "test_file_manager.moc"
