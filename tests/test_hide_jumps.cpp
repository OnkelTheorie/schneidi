// SPDX-License-Identifier: AGPL-3.0-only
// Jump cuts (0.17 package "hide jumps"): core/JumpCut (clips of one take that skip a part of it, not a cut to
// another file, a speed change or a gap), the edit op hide_jumps (every second clip of a run pushed in, the first
// left, own zoom left out, centre on the subject of a synthetic clip with a skin-coloured patch) and the `check`
// info jump_cut that it answers.
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/ExportCheck.h"
#include "core/JumpCut.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/TimelineOps.h"

#include "check.h"

#include <QDir>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <memory>

namespace {

Clip media(int id, int start, int in, int length, const QString& path = "/x/a.mp4")
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.in = in;
    c.out = in + length - 1;
    return c;
}

// V1 at 25 fps: 1 [0,50) a 0..49 | 2 [50,100) a 80.. (30 skipped) | 3 [100,150) a 140.. (10 skipped: < 0.3 s? no,
// 10 frames = 0.4 s) | 4 [150,200) b.mp4 | 5 [200,250) b, 5 frames skipped (too short) | gap | 6 [300,350) b
Track track()
{
    Track t;
    t.clips << media(1, 0, 0, 50) << media(2, 50, 80, 50) << media(3, 100, 140, 50) << media(4, 150, 0, 50, "/x/b.mp4")
            << media(5, 200, 55, 50, "/x/b.mp4") << media(6, 300, 200, 50, "/x/b.mp4");
    return t;
}

void testFind()
{
    const QVector<JumpCut::Jump> j = JumpCut::find(track(), 8);
    if (CHECK_EQ(j.size(), 2)) {
        CHECK(j[0].left == 1 && j[0].right == 2 && j[0].at == 50 && j[0].skipped == 30);
        CHECK(j[1].left == 2 && j[1].right == 3 && j[1].skipped == 10);
    }
    CHECK_EQ(JumpCut::find(track(), 1).size(), 3); // 4|5 skips 5 frames
    Track slow = track();
    slow.clips[1].speed = 0.5;
    CHECK(JumpCut::find(slow, 8).isEmpty()); // retimed: no jump
    Clip a = media(1, 0, 0, 50), b = media(2, 50, 80, 50);
    CHECK(JumpCut::sameFraming(a, b));
    b.transform.zoomX = b.transform.zoomY = 1.12;
    CHECK(!JumpCut::sameFraming(a, b));
}

// ---------- schneidi-cli ----------

QString g_path;

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    return cmd->run(Cli::normalize(*cmd, raw), ctx);
}

std::unique_ptr<Project> loaded()
{
    ProjectData d;
    ProjectFile::load(g_path, &d, nullptr);
    auto p = std::make_unique<Project>();
    p->load(d);
    return p;
}

Clip clipOf(const Project& p, int id)
{
    const Clip* c = TimelineOps::findClip(p.timeline(), id);
    return c ? *c : Clip{};
}

void save(const Timeline& tl, const QVector<MediaInfo>& media)
{
    Project p;
    for (const MediaInfo& m : media) p.addMedia(m);
    p.load(ProjectData{ProjectFormat{}, 0, p.media(), {}, tl, 20, 0});
    QString error;
    CHECK(ProjectFile::save(p.data(), g_path, &error));
}

int jumpInfos(const Project& p)
{
    int n = 0;
    for (const ExportCheck::Issue& i : ExportCheck::jumpCuts(p.timeline(), 0, 400, p.format())) n += i.check == "jump_cut";
    return n;
}

