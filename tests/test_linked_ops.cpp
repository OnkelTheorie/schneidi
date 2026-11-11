// SPDX-License-Identifier: AGPL-3.0-only
// Ops on linked clips (AI test run 2026-11-07, a recording with five sound streams): naming clip ids acts on exactly
// those. 'delete' of sound clips removes only them (their picture and the other streams stay, still linked), a picture
// takes its sound along as before; 'speed' with freeze:true stops the named picture and its linked sound plays on in
// sync (linked:true: silent too), each with a note saying what happened to the partners.
#include "check.h"
#include "timeline_guard.h"

#include "cli/Commands.h"
#include "core/ProjectFile.h"
#include "core/TimelineOps.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

namespace {

QString g_project;

QJsonObject run(const QString& name, const QJsonObject& raw, QString* error = nullptr)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        if (error) *error = e.code + ": " + e.message;
        else std::printf("       unexpected error %s: %s\n", qPrintable(e.code), qPrintable(e.message));
        return {};
    }
}

QJsonObject edit(const QJsonArray& ops, QString* error = nullptr)
{
    return run("edit", {{"project", g_project}, {"ops", ops}}, error);
}

Timeline load()
{
    ProjectData d;
    QString error;
    ProjectFile::load(g_project, &d, &error);
    CHECK_TIMELINE(d.timeline);
    return d.timeline;
}

QString notesOf(const QJsonObject& r) { return QString::fromUtf8(QJsonDocument(r.value("notes").toArray()).toJson()); }

// The picture and the sound clips of the first placement: {video, a0, a1, a2}
QVector<int> ids(const Timeline& tl)
{
    QVector<int> out{tl.video.value(0).clips.value(0).id};
    for (const Track& t : tl.audio)
        if (!t.clips.isEmpty()) out << t.clips.first().id;
    return out;
}

void setUp(const QString& file)
{
    QFile::remove(g_project);
    run("new", {{"project", g_project}, {"width", 320}, {"height", 180}, {"fps", 25}});
    edit({QJsonObject{{"op", "append"}, {"media", file}}});
}

void testDeleteSoundOnly(const QString& file)
{
    setUp(file);
    Timeline tl = load();
    QVector<int> c = ids(tl);
    if (!CHECK_EQ(c.size(), 4)) return;
    // Two unwanted streams in one op: only they go, the picture and the third stream stay linked
    const QJsonObject r = edit({QJsonObject{{"op", "delete"}, {"clips", QJsonArray{c[1], c[2]}}}});
    tl = load();
    CHECK(TimelineOps::findClip(tl, c[0]) && TimelineOps::findClip(tl, c[3]));
    CHECK(!TimelineOps::findClip(tl, c[1]) && !TimelineOps::findClip(tl, c[2]));
    CHECK_EQ(TimelineOps::linkedGroup(tl, c[0]).size(), 2);
    if (!CHECK(notesOf(r).contains("only the named sound clips"))) std::printf("       %s\n", qPrintable(notesOf(r)));
    // A picture still takes its sound along
    edit({QJsonObject{{"op", "delete"}, {"clips", QJsonArray{c[0]}}}});
    tl = load();
    CHECK(!TimelineOps::findClip(tl, c[0]) && !TimelineOps::findClip(tl, c[3]));
    // linked:true on a sound clip: the whole group, as before
    setUp(file);
    c = ids(load());
    edit({QJsonObject{{"op", "delete"}, {"clips", QJsonArray{c[2]}}, {"linked", true}}});
    tl = load();
    for (int id : c) CHECK(!TimelineOps::findClip(tl, id));
}

void testFreezePictureOnly(const QString& file)
{
    setUp(file);
    QVector<int> c = ids(load());
    if (!CHECK_EQ(c.size(), 4)) return;
    const QJsonObject r = edit({QJsonObject{{"op", "speed"}, {"clips", QJsonArray{c[0]}}, {"freeze", true}}});
    Timeline tl = load();
    const Clip* v = TimelineOps::findClip(tl, c[0]);
    CHECK(v && v->freeze);
    for (int i = 1; i < 4; ++i) {
        const Clip* a = TimelineOps::findClip(tl, c[i]);
        CHECK(a && !a->freeze && a->enabled && v && a->start == v->start && a->length() == v->length());
    }
    CHECK_EQ(TimelineOps::linkedGroup(tl, c[0]).size(), 4); // still linked
    if (!CHECK(notesOf(r).contains("picture only"))) std::printf("       %s\n", qPrintable(notesOf(r)));
    // Only sound named: a clear answer
    QString why;
    edit({QJsonObject{{"op", "speed"}, {"clips", QJsonArray{c[1]}}, {"freeze", true}}}, &why);
    CHECK(why.contains("freeze stops a picture"));
    // linked:true: the sound freezes too (silent), as a selection of picture and sound does in the app
    setUp(file);
    c = ids(load());
    edit({QJsonObject{{"op", "speed"}, {"clips", QJsonArray{c[0]}}, {"freeze", true}, {"linked", true}}});
    tl = load();
    for (int id : c) CHECK(TimelineOps::findClip(tl, id) && TimelineOps::findClip(tl, id)->freeze);
}

