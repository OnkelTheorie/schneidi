// SPDX-License-Identifier: AGPL-3.0-only
// Benchmark (not a CTest test): probing media files on import (Engine::probe, engine/MediaProbe). Per file: median
// time; then an import of all files at once as the Media Pool does it (thread pool, half the cores). Same output lines
// as the 0.14 version ("own") so tools/bench.py compares across versions; the MLT column is gone with MLT.
// Run: QT_QPA_PLATFORM=offscreen ./build/tests/bench_probe <media files…> [--reps 3] [--rate 60]
#include "check.h"

#include "engine/Engine.h"
#include "engine/MediaProbe.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <vector>

namespace {

double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[v.size() / 2];
}

double importAll(const ProjectFormat& format, const QStringList& paths, MediaInfo (*probe)(const ProjectFormat&, const QString&))
{
    QElapsedTimer t;
    t.start();
    QVector<MediaInfo> infos(paths.size());
    QThreadPool pool;
    pool.setMaxThreadCount(std::clamp(QThread::idealThreadCount() / 2, 1, int(paths.size())));
    MediaInfo* out = infos.data();
    for (int i = 0; i < paths.size(); ++i) pool.start([out, &format, &paths, i, probe] { out[i] = probe(format, paths[i]); });
    pool.waitForDone();
    return t.nsecsElapsed() / 1e6;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("bench_probe");
    Check::initEngine();
    std::setlocale(LC_NUMERIC, "C"); // tools/bench.py reads the numbers
    QStringList files;
    int reps = 3;
    ProjectFormat format;
    const QStringList args = app.arguments().mid(1);
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == "--reps" && i + 1 < args.size()) reps = std::max(1, args[++i].toInt());
        else if (args[i] == "--rate" && i + 1 < args.size()) format.rate = {args[++i].toInt(), 1};
        else files << args[i];
    }
    double own = 0;
    int unreadable = 0;
    for (const QString& path : files) {
        std::vector<double> o;
        MediaInfo a;
        for (int r = 0; r < reps; ++r) {
            QElapsedTimer t;
            t.start();
            a = Engine::probe(format, path);
            o.push_back(t.nsecsElapsed() / 1e6);
        }
        own += median(o);
        unreadable += !a.hasVideo && !a.hasAudio && !a.isImage;
        std::printf("%8.2f ms  len=%lld video=%d audio=%d streams=%d  %s\n", median(o), qlonglong(a.length), a.hasVideo,
                    a.hasAudio, int(a.audioStreams.size()), qPrintable(QFileInfo(path).fileName()));
    }
    std::printf("%d files: own %.1f ms (%.2f per file); %d unreadable\n", int(files.size()), own,
                files.isEmpty() ? 0 : own / files.size(), unreadable);
    if (!files.isEmpty())
        std::printf("import of %d files (thread pool): own %.1f ms\n", int(files.size()),
                    importAll(format, files, &Engine::probe));
    return unreadable ? 1 : 0;
}
