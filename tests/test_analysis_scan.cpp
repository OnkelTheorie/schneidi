// SPDX-License-Identifier: AGPL-3.0-only
// The analyses read the picture in-process (engine/video/Scan, cli/AnalysisPicture.cpp) instead of an ffmpeg process:
// same results as the ffmpeg way (SCHNEIDI_ANALYSIS=ffmpeg) on generated files long enough for several parallel time
// ranges (cuts, B-frames, a rotated phone video, variable frame rate). scenes and overview: identical. Region and
// colour events: identical with every picture decoded (region always, colour with SCHNEIDI_ANALYSIS=full); colour by
// default (non-reference pictures skipped) the same events at most 3 frames apart. Times of both ways are printed.
#include "cli/Commands.h"

#include "check.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QJsonArray>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <cmath>
#include <cstdlib>

namespace {

QJsonObject run(const QString& name, const QJsonObject& raw, const char* mode, double* ms = nullptr)
{
    if (mode) qputenv("SCHNEIDI_ANALYSIS", mode);
    else qunsetenv("SCHNEIDI_ANALYSIS");
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    QElapsedTimer t;
    t.start();
    QJsonObject out;
    try {
        out = cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        std::printf("%s: %s %s\n", qPrintable(name), qPrintable(e.code), qPrintable(e.message));
        out = QJsonObject{{"failed", e.code}};
    }
    if (ms) *ms = t.nsecsElapsed() / 1e6;
    qunsetenv("SCHNEIDI_ANALYSIS");
    return out;
}

QString frames(const QJsonArray& list)
{
    QStringList s;
    for (const QJsonValue& v : list) s << QString::number(v["frame"].toInt());
    return s.join(' ');
}

// The same frames, numbers within `slack` (a rotated picture is turned after scaling here, before it with ffmpeg:
// slightly different pixels)
bool same(const QJsonArray& a, const QJsonArray& b, double slack)
{
    if (a.size() != b.size()) return false;
    for (qsizetype i = 0; i < a.size(); ++i) {
        const QJsonObject x = a[i].toObject(), y = b[i].toObject();
        if (x.keys() != y.keys()) return false;
        for (const QString& k : x.keys()) {
            const bool exact = k == "frame" || k == "from" || k == "to" || k == "kind" || k.endsWith("tc") || k.endsWith("_s") || k == "s";
            if (exact ? x[k] != y[k] : std::abs(x[k].toDouble() - y[k].toDouble()) > slack) return false;
        }
    }
    return true;
}

// Every event of `want` has one of `got` at most `slack` frames away, and the other way round
bool near(const QJsonArray& want, const QJsonArray& got, int slack)
{
    const auto covered = [slack](const QJsonArray& a, const QJsonArray& b) {
        for (const QJsonValue& x : a) {
            bool found = false;
            for (const QJsonValue& y : b) found = found || std::abs(x["frame"].toInt() - y["frame"].toInt()) <= slack;
            if (!found) return false;
        }
        return true;
    };
    return covered(want, got) && covered(got, want);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("analysis_scan");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg missing");
    Check::initEngine();
    QTemporaryDir dir;
    qputenv("XDG_DATA_HOME", dir.filePath("data").toUtf8());

    // 32 s at 25 fps in 8 shots of different pictures (cuts every 4 s), x264 with B-frames, and in the top right
    // corner (kept black) a yellow box that is there in 2-second steps (region and colour events)
    QStringList inputs, labels;
    const char* sources[] = {"testsrc2", "smptebars", "mandelbrot", "rgbtestsrc", "testsrc", "color=c=0x305080",
                             "cellauto", "life"};
    for (int i = 0; i < 8; ++i) {
        inputs << "-f" << "lavfi" << "-t" << "4" << "-i"
               << QString("%1=s=320x180:r=25").arg(sources[i]).replace("=c=0x305080=", "=c=0x305080:");
        labels << QString("[%1:v]format=yuv420p,setsar=1[v%1];").arg(i);
    }
    QString graph = labels.join("");
    for (int i = 0; i < 8; ++i) graph += QString("[v%1]").arg(i);
    graph += "concat=n=8:v=1:a=0,drawbox=x=230:y=0:w=90:h=50:color=black:t=fill,drawbox=x=250:y=10:w=50:h=20:color=0xE7E371:t=fill:enable='lt(mod(t\\,4)\\,2)'[v]";
    const QStringList x264{"-c:v", "libx264", "-preset", "ultrafast", "-bf", "2", "-g", "50", "-pix_fmt", "yuv420p"};
    const QString cuts = Check::makeMedia(dir.filePath("cuts.mp4"), inputs + QStringList{"-filter_complex", graph, "-map", "[v]"} + x264);
    if (!CHECK(!cuts.isEmpty())) return Check::result();
    const QString rotated = Check::makeMedia(dir.filePath("rotated.mp4"),
                                             {"-display_rotation:v:0", "90", "-i", cuts, "-c", "copy"});
    const QString vfr = Check::makeMedia(dir.filePath("vfr.mkv"),
                                         {"-i", cuts, "-vf", "select='not(between(n\\,100\\,140))'", "-fps_mode", "vfr",
                                          "-c:v", "libx264", "-preset", "ultrafast", "-bf", "2"});

    for (const QString& file : {cuts, rotated, vfr}) {
        if (file.isEmpty()) continue;
        const QString name = QFileInfo(file).fileName();
        double ownMs = 0, ffMs = 0;
        // scenes: the same cuts with the same scores
        const QJsonObject own = run("scenes", {{"file", file}}, nullptr, &ownMs);
        const QJsonObject ff = run("scenes", {{"file", file}}, "ffmpeg", &ffMs);
        CHECK(own["cuts"].toArray().size() >= 6);
        Check::report(same(own["cuts"].toArray(), ff["cuts"].toArray(), file == rotated ? 0.01 : 0), "scenes == ffmpeg", __FILE__, __LINE__,
                      QString("%1\n       own: %2\n       ffmpeg: %3")
                          .arg(name, frames(own["cuts"].toArray()), frames(ff["cuts"].toArray())));
        std::printf("%-12s scenes  own %7.1f ms  ffmpeg %7.1f ms  cuts %s\n", qPrintable(name), ownMs, ffMs,
                    qPrintable(frames(own["cuts"].toArray())));
        // overview: the same windows (motion, cuts, brightness)
        const QJsonObject ownOv = run("overview", {{"file", file}, {"window", "2s"}}, nullptr, &ownMs);
        const QJsonObject ffOv = run("overview", {{"file", file}, {"window", "2s"}}, "ffmpeg", &ffMs);
        CHECK_EQ(ownOv["windows"].toArray().size(), 16);
        Check::report(same(ownOv["windows"].toArray(), ffOv["windows"].toArray(), file == rotated ? 1 : 0),
                      "overview == ffmpeg", __FILE__, __LINE__, name);
        std::printf("%-12s overview own %7.1f ms  ffmpeg %7.1f ms\n", qPrintable(name), ownMs, ffMs);

        // Region and colour events: the box appears every 4 s (and goes away 2 s later)
        const bool turned = file == rotated; // the corner is elsewhere in the upright picture
        const QString region = turned ? "0,0,0.25,0.25" : "0.75,0,0.25,0.25";
        for (const bool color : {false, true}) {
            QJsonObject a{{"file", file}, {"region", region}};
            if (color) {
                a["color"] = "#e7e371";
                a["only"] = "both";
            }
            const QJsonArray o = run("scenes", a, nullptr, &ownMs)["changes"].toArray();
            const QJsonArray full = run("scenes", a, "full")["changes"].toArray();
            const QJsonArray f = run("scenes", a, "ffmpeg", &ffMs)["changes"].toArray();
            const char* what = color ? "colour" : "region";
            CHECK(o.size() >= (color ? 14 : 7));
            Check::report(same(full, f, turned ? 1 : 0), "region events (every picture) == ffmpeg", __FILE__, __LINE__,
                          QString("%1 %2\n       own: %3\n       ffmpeg: %4").arg(name, what, frames(full), frames(f)));
            // region: every picture always (B-frames skipped gave uneven samples); colour: skipping, close
            Check::report(color ? near(f, o, 3) : same(o, f, turned ? 1 : 0), "region events (default) ~ ffmpeg",
                          __FILE__, __LINE__,
                          QString("%1 %2\n       own: %3\n       ffmpeg: %4").arg(name, what, frames(o), frames(f)));
            std::printf("%-12s %s own %7.1f ms  ffmpeg %7.1f ms  events %s\n", qPrintable(name), what, ownMs, ffMs,
                        qPrintable(frames(o)));
        }
    }

    // Region over moving content with B-frames: skipping them sampled unevenly, which looked like changes (12-18
    // events instead of 8 on a zoom)
    {
        const QString zoom = Check::makeMedia(
            dir.filePath("zoom.mp4"), {"-f", "lavfi", "-t", "12", "-i", "mandelbrot=s=320x180:r=30", "-c:v", "libx264",
                                       "-preset", "ultrafast", "-bf", "3", "-pix_fmt", "yuv420p"});
        if (CHECK(!zoom.isEmpty())) {
            const QJsonObject a{{"file", zoom}, {"region", "0.1,0.1,0.3,0.3"}};
            const QJsonArray o = run("scenes", a, nullptr)["changes"].toArray();
            const QJsonArray f = run("scenes", a, "ffmpeg")["changes"].toArray();
            Check::report(same(o, f, 0), "region events on moving B-frame video == ffmpeg", __FILE__, __LINE__,
                          QString("\n       own: %1\n       ffmpeg: %2").arg(frames(o), frames(f)));
        }
    }

    // check: black and frozen picture (2 s moving, 2 s black, 4 s still bars, 2 s moving, 3.5 s still to the end)
    {
        const QString shots = Check::makeMedia(
            dir.filePath("check.mp4"),
            {"-f", "lavfi", "-t", "2", "-i", "testsrc2=s=320x180:r=25", "-f", "lavfi", "-t", "2", "-i", "color=c=black:s=320x180:r=25",
             "-f", "lavfi", "-t", "4", "-i", "smptebars=s=320x180:r=25", "-f", "lavfi", "-t", "2", "-i", "testsrc2=s=320x180:r=25",
             "-f", "lavfi", "-t", "3.5", "-i", "rgbtestsrc=s=320x180:r=25", "-filter_complex",
             "[0][1][2][3][4]concat=n=5:v=1:a=0[v]", "-map", "[v]", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
        if (CHECK(!shots.isEmpty())) {
            const auto issues = [&](const char* mode) {
                QStringList out;
                for (const QJsonValue& v : run("check", {{"file", shots}, {"sound", false}}, mode)["issues"].toArray())
                    if (v["check"] == "black_file" || v["check"] == "frozen")
                        out << QString("%1 %2-%3").arg(v["check"].toString()).arg(v["from"].toInt()).arg(v["to"].toInt());
                out.sort();
                return out;
            };
            const QStringList own = issues(nullptr), ff = issues("ffmpeg");
            CHECK_EQ(own.size(), 3); // black 50-100, still bars 100-200, still to the end
            // The same ranges within a frame
            bool same = own.size() == ff.size();
            for (qsizetype i = 0; same && i < own.size(); ++i) {
                const QStringList a = own[i].split(QRegularExpression("[ -]")), b = ff[i].split(QRegularExpression("[ -]"));
                same = a.size() == 3 && b.size() == 3 && a[0] == b[0] && std::abs(a[1].toInt() - b[1].toInt()) <= 1
                       && std::abs(a[2].toInt() - b[2].toInt()) <= 1;
            }
            Check::report(same, "black/frozen == ffmpeg", __FILE__, __LINE__,
                          QString("\n       own: %1\n       ffmpeg: %2").arg(own.join(", "), ff.join(", ")));
        }
    }
    return Check::result();
}
