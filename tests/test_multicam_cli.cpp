// Multicam in schneidi-cli / MCP (docs/plan-0.8.md): edit ops multicam (sync start/in/marker/sound), source_marker,
// angle (cut/switch, by number or name, modes), angle_cuts (a list of switch points at once, e.g. from an AI that
// found the kills per POV), flatten, multicam_nudge, angle_name; info/timelines show angles; the sync command
// measures offsets of files without a project. Errors name the op/cut and save nothing.
#include "cli/Commands.h"
#include "core/Project.h"
#include "core/ProjectFile.h"

#include "check.h"

#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtEndian>

#include <cmath>

namespace {

QString g_path;

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    if (!cmd) {
        qWarning() << "no command" << name;
        return {};
    }
    Cli::Context ctx;
    try {
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        qWarning().noquote() << "unexpected error" << e.code << e.message;
        return {{"error", e.code}, {"message", e.message}};
    }
}

QString errorOf(const QString& name, const QJsonObject& raw, QString* message = nullptr)
{
    try {
        const Cli::Command* cmd = Cli::find(name);
        if (!cmd) return "NO_COMMAND";
        Cli::Context ctx;
        cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        if (message) *message = e.message;
        return e.code;
    }
    return "none";
}

QJsonObject edit(const QString& ops) { return run("edit", {{"project", g_path}, {"ops", ops}}); }
QString editError(const QString& ops, QString* message = nullptr)
{
    return errorOf("edit", {{"project", g_path}, {"ops", ops}}, message);
}

ProjectData saved()
{
    ProjectData d;
    QString error;
    ProjectFile::load(g_path, &d, &error);
    return d;
}

// Video track 1 of the saved project as "M<angle>[start-end]" (multicam) or the file's base name
QString v1()
{
    QString s;
    for (const Clip& c : saved().timeline.video.value(0).clips)
        s += QString("%1%2[%3-%4]")
                 .arg(s.isEmpty() ? "" : " ")
                 .arg(c.isMulticam() ? QString("M%1").arg(c.angle + 1) : QFileInfo(c.mediaPath).baseName())
                 .arg(c.start)
                 .arg(c.end());
    return s;
}

// Empty project with three POVs (not on disk: the ops here need only their lengths) and two markers
void writeProject()
{
    Project p;
    ProjectData d;
    d.timeline = emptyTimeline();
    p.load(d);
    p.addMedia({"/x/pov1.mp4", "pov1.mp4", 1000, true, true, false});
    p.addMedia({"/x/pov2.mp4", "pov2.mp4", 1000, true, true, false});
    p.addMedia({"/x/pov3.mp4", "pov3.mp4", 800, true, true, false});
    QString error;
    CHECK(ProjectFile::save(p.data(), g_path, &error));
}

QJsonObject multicamTimeline(const QJsonObject& result)
{
    for (const QJsonValue& t : result.value("timelines").toArray())
        if (t["multicam"].toBool()) return t.toObject();
    return {};
}

// ---- Creating ----

void testMulticamOpStartSyncCreatesAndPlaces()
{
    writeProject();
    const QJsonObject r = edit(R"([{"op":"multicam","name":"Match","sync":"start",
                                   "files":["/x/pov1.mp4","/x/pov2.mp4","/x/pov3.mp4"],"at":"2s"}])");
    CHECK_EQ(v1(), QString("M1[50-1050]"));
    const QJsonObject clip = r["timeline"]["tracks"][0]["clips"][0].toObject();
    CHECK_EQ(clip["kind"].toString(), QString("multicam"));
    CHECK_EQ(clip["angle"].toInt(), 1); // 1-based like the grid and the keys
    CHECK_EQ(clip["angle_name"].toString(), QString("pov1"));
    const QJsonObject mc = multicamTimeline(r);
    CHECK_EQ(mc["name"].toString(), QString("Match"));
    CHECK_EQ(clip["multicam"].toInt(), mc["id"].toInt());
    const QJsonArray angles = mc["angles"].toArray();
    if (!CHECK_EQ(angles.size(), 3)) return;
    CHECK_EQ(angles[2]["angle"].toInt(), 3);
    CHECK_EQ(angles[2]["files"][0]["path"].toString(), QString("/x/pov3.mp4"));
    CHECK_EQ(angles[2]["files"][0]["start"].toInt(), 0);
    // Not placed without `at`; a second multicam clip numbered on
    const QJsonObject r2 = edit(R"([{"op":"multicam","sync":"start","angles":[{"name":"A","files":["/x/pov1.mp4"]},
                                                                            {"name":"B","files":["/x/pov2.mp4"]}]}])");
    CHECK_EQ(v1(), QString("M1[50-1050]"));
    int multicams = 0;
    for (const QJsonValue& t : r2["timelines"].toArray()) multicams += t["multicam"].toBool();
    CHECK_EQ(multicams, 2);
}

