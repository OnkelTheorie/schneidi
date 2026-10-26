// Check before export (core/ExportCheck.h, CLI/MCP `check`): gaps without picture (also from hidden tracks and
// disabled clips), offline media, subtitles too short/too fast/past the end; sound: no sound, clipping, true peak with
// and without the limiter advice, loudness off the target, silence at the end/start/inside. Then the command on a
// small project and on its render.
#include "cli/Commands.h"
#include "core/ExportCheck.h"

#include "check.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QTemporaryDir>

#include <cmath>

namespace {

Clip clip(int id, int start, int length, const QString& media = {})
{
    Clip c;
    c.id = id;
    c.kind = media.isEmpty() ? ClipKind::Title : ClipKind::Media;
    c.mediaPath = media;
    c.start = start;
    c.out = length - 1;
    return c;
}

QStringList checks(const QVector<ExportCheck::Issue>& issues)
{
    QStringList out;
    for (const ExportCheck::Issue& i : issues) out << i.check;
    return out;
}

const ExportCheck::Issue* find(const QVector<ExportCheck::Issue>& issues, const QString& check)
{
    for (const ExportCheck::Issue& i : issues)
        if (i.check == check) return &i;
    return nullptr;
}

float lin(double db) { return float(std::pow(10.0, db / 20)); }

QStringList checksOf(const QJsonObject& result)
{
    QStringList out;
    for (const QJsonValue& v : result["issues"].toArray()) out << v["check"].toString();
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("check");
    const ProjectFormat f; // 1920x1080, 25 fps

    // --- Picture: a forgotten frame between clips, a hidden track and a disabled clip do not count as picture
    {
        Timeline tl;
        tl.video.resize(2);
        tl.video[0].clips << clip(1, 0, 50) << clip(2, 51, 49);
        tl.video[1].hidden = true;
        tl.video[1].clips << clip(3, 50, 1); // would cover the gap, but the track is hidden
        auto issues = ExportCheck::timeline(tl, 0, 100, f);
        CHECK_EQ(checks(issues), QStringList({"black"}));
        if (const auto* gap = find(issues, "black")) {
            CHECK_EQ(gap->from, 50);
            CHECK_EQ(gap->to, 51);
            CHECK(gap->severity == ExportCheck::Severity::Warning);
        }
        tl.video[1].hidden = false;
        CHECK(ExportCheck::timeline(tl, 0, 100, f).isEmpty());
        tl.video[1].clips[0].enabled = false;
        CHECK_EQ(checks(ExportCheck::timeline(tl, 0, 100, f)), QStringList({"black"}));
        // Black at the end of the range (sound goes on) and nothing outside the checked range
        CHECK_EQ(checks(ExportCheck::timeline(tl, 51, 120, f)), QStringList({"black"}));
        CHECK(ExportCheck::timeline(tl, 0, 50, f).isEmpty());
        // A timeline without any picture
        Timeline sound;
        sound.audio.resize(1);
        sound.audio[0].clips << clip(5, 0, 100, "/x/a.wav");
        CHECK(checks(ExportCheck::timeline(sound, 0, 100, f)).contains("black"));
    }

    // --- Offline media: an error, once per file, only clips in the range
    {
        Timeline tl;
        tl.video.resize(1);
        tl.audio.resize(1);
        tl.video[0].clips << clip(1, 0, 50, "/does/not/exist.mp4") << clip(2, 50, 50, "/does/not/exist.mp4");
        tl.audio[0].clips << clip(3, 0, 50, "/does/not/exist.mp4") << clip(4, 200, 10, "/other/missing.wav");
        const auto issues = ExportCheck::timeline(tl, 0, 100, f);
        CHECK_EQ(checks(issues), QStringList({"offline"}));
        CHECK_EQ(ExportCheck::errors(issues), 1);
    }

    // --- Subtitles: too short, too fast, past the end; disabled tracks are not exported
    {
        Timeline tl;
        tl.video.resize(1);
        tl.video[0].clips << clip(1, 0, 250);
        tl.subtitles.resize(1);
        tl.subtitles[0].cues << SubtitleCue{10, 0, 10, "Hi"}                                 // 0.4 s
                             << SubtitleCue{11, 25, 50, QString(40, 'x')}                     // 40 chars in 1 s
                             << SubtitleCue{12, 100, 150, "Fine to read"}
                             << SubtitleCue{13, 225, 300, "Cut off"};                          // past frame 250
        const auto issues = ExportCheck::timeline(tl, 0, 250, f);
        CHECK_EQ(checks(issues), QStringList({"subtitle_short", "subtitle_fast", "subtitle_end"}));
        tl.subtitles[0].enabled = false;
        CHECK(ExportCheck::timeline(tl, 0, 250, f).isEmpty());
    }

    // --- Sound
    {
        const std::vector<float> normal(250, lin(-12));
        CHECK(ExportCheck::sound(normal, 0, -14.2, -2.0, f).isEmpty());
        // Off the target: too quiet / too loud, the tolerance holds
        CHECK_EQ(checks(ExportCheck::sound(normal, 0, -20, -6, f)), QStringList({"loudness"}));
        CHECK(ExportCheck::sound(normal, 0, -15.5, -2, f).isEmpty());
        ExportCheck::Options broadcast;
        broadcast.targetLufs = -23;
        CHECK(ExportCheck::sound(normal, 0, -23.4, -9, f, broadcast).isEmpty());
        // True peak: above 0 = error, above -1 = warning; advice for the limiter only when it is off
        auto issues = ExportCheck::sound(normal, 0, -14, 0.8, f);
        CHECK_EQ(checks(issues), QStringList({"clipping"}));
        CHECK_EQ(ExportCheck::errors(issues), 1);
        issues = ExportCheck::sound(normal, 0, -14, -0.5, f);
        CHECK_EQ(checks(issues), QStringList({"true_peak"}));
        const QString advice = issues[0].message;
        issues = ExportCheck::sound(normal, 0, -14, -0.5, f, {}, true);
        CHECK(issues[0].message.size() < advice.size());
        // No sound at all
        CHECK_EQ(checks(ExportCheck::sound(std::vector<float>(250, 0.f), 0, -200, -200, f)), QStringList({"no_sound"}));
        CHECK_EQ(checks(ExportCheck::sound({}, 0, -200, -200, f)), QStringList({"no_sound"}));
        // Silence: 3 s at the end = warning, 3 s at the start = info, 6 s inside = info, 1 s inside = nothing
        std::vector<float> peaks(1000, lin(-12));
        for (int i = 0; i < 75; ++i) peaks[size_t(i)] = 0;               // start
        for (int i = 300; i < 450; ++i) peaks[size_t(i)] = 0;            // 6 s inside
        for (int i = 600; i < 625; ++i) peaks[size_t(i)] = lin(-70);     // 1 s inside
        for (int i = 925; i < 1000; ++i) peaks[size_t(i)] = 0;           // end
        issues = ExportCheck::sound(peaks, 100, -14, -3, f);
        CHECK_EQ(checks(issues), QStringList({"silence_start", "silence", "silence_end"}));
        if (const auto* end = find(issues, "silence_end")) {
            CHECK_EQ(end->from, 1025);
            CHECK_EQ(end->to, 1100);
            CHECK(end->severity == ExportCheck::Severity::Warning);
        }
        CHECK(find(issues, "silence_start")->severity == ExportCheck::Severity::Info);
    }

    // --- The command on a project and on its render
    if (!Check::haveFfmpeg()) return Check::result();
    Check::initMlt();
    QTemporaryDir dir;
    qputenv("XDG_DATA_HOME", dir.filePath("data").toUtf8()); // backups of the test project
    const QString media = Check::makeMedia(dir.filePath("a.mp4"),
                                           {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=25:d=4", "-f", "lavfi", "-i",
                                            "sine=f=440:d=4", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac",
                                            "-shortest"});
    if (!CHECK(!media.isEmpty())) return Check::result();
    const QString project = dir.filePath("p.schneidi"), out = dir.filePath("out.mp4");
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
    run("new", {{"project", project}, {"width", 320}, {"height", 180}, {"media", QJsonArray{media}}});
    run("edit", {{"project", project},
                 {"ops", QString(R"([{"op":"place","media":"%1","at":0},{"op":"place","media":"%1","at":"4.04s"},
                                    {"op":"subtitles","cues":[{"from":"1s","to":"1.2s","text":"Hi"}]}])")
                             .arg(media)}});
    QJsonObject r = run("check", {{"source", project}});
    CHECK_EQ(checksOf(r), QStringList({"loudness", "subtitle_short", "black"})); // warnings sorted by time
    CHECK_EQ(r["errors"].toInt(), 0);
    CHECK_EQ(r["warnings"].toInt(), 3);
    const QJsonArray issues = r["issues"].toArray();
    if (CHECK_EQ(issues.size(), 3)) {
        CHECK_EQ(issues[2]["from"].toInt(), 100);
        CHECK_EQ(issues[2]["to"].toInt(), 101);
        CHECK(issues[2]["from_tc"].isString());
    }
    const double lufs = r["sound"].toObject()["integrated_lufs"].toDouble();
    CHECK(lufs < -16);
    // Quick checks only, other target, a range without the gap
    r = run("check", {{"source", project}, {"sound", false}, {"to", 99}});
    CHECK_EQ(checksOf(r), QStringList({"subtitle_short"}));
    CHECK(!r.contains("sound"));
    r = run("check", {{"source", project}, {"target_lufs", lufs}, {"from", "2s"}, {"to", "3.9s"}});
    CHECK(checksOf(r).isEmpty());

    // The rendered file: the sound checks again; the gap of one frame is too short for the picture check of a file
    run("render", {{"project", project}, {"out", out}});
    r = run("check", {{"source", out}});
    CHECK_EQ(checksOf(r), QStringList({"loudness"}));
    CHECK_EQ(r["frames"].toInt(), 201);
    // A file with a long black stretch and no sound
    const QString black = Check::makeMedia(dir.filePath("black.mp4"),
                                           {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=25:d=2", "-f", "lavfi", "-i",
                                            "color=c=black:s=320x180:r=25:d=2", "-filter_complex", "[0][1]concat=n=2",
                                            "-c:v", "libx264", "-pix_fmt", "yuv420p"});
    r = run("check", {{"source", black}});
    CHECK_EQ(checksOf(r), QStringList({"no_audio", "black_file"}));
    if (const QJsonArray list = r["issues"].toArray(); list.size() == 2) {
        CHECK(std::abs(list[1]["from"].toInt() - 50) <= 1);
        CHECK(std::abs(list[1]["to"].toInt() - 100) <= 1);
    }

    return Check::result();
}