void testOp(const QString& dir)
{
    g_path = dir + "/jumps.schneidi";
    Timeline tl;
    tl.video << track();
    tl.video[0].clips[2].transform.zoomX = tl.video[0].clips[2].transform.zoomY = 1.3; // own zoom: left out
    tl.audio.resize(1);
    save(tl, {MediaInfo{"/x/a.mp4", "a.mp4", 1000, true, true, false}, MediaInfo{"/x/b.mp4", "b.mp4", 1000, true, true, false}});
    CHECK_EQ(jumpInfos(*loaded()), 1); // 1|2 (2|3 has another framing already)
    const QJsonObject r = run("edit", {{"project", g_path}, {"ops", QJsonArray{QJsonObject{{"op", "hide_jumps"}, {"center", "middle"}}}}});
    const QString answer = QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact));
    if (!CHECK(answer.contains("2 jump cuts on V1, zoom 1.12 on clip 2"))) std::printf("       %s\n", qPrintable(answer));
    {
        const auto p = loaded();
        CHECK_EQ(clipOf(*p, 1).transform.zoomX, 1.0);  // the first of the run stays
        CHECK_EQ(clipOf(*p, 2).transform.zoomX, 1.12); // pushed in
        CHECK_EQ(clipOf(*p, 2).transform.posX, 0.0);   // middle
        CHECK_EQ(clipOf(*p, 3).transform.zoomX, 1.3);  // its own zoom stays
        CHECK_EQ(clipOf(*p, 4).transform.zoomX, 1.0);  // a cut to another file: no jump
        CHECK_EQ(clipOf(*p, 5).transform.zoomX, 1.0);
        CHECK_EQ(jumpInfos(*p), 0);
    }
    // Covered by B-roll on V2 at the cut: no jump_cut; a locked track: nothing zoomed, the answer says so
    {
        Timeline covered;
        covered.video << track();
        covered.video.resize(2);
        covered.video[1].clips << media(9, 40, 0, 20, "/x/c.mp4");
        covered.video[0].clips.resize(2);
        save(covered, {MediaInfo{"/x/a.mp4", "a.mp4", 1000, true, true, false}, MediaInfo{"/x/c.mp4", "c.mp4", 1000, true, false, false}});
        CHECK_EQ(jumpInfos(*loaded()), 0);
        Timeline locked = covered;
        locked.video.resize(1);
        locked.video[0].locked = true;
        save(locked, {MediaInfo{"/x/a.mp4", "a.mp4", 1000, true, true, false}});
        CHECK_EQ(jumpInfos(*loaded()), 1);
        const QJsonObject l = run("edit", {{"project", g_path}, {"ops", QJsonArray{QJsonObject{{"op", "hide_jumps"}, {"center", "middle"}}}}});
        CHECK(QJsonDocument(l).toJson().contains("locked track"));
        CHECK_EQ(clipOf(*loaded(), 2).transform.zoomX, 1.0);
        // A transform that is off with an old zoom kept: framing as shown (no zoom), so the jump counts
        Timeline off = covered;
        off.video.resize(1);
        off.video[0].clips[1].transform.zoomX = off.video[0].clips[1].transform.zoomY = 1.5;
        off.video[0].clips[1].transform.transformOn = false;
        save(off, {MediaInfo{"/x/a.mp4", "a.mp4", 1000, true, true, false}});
        CHECK_EQ(jumpInfos(*loaded()), 1);
    }

    // No jump at all: a note, nothing changes
    Timeline plain;
    plain.video.resize(1);
    plain.video[0].clips << media(1, 0, 0, 50) << media(2, 50, 0, 50, "/x/b.mp4");
    save(plain, {MediaInfo{"/x/a.mp4", "a.mp4", 1000, true, true, false}, MediaInfo{"/x/b.mp4", "b.mp4", 1000, true, true, false}});
    CHECK(QJsonDocument(run("edit", {{"project", g_path}, {"ops", QJsonArray{QJsonObject{{"op", "hide_jumps"}}}}}))
              .toJson()
              .contains("no jump cut"));

    // Centre on the subject: a skin-coloured, textured patch on the right of a grey picture
    if (!Check::haveFfmpeg()) return;
    const QString clip = Check::makeMedia(QDir(dir).filePath("talk.mp4"),
                                          {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=25:d=8", "-vf",
                                           "drawbox=x=0:y=0:w=320:h=180:color=gray:t=fill,"
                                           "drawbox=x=220:y=50:w=60:h=80:color=0xd8a080:t=fill,"
                                           "noise=alls=30:allf=t",
                                           "-c:v", "libx264", "-pix_fmt", "yuv420p"});
    if (!CHECK(!clip.isEmpty())) return;
    Timeline talk;
    talk.video.resize(1);
    talk.video[0].clips << media(1, 0, 0, 50, clip) << media(2, 50, 80, 50, clip) << media(3, 100, 150, 40, clip);
    save(talk, {MediaInfo{clip, "talk.mp4", 200, true, false, false}});
    run("edit", {{"project", g_path}, {"ops", QJsonArray{QJsonObject{{"op", "hide_jumps"}, {"zoom", 1.2}}}}});
    const auto p = loaded();
    const Clip z = clipOf(*p, 2);
    CHECK_EQ(z.transform.zoomX, 1.2);
    // The subject right of the middle stays there: the zoomed picture moves left (and still fills the frame)
    if (!CHECK(z.transform.posX < -5)) std::printf("       posX %.1f\n", z.transform.posX);
    CHECK(z.transform.posX >= -0.2 * p->format().width / 2 - 0.1);
    CHECK_EQ(clipOf(*p, 3).transform.zoomX, 1.0);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("hide-jumps");
    QTemporaryDir dir;
    testFind();
    testOp(dir.path());
    return Check::result();
}