void testSourceMarkerAndMarkerSync()
{
    writeProject();
    edit(R"([{"op":"source_marker","media":"/x/pov1.mp4","at":40},
             {"op":"source_marker","media":"/x/pov2.mp4","at":"0.4s"},
             {"op":"source_marker","media":"/x/pov2.mp4","at":500},
             {"op":"source_marker","media":"/x/pov2.mp4","at":500,"remove":true}])");
    const ProjectData d = saved();
    CHECK_EQ(d.media.value(0).markers, QVector<int>({40}));
    CHECK_EQ(d.media.value(1).markers, QVector<int>({10}));
    const QJsonObject info = run("info", {{"project", g_path}});
    CHECK_EQ(info["media"][1]["markers"].toArray().size(), 1);

    const QJsonObject r = edit(R"([{"op":"multicam","sync":"marker","angles":[{"name":"A","files":["/x/pov1.mp4"]},
                                   {"name":"B","files":["/x/pov2.mp4"]},{"name":"C","files":["/x/pov3.mp4"]}]}])");
    const QJsonArray angles = multicamTimeline(r)["angles"].toArray();
    if (!CHECK_EQ(angles.size(), 3)) return;
    CHECK_EQ(angles[0]["files"][0]["start"].toInt(), 0);
    CHECK_EQ(angles[1]["files"][0]["start"].toInt(), 30); // markers 40 and 10 line up
    CHECK(angles[1]["files"][0]["synced"].toBool());
    CHECK(!angles[2]["files"][0]["synced"].toBool());     // pov3 has no marker
    bool noted = false;
    for (const QJsonValue& n : r["notes"].toArray()) noted = noted || n.toString().contains("pov3");
    CHECK(noted); // the AI is told which angle is not synced

    CHECK_EQ(editError(R"([{"op":"source_marker","media":"/x/nope.mp4","at":1}])"), QString("NOT_FOUND"));
    CHECK_EQ(editError(R"([{"op":"source_marker","media":"/x/pov1.mp4","at":1000}])"), QString("BAD_ARGUMENT"));
}

// ---- Switching ----

void testAngleOpCutAndSwitch()
{
    writeProject();
    edit(R"([{"op":"multicam","sync":"start","at":0,"angles":[{"name":"Anna","files":["/x/pov1.mp4"]},
             {"name":"Ben","files":["/x/pov2.mp4"]},{"name":"Cem","files":["/x/pov3.mp4"]}]}])");
    edit(R"([{"op":"angle","at":300,"angle":2}])");
    CHECK_EQ(v1(), QString("M1[0-300] M2[300-1000]"));
    edit(R"([{"op":"angle","at":"16s","angle":"Cem"}])"); // by name, 16 s = frame 400
    CHECK_EQ(v1(), QString("M1[0-300] M2[300-400] M3[400-1000]"));
    edit(R"([{"op":"angle","at":350,"angle":1,"cut":false}])"); // switch the segment, no cut
    CHECK_EQ(v1(), QString("M1[0-300] M1[300-400] M3[400-1000]"));
    edit(R"([{"op":"angle","at":500,"angle":2,"mode":"video"}])");
    const ProjectData d = saved();
    CHECK_EQ(d.timeline.audio.value(0).clips.value(3).angle, 2); // the sound stayed Cem
    CHECK_EQ(d.timeline.video.value(0).clips.value(3).angle, 1);

    QString msg;
    CHECK_EQ(editError(R"([{"op":"angle","at":100,"angle":0}])"), QString("BAD_ARGUMENT"));
    CHECK_EQ(editError(R"([{"op":"angle","at":100,"angle":9}])", &msg), QString("BAD_ARGUMENT"));
    CHECK(msg.contains("3")); // tells how many angles there are
    CHECK_EQ(editError(R"([{"op":"angle","at":100,"angle":"Dora"}])"), QString("BAD_ARGUMENT"));
    CHECK_EQ(editError(R"([{"op":"angle","at":100,"angle":1,"mode":"both_ways"}])"), QString("BAD_ARGUMENT"));
    CHECK_EQ(editError(R"([{"op":"angle","at":5000,"angle":1}])"), QString("NOT_FOUND")); // no multicam clip there
}

