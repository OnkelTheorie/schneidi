// SPDX-License-Identifier: AGPL-3.0-only
// Benchmark (not run by CTest): how fast the Media Storage shows the thumbnails of a folder.
//   bench_storage_thumbs <folder>
// Opens the folder in an offscreen Media Storage (900x600) and prints the time until the first visible thumbnail, all
// visible ones and all of them are there; then once more in the same process (folder changed and back). The disk
// cache lives under $BENCH_CACHE_HOME (XDG_CACHE_HOME): an empty folder = cold, the same folder again = warm (restart).
// BENCH_DUMP=<file.png> saves a picture of the panel once all thumbnails are there (BENCH_HEIGHT: a taller panel).
#include "app/Theme.h"
#include "ui/MediaStorage.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QListWidget>
#include <QTemporaryDir>

#include <cstdio>

namespace {

struct Times {
    qint64 firstVisible = -1, allVisible = -1, all = -1;
};

Times measure(MediaStorage& storage, const QString& folder)
{
    auto* list = storage.findChild<QListWidget*>();
    QElapsedTimer t;
    t.start();
    storage.setFolder(folder);
    // Done: no placeholder any more (placeholders are plain Theme::thumbBg with a sign in the middle)
    const auto done = [&](QListWidgetItem* it) {
        const QImage img = it->icon().pixmap(QSize(144, 81)).toImage();
        if (img.isNull()) return false;
        const QColor bg = Theme::thumbBg;
        return !(img.pixelColor(2, 2) == bg && img.pixelColor(141, 78) == bg && img.pixelColor(2, 78) == bg);
    };
    Times r;
    while (t.elapsed() < 600000) {
        const QRect view = list->viewport()->rect();
        int visible = 0, visibleDone = 0, allDone = 0;
        for (int i = 0; i < list->count(); ++i) {
            QListWidgetItem* it = list->item(i);
            const bool d = done(it);
            allDone += d;
            if (list->visualItemRect(it).intersects(view)) {
                ++visible;
                visibleDone += d;
            }
        }
        if (r.firstVisible < 0 && visibleDone > 0) r.firstVisible = t.elapsed();
        if (r.allVisible < 0 && visibleDone == visible) r.allVisible = t.elapsed();
        if (allDone == list->count()) {
            r.all = t.elapsed();
            break;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return r;
}

} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // HOME too: the panel starts in ~/Videos (MoviesLocation) and would make thumbnails of the files there
    if (qEnvironmentVariableIsSet("BENCH_CACHE_HOME")) {
        qputenv("XDG_CACHE_HOME", qgetenv("BENCH_CACHE_HOME"));
        qputenv("HOME", qgetenv("BENCH_CACHE_HOME"));
        qunsetenv("XDG_VIDEOS_DIR");
        qunsetenv("XDG_CONFIG_HOME");
    }
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("schneidi-bench");
    QCoreApplication::setApplicationName("schneidi-bench-storage-thumbs");
    if (argc < 2) {
        std::printf("usage: bench_storage_thumbs <folder>\n");
        return 2;
    }
    const QString folder = QDir(QString::fromLocal8Bit(argv[1])).absolutePath();
    QTemporaryDir empty;
    MediaStorage storage;
    storage.setPersistent(false);
    storage.resize(900, qEnvironmentVariableIntValue("BENCH_HEIGHT") > 0 ? qEnvironmentVariableIntValue("BENCH_HEIGHT") : 600);
    storage.show();
    QCoreApplication::processEvents();
    const int files = int(QDir(folder).entryList(QDir::Files).size());
    const Times first = measure(storage, folder);
    std::printf("open:   %d files, first visible %lld ms, all visible %lld ms, all %lld ms\n", files, first.firstVisible,
                first.allVisible, first.all);
    // BENCH_DUMP=<file.png>: a picture of the panel with all thumbnails
    if (qEnvironmentVariableIsSet("BENCH_DUMP")) storage.grab().save(qEnvironmentVariable("BENCH_DUMP"));
    storage.setFolder(empty.path());
    QCoreApplication::processEvents();
    const Times again = measure(storage, folder);
    std::printf("reopen: first visible %lld ms, all visible %lld ms, all %lld ms\n", again.firstVisible, again.allVisible,
                again.all);
    return 0;
}
