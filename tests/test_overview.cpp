// `overview` (cli/AnalysisOverview.cpp): windows on whole seconds (a short rest joins the last one), sound level and
// silent share, motion, cuts and brightness of a file (a cut from a moving test picture to black silence), the
// clips and sound of a project, several files.
#include "cli/Commands.h"

#include "check.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QTemporaryDir>

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("overview");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg missing");
    Check::initMlt();
    QTemporaryDir dir;
    qputenv("XDG_DATA_HOME", dir.filePath("data").toUtf8());
    // 4 s moving test picture with a tone, then 4 s black and silent (one cut at 4 s)
    const QString media = Check::makeMedia(
        dir.filePath("a.mp4"),
        {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=25:d=4", "-f", "lavfi", "-i", "color=c=black:s=320x180:r=25:d=4",
         "-f", "lavfi", "-i", "sine=f=440:d=4", "-f", "lavfi", "-i", "anullsrc=r=48000:cl=mono:d=4", "-filter_complex",
         "[0][2][1][3]concat=n=2:v=1:a=1[v][a]", "-map", "[v]", "-map", "[a]", "-c:v", "libx264", "-pix_fmt", "yuv420p",
         "-c:a", "aac"});
    if (!CHECK(!media.isEmpty())) return Check::result();
    const auto run = [](const QString& name, const QJsonObject& raw) {
        const Cli::Command* cmd = Cli::find(name);
        Cli::Context ctx;
        try {
            return cmd->run(Cli::normalize(*cmd, raw), ctx);
        } catch (const Cli::Error& e) {
            std::printf("%s: %s %s\n", qPrintable(name), qPrintable(e.code), qPrintable(e.message));
            return QJsonObject{{"failed", e.code}};
        }
    };

    QJsonObject r = run("overview", {{"file", media}, {"window", "2s"}});
    QJsonArray w = r["windows"].toArray();
    if (CHECK_EQ(w.size(), 4)) {
        CHECK(std::as_const(w)[0]["motion"].toInt() > 0);
        CHECK(std::as_const(w)[0]["brightness"].toInt() > 20);
        CHECK(std::as_const(w)[0]["silent_percent"].toInt() < 5);
        CHECK(std::as_const(w)[0]["sound_db"].toInt() > -30);
        CHECK_EQ(std::as_const(w)[2]["cuts"].toInt(), 1); // 4 s
        CHECK_EQ(std::as_const(w)[3]["motion"].toInt(), 0);
        CHECK(std::as_const(w)[3]["brightness"].toInt() < 8);
        CHECK_EQ(std::as_const(w)[3]["silent_percent"].toInt(), 100);
    }
    const QStringList lines = r["text"].toString().split('\n');
    if (CHECK_EQ(lines.size(), 4)) {
        CHECK(lines[0].startsWith("0:00-0:02  sound "));
        CHECK(lines[3].contains("silent"));
        CHECK(lines[3].contains("black"));
    }
    // Default window: about 1/20 of 8 s = whole seconds; a rest of 0.5 s joins the last window
    r = run("overview", {{"file", media}});
    CHECK_EQ(r["window_s"].toDouble(), 1.0);
    CHECK_EQ(r["windows"].toArray().size(), 8);
    r = run("overview", {{"file", media}, {"window", "3s"}});
    w = r["windows"].toArray();
    if (CHECK_EQ(w.size(), 3)) CHECK_EQ(std::as_const(w)[2]["to"].toInt(), 200); // 6..8 s, not 6..9
    r = run("overview", {{"file", media}, {"window", "2.5s"}});
    CHECK_EQ(r["windows"].toArray().size(), 3); // 7.5..8 s (0.5 s) joins 5..7.5

    // A project: which clips, how loud
    const QString project = dir.filePath("p.schneidi");
    run("new", {{"project", project}, {"width", 320}, {"height", 180}, {"media", QJsonArray{media}}});
    run("edit", {{"project", project}, {"ops", QString(R"([{"op":"place","media":"%1","at":0}])").arg(media)}});
    r = run("overview", {{"file", project}, {"window", "4s"}});
    w = r["windows"].toArray();
    if (CHECK_EQ(w.size(), 2)) {
        CHECK_EQ(std::as_const(w)[0]["clips"].toArray().size(), 1);
        CHECK(r["text"].toString().contains("a.mp4"));
        CHECK_EQ(std::as_const(w)[1]["silent_percent"].toInt(), 100);
    }
    // Several files: one overview each
    r = run("overview", {{"files", QJsonArray{media, media}}, {"window", "4s"}});
    CHECK_EQ(r["files"].toArray().size(), 2);
    CHECK_EQ(run("overview", {{"file", project}, {"window", 0}})["failed"].toString(), QString("BAD_ARGUMENT"));
    return Check::result();
}