void testAngleCutsList()
{
    writeProject();
    edit(R"([{"op":"multicam","sync":"start","at":0,"angles":[{"name":"Anna","files":["/x/pov1.mp4"]},
             {"name":"Ben","files":["/x/pov2.mp4"]},{"name":"Cem","files":["/x/pov3.mp4"]}]}])");
    // Unsorted, mixed forms: applied in time order
    const QJsonObject r = edit(R"([{"op":"angle_cuts","cuts":[[300,"Cem"],{"at":"4s","angle":2},[600,1]]}])");
    CHECK_EQ(v1(), QString("M1[0-100] M2[100-300] M3[300-600] M1[600-1000]"));
    CHECK(r.contains("backup")); // one save for the whole list
    // A bad cut names its index; nothing of the list is applied
    const QString before = v1();
    QString msg;
    CHECK_EQ(editError(R"([{"op":"angle_cuts","cuts":[[700,2],[800,7]]}])", &msg), QString("BAD_ARGUMENT"));
    CHECK(msg.contains("cut 1"));
    CHECK_EQ(editError(R"([{"op":"angle_cuts","cuts":[[700,2],[700,3]]}])", &msg), QString("BAD_ARGUMENT")); // same frame twice
    CHECK_EQ(editError(R"([{"op":"angle_cuts","cuts":[]}])"), QString("BAD_ARGUMENT"));
    CHECK_EQ(editError(R"([{"op":"angle_cuts","cuts":[[700]]}])"), QString("BAD_ARGUMENT"));
    CHECK_EQ(v1(), before);
    // Video only, for one given multicam clip
    const int id = saved().timeline.video.value(0).clips.value(0).id;
    edit(QString(R"([{"op":"angle_cuts","clip":%1,"mode":"video","cuts":[[50,3]]}])").arg(id));
    CHECK_EQ(v1(), QString("M1[0-50] M3[50-100] M2[100-300] M3[300-600] M1[600-1000]"));
    CHECK_EQ(saved().timeline.audio.value(0).clips.value(1).angle, 0);
}

