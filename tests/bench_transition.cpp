// SPDX-License-Identifier: AGPL-3.0-only
// Benchmark (not a CTest test): the preview renderer over the first transition of a project, frame by frame as the
// playback pump asks for them (forward, `workers` frames in flight, Renderer::prepare now and then). Reports ms per
// frame before, inside and after the transition (mean / worst) and the frames that took longer than one frame time.
// Run: SCHNEIDI_GPU=0|1 QT_QPA_PLATFORM=offscreen build/tests/bench_transition <project.schneidi> [preview edge]
//      [workers] [--verbose]
#include "check.h"

#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/TimelineOps.h"
#include "engine/Lumas.h"
#include "engine/picture/Renderer.h"

#include <QApplication>
#include <QElapsedTimer>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("bench-transition");
    bool verbose = false;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--verbose") verbose = true;
        else args.push_back(argv[i]);
    }
    if (args.empty()) {
        std::printf("usage: bench_transition <project.schneidi> [preview edge] [workers] [--verbose]\n");
        return 2;
    }
    ProjectData data;
    QString error;
    if (!ProjectFile::load(QString::fromStdString(args[0]), &data, &error)) {
        std::printf("cannot load: %s\n", qPrintable(error));
        return 1;
    }
    Project project;
    project.load(data);
    const Timeline tl = project.timeline();
    const ProjectFormat fmt = project.format();
    const int edge = args.size() > 1 ? std::stoi(args[1]) : 960;
    const int workers = args.size() > 2 ? std::stoi(args[2]) : 3;
    const double fps = fmt.rate.fps();

    int tStart = -1, tEnd = -1;
    for (const Track& t : tl.video) {
        const auto spans = TimelineOps::transitions(t, [](const Clip&) { return 0; });
        if (!spans.isEmpty()) {
            tStart = spans.first().start;
            tEnd = spans.first().end;
            break;
        }
    }
    if (tStart < 0) {
        std::printf("no transition in the project\n");
        return 1;
    }
    const int from = std::max(0, tStart - int(2 * fps)), to = tEnd + int(1 * fps);

    Picture::Options o;
    o.format = fmt;
    o.mode = Picture::Mode::Preview;
    o.output = fmt.size().scaled(edge, edge, Qt::KeepAspectRatio);
    o.output = QSize(o.output.width() & ~1, o.output.height() & ~1);
    o.parallelFrames = workers;
    Picture::Renderer r(tl, o);
    r.setDirection(1);
    r.prepare(from); // as the engine does before playback starts
    r.render(from);  // warm up (open the first decoder)

    std::vector<double> ms(size_t(to - from + 1), 0.0);
    std::atomic<int> nextFrame{from + 1};
    std::atomic<bool> gpu{false};
    QElapsedTimer total;
    total.start();
    std::vector<std::thread> pool;
    for (int w = 0; w < workers; ++w)
        pool.emplace_back([&, w] {
            for (int f = nextFrame++; f <= to; f = nextFrame++) {
                if (w == 0 && f % 15 == 0) r.prepare(f); // the pump's idle workers call it every 250 ms
                QElapsedTimer t;
                t.start();
                const Picture::Frame fr = r.render(f);
                ms[size_t(f - from)] = t.nsecsElapsed() / 1e6;
                if (fr.gpu()) gpu = true;
            }
        });
    for (std::thread& t : pool) t.join();
    const double wall = total.nsecsElapsed() / 1e9;

    auto report = [&](const char* name, int a, int b) {
        double sum = 0, worst = 0;
        int n = 0, slow = 0;
        for (int f = std::max(a, from + 1); f < b && f <= to; ++f, ++n) {
            const double v = ms[size_t(f - from)];
            sum += v;
            worst = std::max(worst, v);
            if (v > workers * 1000.0 / fps) ++slow; // the pump has `workers` frame times per frame
        }
        std::printf("  %-12s frames %4d..%4d: mean %6.1f ms, worst %6.1f ms, %d over %.1f ms\n", name, a, b - 1,
                    n ? sum / n : 0.0, worst, slow, workers * 1000.0 / fps);
    };
    std::printf("%s: %dx%d preview, %d workers, %s path, %.3f fps, transition %d..%d\n", args[0].c_str(),
                o.output.width(), o.output.height(), workers, gpu ? "GPU" : "CPU", fps, tStart, tEnd - 1);
    report("before", from, tStart);
    report("transition", tStart, tEnd);
    report("after", tEnd, to + 1);
    std::printf("  wall %.2f s for %d frames = %.1f fps (needs %.1f)\n", wall, to - from, (to - from) / wall, fps);
    // The gradient of a luma transition alone (built per frame before the cache)
    for (const Track& t : tl.video)
        for (const Clip& c : t.clips)
            if (c.transOut > 0 && c.transOutStyle.isLuma()) {
                QElapsedTimer lt;
                lt.start();
                for (int k = 0; k < 5; ++k) Lumas::image(c.transOutStyle.luma, o.output.width(), o.output.height());
                std::printf("  luma gradient %s at %dx%d: %.1f ms\n", qPrintable(c.transOutStyle.luma), o.output.width(),
                            o.output.height(), lt.nsecsElapsed() / 5e6);
            }
    if (verbose)
        for (int f = from + 1; f <= to; ++f) std::printf("    %5d %7.1f ms%s\n", f, ms[size_t(f - from)],
                                                         f >= tStart && f < tEnd ? "  T" : "");
    return 0;
}
