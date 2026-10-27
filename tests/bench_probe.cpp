// Benchmark (not a CTest test): probing media files on import, own header reader (Engine::probe, engine/MediaProbe)
// against the MLT producer (Engine::probeMlt). Per file: median time of both and whether the results agree; then an
// import of all files at once as the Media Pool does it (thread pool, half the cores) with each.
// Run: QT_QPA_PLATFORM=offscreen ./build/tests/bench_probe <media files…> [--reps 3] [--rate 60]
#include "check.h"

#include "engine/Engine.h"
#include "engine/MediaProbe.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {

double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[v.size() / 2];
}

QString describe(const MediaInfo& m)
{
    QString s = QString("len=%1 video=%2 audio=%3 image=%4 [").arg(m.length).arg(m.hasVideo).arg(m.hasAudio).arg(m.isImage);
    for (const AudioStreamInfo& a : m.audioStreams) s += QString("%1ch '%2' ").arg(a.channels).arg(a.title);
    return s + "]";
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
    Check::initMlt();
    QStringList files;
    int reps = 3;
    ProjectFormat format;
    const QStringList args = app.arguments().mid(1);
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == "--reps" && i + 1 < args.size()) reps = std::max(1, args[++i].toInt());
        else if (args[i] == "--rate" && i + 1 < args.size()) format.rate = {args[++i].toInt(), 1};
        else files << args[i];
    }
    double own = 0, mlt = 0;
    int differ = 0, byMlt = 0;
    for (const QString& path : files) {
        std::vector<double> o, m;
        MediaInfo a, b;
        bool decided = false;
        for (int r = 0; r < reps; ++r) {
            QElapsedTimer t;
            t.start();
            a = Engine::probe(format, path);
            o.push_back(t.nsecsElapsed() / 1e6);
            t.restart();
            b = Engine::probeMlt(format, path);
            m.push_back(t.nsecsElapsed() / 1e6);
        }
        decided = MediaProbe::read(path, format.rate).has_value();
        own += median(o);
        mlt += median(m);
        const bool same = describe(a) == describe(b);
        differ += !same;
        byMlt += !decided;
        std::printf("%8.2f ms %8.2f ms  %s%s  %s\n", median(o), median(m), same ? "same" : "DIFFERENT",
                    decided ? "" : " (MLT)", qPrintable(QFileInfo(path).fileName()));
        if (!same) std::printf("    own: %s\n    MLT: %s\n", qPrintable(describe(a)), qPrintable(describe(b)));
    }
    std::printf("%d files: own %.1f ms (%.2f per file), MLT %.1f ms (%.2f per file); %d different, %d left to MLT\n",
                int(files.size()), own, files.isEmpty() ? 0 : own / files.size(), mlt,
                files.isEmpty() ? 0 : mlt / files.size(), differ, byMlt);
    if (!files.isEmpty()) {
        const double ownImport = importAll(format, files, &Engine::probe);
        const double mltImport = importAll(format, files, &Engine::probeMlt);
        std::printf("import of %d files (thread pool): own %.1f ms, MLT %.1f ms\n", int(files.size()), ownImport, mltImport);
    }
    return differ ? 1 : 0;
}