void testFlattenNudgeAndAngleName()
{
    writeProject();
    const QJsonObject r = edit(R"([{"op":"multicam","name":"M","sync":"start","at":0,
                                   "files":["/x/pov1.mp4","/x/pov2.mp4"]},{"op":"angle","at":300,"angle":2}])");
    const int seq = multicamTimeline(r)["id"].toInt();
    // Rename and resync from outside (by id or name of the multicam clip)
    edit(QString(R"([{"op":"angle_name","timeline":%1,"angle":2,"name":"Ben"},
                     {"op":"multicam_nudge","timeline":"M","angle":"Ben","by":5}])").arg(seq));
    const ProjectData d = saved();
    for (const Sequence& s : d.sequences)
        if (s.id == seq) {
            CHECK_EQ(s.angles.value(1).name, QString("Ben"));
            CHECK_EQ(s.timeline.video.value(1).clips.value(0).start, 5);
        }
    CHECK_EQ(editError(R"([{"op":"multicam_nudge","timeline":"M","angle":3,"by":5}])"), QString("BAD_ARGUMENT"));
    CHECK_EQ(editError(R"([{"op":"angle_name","timeline":"Timeline 1","angle":1,"name":"x"}])"), QString("BAD_ARGUMENT"));

    // Flatten: normal media clips of the angles
    const int second = saved().timeline.video.value(0).clips.value(1).id;
    edit(QString(R"([{"op":"flatten","clips":[%1]}])").arg(second));
    CHECK_EQ(v1(), QString("M1[0-300] pov2[300-1000]"));
    CHECK_EQ(editError(R"([{"op":"flatten","clips":[999]}])"), QString("NOT_FOUND"));
    const int first = saved().timeline.video.value(0).clips.value(1).id; // the flattened (media) clip
    CHECK_EQ(editError(QString(R"([{"op":"flatten","clips":[%1]}])").arg(first)), QString("FAILED"));
}

void testMulticamErrors()
{
    writeProject();
    CHECK_EQ(editError(R"([{"op":"multicam","sync":"start","files":["/x/nope.mp4"]}])"), QString("NOT_FOUND"));
    CHECK_EQ(editError(R"([{"op":"multicam","sync":"start","files":[]}])"), QString("BAD_ARGUMENT"));
    CHECK_EQ(editError(R"([{"op":"multicam","sync":"start"}])"), QString("BAD_ARGUMENT"));
    CHECK_EQ(editError(R"([{"op":"multicam","sync":"clapper","files":["/x/pov1.mp4"]}])"), QString("BAD_ARGUMENT"));
    CHECK_EQ(editError(R"([{"op":"multicam","sync":"start","stream":-1,"files":["/x/pov1.mp4"]}])"), QString("BAD_ARGUMENT"));
    CHECK_EQ(editError(R"([{"op":"multicam","sync":"start","angles":[{"name":"A"}]}])"), QString("BAD_ARGUMENT"));
    CHECK_EQ(saved().sequences.size(), 1);
    // The edit help describes every new op
    for (const char* op : {"multicam", "angle", "angle_cuts", "flatten", "multicam_nudge", "angle_name", "source_marker"})
        CHECK_EQ(errorOf("help", {{"command", "edit"}, {"op", op}}), QString("none"));
    CHECK(Cli::find("sync"));
}

// ---- Sound (decoding) ----