// The streams deleted by op come back with the next piece of the same recording (the import presets say what is
// placed), on their own tracks: no new tracks
void testNextPieceKeepsTheTracks(const QString& file)
{
    setUp(file);
    QVector<int> c = ids(load());
    if (!CHECK_EQ(c.size(), 4)) return;
    edit({QJsonObject{{"op", "delete"}, {"clips", QJsonArray{c[1], c[2]}}}});
    const QJsonObject r = edit({QJsonObject{{"op", "append"}, {"media", file}}});
    const Timeline tl = load();
    CHECK_EQ(tl.audio.size(), 3);
    for (int i = 0; i < 3; ++i) CHECK_EQ(tl.audio.value(i).clips.last().audioStream, i);
    CHECK(!notesOf(r).contains("left out"));
}

// A freeze on a locked track changes nothing
void testFreezeLockedTrack(const QString& file)
{
    setUp(file);
    QVector<int> c = ids(load());
    if (!CHECK_EQ(c.size(), 4)) return;
    {
        ProjectData d;
        QString error;
        ProjectFile::load(g_project, &d, &error);
        d.timeline.video[0].locked = true;
        CHECK(ProjectFile::save(d, g_project, &error));
    }
    edit({QJsonObject{{"op", "speed"}, {"clips", QJsonArray{c[0]}}, {"freeze", true}}});
    const Timeline tl = load();
    const Clip* v = TimelineOps::findClip(tl, c[0]);
    CHECK(v && !v->freeze);
}

} // namespace

// A ripple that would take picture and sound apart fails with OUT_OF_SYNC and a reason (the project stays); one that
// takes out less time than deleted says so in a note
void testRippleKeepsSync(const QString& file)
{
    setUp(file);
    edit({QJsonObject{{"op", "append"}, {"media", file}}});
    Timeline tl = load();
    QVector<int> c = ids(tl);
    if (!CHECK_EQ(c.size(), 4)) return;
    // The first Mix sound alone with ripple: its picture and other streams stay in the deleted time, so V1/A2/A3 cannot
    // move up while A1 would - the second recording's picture and Mix would come apart
    QString why;
    edit({QJsonObject{{"op", "delete"}, {"clips", QJsonArray{c[1]}}, {"ripple", true}, {"linked", false}}}, &why);
    if (!CHECK(why.startsWith("OUT_OF_SYNC") && why.contains("rec.mkv"))) std::printf("       %s\n", qPrintable(why));
    CHECK(load() == tl);
    // The whole first recording: everything moves up, no note
    const QJsonObject r = edit({QJsonObject{{"op", "delete"}, {"clips", QJsonArray{c[0]}}, {"ripple", true}}});
    CHECK_EQ(load().video.value(0).clips.value(0).start, 0);
    CHECK(!notesOf(r).contains("delete:"));
}

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("linked_ops");
    if (!Check::haveFfmpeg()) {
        std::printf("no ffmpeg: skipped\n");
        return Check::result();
    }
    QTemporaryDir dir;
    // A recording with a picture and three titled sound streams
    const QString file = Check::makeMedia(
        dir.filePath("rec.mkv"),
        {"-f", "lavfi", "-i", "color=c=gray:s=64x64:r=25:d=2", "-f", "lavfi", "-i", "sine=f=440:d=2", "-f", "lavfi",
         "-i", "sine=f=550:d=2", "-f", "lavfi", "-i", "sine=f=660:d=2", "-map", "0", "-map", "1", "-map", "2", "-map",
         "3", "-c:v", "ffv1", "-c:a", "pcm_s16le", "-metadata:s:a:0", "title=Mix", "-metadata:s:a:1", "title=Discord",
         "-metadata:s:a:2", "title=Game"});
    if (!CHECK(!file.isEmpty())) return Check::result();
    Check::initEngine();
    g_project = dir.filePath("p.schneidi");
    testDeleteSoundOnly(file);
    testFreezePictureOnly(file);
    testNextPieceKeepsTheTracks(file);
    testFreezeLockedTrack(file);
    testRippleKeepsSync(file);
    return Check::result();
}