uint32_t hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// Mono WAV with short noise bursts (each burst its own sound, same in every file) at the given times
bool burstWav(const QString& path, double length, const QVector<QPair<double, int>>& bursts, int seed)
{
    const int rate = 48000;
    std::vector<qint16> pcm(size_t(length * rate));
    for (size_t i = 0; i < pcm.size(); ++i) {
        double s = 0.005 * (double(hash(uint32_t(seed) * 977u + uint32_t(i))) / 2147483648.0 - 1.0);
        for (const auto& [t, kind] : bursts) {
            const double d = double(i) / rate - t;
            if (d >= 0 && d < 0.2)
                s += 0.5 * std::exp(-d / 0.04) * (double(hash(uint32_t(kind) * 7919u + uint32_t(d * rate))) / 2147483648.0 - 1.0);
        }
        pcm[i] = qint16(std::clamp(s, -1.0, 1.0) * 32767);
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    const quint32 data = quint32(pcm.size() * 2);
    auto le32 = [&](quint32 v) { char b[4]; qToLittleEndian(v, b); f.write(b, 4); };
    auto le16 = [&](quint16 v) { char b[2]; qToLittleEndian(v, b); f.write(b, 2); };
    f.write("RIFF"); le32(36 + data); f.write("WAVEfmt "); le32(16); le16(1); le16(1); le32(rate); le32(rate * 2);
    le16(2); le16(16); f.write("data"); le32(data);
    for (qint16 v : pcm) le16(quint16(v));
    return true;
}

void testSyncCommandAndSoundSync(const QString& dir)
{
    if (!Check::haveFfmpeg()) return;
    // Shared bursts (round start, spike …) every ~1.3 s; B started 1.6 s after A and has its own bursts too
    QVector<QPair<double, int>> shared, a, b;
    for (int k = 0; k < 14; ++k) shared << qMakePair(1.0 + 1.3 * k + 0.17 * (k % 3), k + 1);
    for (const auto& [t, kind] : shared) {
        a << qMakePair(t, kind);
        if (t - 1.6 > 0) b << qMakePair(t - 1.6, kind);
    }
    for (int k = 0; k < 10; ++k) b << qMakePair(0.4 + 1.1 * k, 500 + k);
    const QString wa = dir + "/a.wav", wb = dir + "/b.wav";
    if (!CHECK(burstWav(wa, 20, a, 1) && burstWav(wb, 16, b, 2))) return;
    const QString va = Check::makeMedia(dir + "/povA.mp4", {"-f", "lavfi", "-i", "color=red:size=160x90:rate=25:duration=20", "-i", wa,
                                                            "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
                                                            "-c:a", "aac", "-shortest"});
    const QString vb = Check::makeMedia(dir + "/povB.mkv", {"-f", "lavfi", "-i", "color=blue:size=160x90:rate=25:duration=16", "-i", wb,
                                                            "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
                                                            "-c:a", "pcm_s16le", "-shortest"});
    const QString vc = Check::makeMedia(dir + "/mute.mp4", {"-f", "lavfi", "-i", "color=lime:size=160x90:rate=25:duration=5",
                                                            "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!va.isEmpty() && !vb.isEmpty() && !vc.isEmpty())) return;

    const QJsonObject s = run("sync", {{"files", QJsonArray{va, vb, vc}}});
    const QJsonArray files = s["files"].toArray();
    CHECK_EQ(s["reference"].toString(), va);
    if (!CHECK_EQ(files.size(), 3)) return;
    CHECK(files[1]["found"].toBool());
    CHECK(std::abs(files[1]["offset_s"].toDouble() - 1.6) < 0.02);
    CHECK_EQ(files[1]["offset_frames"].toInt(), 40);
    CHECK(files[1]["confidence"].toDouble() > 0);
    CHECK(!files[2]["found"].toBool());
    CHECK_EQ(errorOf("sync", {{"files", QJsonArray{va}}}), QString("BAD_ARGUMENT")); // needs two files
    CHECK_EQ(errorOf("sync", {{"files", QJsonArray{va, dir + "/missing.mp4"}}}), QString("NOT_FOUND"));

    // multicam with sound sync imports the files and places B 40 frames after A; the silent file is reported
    QTemporaryDir pdir;
    g_path = pdir.filePath("sound.schneidi");
    CHECK(run("new", {{"project", g_path}}).contains("project"));
    const QJsonObject r = run("edit", {{"project", g_path},
                                       {"ops", QString(R"([{"op":"multicam","files":["%1","%2","%3"],"at":0}])").arg(va, vb, vc)}});
    const QJsonArray angles = multicamTimeline(r)["angles"].toArray();
    if (!CHECK_EQ(angles.size(), 3)) return;
    CHECK_EQ(angles[1]["files"][0]["start"].toInt() - angles[0]["files"][0]["start"].toInt(), 40);
    CHECK_EQ(angles[1]["files"][0]["via"].toString(), QString("sound"));
    CHECK(!angles[2]["files"][0]["synced"].toBool());
    bool noted = false;
    for (const QJsonValue& n : r["notes"].toArray()) noted = noted || n.toString().contains("mute.mp4");
    CHECK(noted);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("multicam-cli");
    QTemporaryDir dir;
    g_path = dir.filePath("t.schneidi");

    testMulticamOpStartSyncCreatesAndPlaces();
    testSourceMarkerAndMarkerSync();
    testAngleOpCutAndSwitch();
    testAngleCutsList();
    testFlattenNudgeAndAngleName();
    testMulticamErrors();
    Check::initMlt();
    testSyncCommandAndSoundSync(dir.path());
    return Check::result();
}
