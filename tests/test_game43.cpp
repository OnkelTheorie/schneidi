// SPDX-License-Identifier: AGPL-3.0-only
// 4:3 game footage in 16:9 recordings: black bar detection (pillarbox, letterbox, a dark scene, a small overlay in the
// bar, a dark video without bars), Auto-Reframe keeping to the picture between the bars (rendered: no black at the
// edges, the path stays inside), crop_bars with undo, the stretched-4:3 correction (Clip::pictureAspect: a stretched
// circle renders round in a 16:9 and a 4:3 timeline, crop keeps its share, project round trip) and the CLI ops.
#include "check.h"

#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/Editor.h"
#include "core/MediaAspect.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Reframe.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/BarDetect.h"
#include "engine/Engine.h"
#include "engine/ReframeAnalysis.h"
#include "engine/Snapshot.h"

#include <QApplication>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QUndoStack>
#include <clocale>
#include <cmath>

namespace {

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

// Project with one clip of `file` on V1 in a timeline of `size`
void setup(Project& p, const QString& file, int frames, QSize size)
{
    p.addMedia({file, QFileInfo(file).fileName(), frames, true, false, false});
    if (size != p.format().size()) p.setSequenceSize(p.currentSequence(), size);
    p.edit("setup", [&](Timeline& tl) {
        Clip v;
        v.id = p.newClipId();
        v.mediaPath = file;
        v.out = frames - 1;
        tl.video[0].clips << v;
    });
}

QImage render(Project& p, int frame)
{
    return Snapshot::Source::timeline(p.timelineFormat(), p.timeline())->frame(frame).convertToFormat(QImage::Format_RGB32);
}

// Columns at the left and right edges (and rows top/bottom) that are black all the way
int blackEdge(const QImage& img)
{
    const auto dark = [&](int x, int y) { return qGray(img.pixel(x, y)) < 24; };
    int n = 0;
    for (int x : {0, 1, img.width() - 2, img.width() - 1}) {
        bool all = true;
        for (int y = 0; y < img.height() && all; y += 4) all = dark(x, y);
        n += all;
    }
    for (int y : {0, 1, img.height() - 2, img.height() - 1}) {
        bool all = true;
        for (int x = 0; x < img.width() && all; x += 4) all = dark(x, y);
        n += all;
    }
    return n;
}

// Width / height of the dark blob (the circle) in a white picture
double blobAspect(const QImage& img, QRect* box = nullptr)
{
    int x0 = img.width(), x1 = -1, y0 = img.height(), y1 = -1;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            if (qGray(img.pixel(x, y)) >= 100) continue;
            // only inside the white picture: black bars beside it are no blob (white left and right of it)
            bool left = false, right = false;
            for (int k = x; k >= 0 && !left; --k) left = qGray(img.pixel(k, y)) > 200;
            for (int k = x; k < img.width() && !right; ++k) right = qGray(img.pixel(k, y)) > 200;
            if (!left || !right) continue;
            x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
        }
    if (x1 < 0) return 0;
    if (box) *box = QRect(QPoint(x0, y0), QPoint(x1, y1));
    return double(x1 - x0 + 1) / (y1 - y0 + 1);
}

void testDetect(const QString& pillar, const QString& letter, const QString& darkPillar, const QString& overlay,
                const QString& dark)
{
    QString error;
    const auto p = BarDetect::content(pillar, &error);
    if (CHECK(p.has_value())) {
        CHECK(near(p->x(), 0.125, 0.006));
        CHECK(near(p->width(), 0.75, 0.01));
        CHECK(near(p->y(), 0, 1e-6));
        CHECK(near(p->height(), 1, 1e-6));
        CHECK(BarDetect::hasBars(*p));
    }
    CHECK(BarDetect::cached(pillar).has_value()); // stored
    const auto l = BarDetect::content(letter);
    if (CHECK(l.has_value())) {
        CHECK(near(l->y(), 0.125, 0.008));
        CHECK(near(l->height(), 0.75, 0.016));
        CHECK(near(l->x(), 0, 1e-6));
    }
    // A dark scene (the game picture far darker than usual, bright only now and then) keeps its columns
    const auto d = BarDetect::content(darkPillar);
    if (CHECK(d.has_value())) {
        CHECK(near(d->x(), 0.125, 0.006));
        CHECK(near(d->width(), 0.75, 0.01));
    }
    // A small overlay inside the bar (a webcam corner, a logo) does not end the bar
    const auto o = BarDetect::content(overlay);
    if (CHECK(o.has_value())) CHECK(near(o->x(), 0.125, 0.006));
    // Dark all through: nothing to tell bars from, the whole picture
    const auto z = BarDetect::content(dark);
    if (CHECK(z.has_value())) CHECK(!BarDetect::hasBars(*z));
    CHECK(!BarDetect::content("/does/not/exist.mp4", &error).has_value());
}

void testReframe(const QString& pillar)
{
    // 9:16 short of a pillarboxed 4:3 game: the bars are cropped, the content fills the frame, the measured path
    // stays inside the content
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    setup(p, pillar, 75, QSize(1080, 1920));
    QVector<Clip> clips{p.timeline().video[0].clips[0]};
    QHash<QString, QRectF> contents;
    const auto paths = ReframeAnalysis::paths(clips, p.timelineFormat(), 0.5, [](const QString&) { return 75; }, nullptr,
                                              nullptr, nullptr, &contents);
    CHECK(contents.contains(pillar) && BarDetect::hasBars(contents.value(pillar)));
    CHECK_EQ(ed.reframeClips({}, Reframe::Mode::Auto, paths, contents), 1);
    const Clip& c = p.timeline().video[0].clips[0];
    const QSizeF fitted = MediaAspect::fit(MediaAspect::ofClip(c), QSizeF(1080, 1920));
    CHECK(near(c.transform.cropLeft, 0.125 * fitted.width(), 0.01 * fitted.width()));
    CHECK(near(c.transform.cropRight, 0.125 * fitted.width(), 0.01 * fitted.width()));
    // Zoom: the content (3/4 of the picture) fills the 9:16 height - the same as the whole picture would
    CHECK(near(c.transform.zoomX, 1920 / fitted.height(), 1e-6));
    // Every position keeps the visible window inside the content: |x| <= (content width * zoom - frame width) / 2
    const double limit = (0.75 * fitted.width() * c.transform.zoomX - 1080) / 2 + 0.5;
    CHECK(std::abs(c.transform.posX) <= limit);
    for (const Keyframe& k : c.keys.value(AnimParam::PosX)) CHECK(std::abs(k.value) <= limit);
    // Rendered: no black column or row at the edges (the subject moves along the left edge of the content)
    for (int f : {5, 35, 70}) CHECK_EQ(blackEdge(render(p, f)), 0);

    // 4:3 timeline, center mode: the content fills it exactly; without bars handling the bars show
    Project q;
    Editor eq(&q, &sel);
    setup(q, pillar, 75, QSize(1440, 1080));
    CHECK_EQ(eq.reframeClips({}, Reframe::Mode::Center, {}, contents), 1);
    CHECK_EQ(blackEdge(render(q, 10)), 0);
    q.undoStack()->undo();
    CHECK_EQ(eq.reframeClips({}, Reframe::Mode::Fit, {}, {}), 1);
    const QImage bars = render(q, 10);
    CHECK(qGray(bars.pixel(3, bars.height() / 2)) < 24); // without: fitted with its bars, they show left and right

    // Fit: the content fitted in (a 4:3 picture in 9:16 is as wide as the frame), the bars cropped
    Project r;
    Editor er(&r, &sel);
    setup(r, pillar, 75, QSize(1080, 1920));
    CHECK_EQ(er.reframeClips({}, Reframe::Mode::Fit, {}, contents), 1);
    const Clip& f = r.timeline().video[0].clips[0];
    CHECK(f.transform.fill == FitFill::Blur);
    const QSizeF fit = MediaAspect::fit(16.0 / 9, QSizeF(1080, 1920));
    CHECK(near(f.transform.zoomX * 0.75 * fit.width(), 1080, 1));
    CHECK(f.transform.cropLeft > 0);
}

void testCropBars(const QString& pillar)
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    setup(p, pillar, 75, QSize(1920, 1080));
    const int before = p.undoStack()->index();
    QHash<QString, QRectF> contents{{pillar, *BarDetect::content(pillar)}};
    CHECK_EQ(ed.cropBars({}, contents), 1);
    CHECK_EQ(p.undoStack()->index(), before + 1);
    const Clip& c = p.timeline().video[0].clips[0];
    CHECK(near(c.transform.cropLeft, 240, 12));
    CHECK(near(c.transform.cropRight, 240, 12));
    CHECK_EQ(c.transform.cropTop, 0.0);
    CHECK_EQ(ed.cropBars({}, contents), 0); // already
    CHECK_EQ(ed.cropBars({}, {{pillar, QRectF(0, 0, 1, 1)}}), 0); // no bars: nothing
    p.undoStack()->undo();
    CHECK_EQ(p.timeline().video[0].clips[0].transform.cropLeft, 0.0);
}

void testStretched(const QString& dir, const QString& stretched)
{
    // A circle of a 4:3 game stretched to 16:9: oval as the file is, round with pictureAspect 4:3
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    setup(p, stretched, 50, QSize(1920, 1080));
    QRect box;
    CHECK(near(blobAspect(render(p, 5), &box), 4.0 / 3, 0.05));
    const int before = p.undoStack()->index();
    CHECK_EQ(ed.setPictureAspect({}, 4.0 / 3), 1);
    CHECK_EQ(p.undoStack()->index(), before + 1);
    CHECK_EQ(ed.setPictureAspect({}, 4.0 / 3), 0); // already
    const QImage wide = render(p, 5);
    CHECK(near(blobAspect(wide, &box), 1.0, 0.03));
    CHECK(near(box.center().x(), 960, 6));
    CHECK(blackEdge(wide) >= 2); // pillarboxed: the 4:3 picture in the middle
    CHECK(near(MediaAspect::ofClip(p.timeline().video[0].clips[0]), 4.0 / 3, 1e-9));

    // In a 4:3 timeline it fills the frame, round
    Project q;
    Editor eq(&q, &sel);
    setup(q, stretched, 50, QSize(1440, 1080));
    eq.setPictureAspect({}, 4.0 / 3);
    const QImage four = render(q, 5);
    CHECK(near(blobAspect(four), 1.0, 0.03));
    CHECK_EQ(blackEdge(four), 0);

    // Crop keeps its share of the picture: 100 px of the 16:9-fitted 1920 are 75 px of the 1440 one
    p.undoStack()->undo();
    p.edit("crop", [](Timeline& tl) { tl.video[0].clips[0].transform.cropLeft = 100; });
    ed.setPictureAspect({}, 4.0 / 3);
    CHECK(near(p.timeline().video[0].clips[0].transform.cropLeft, 75, 1e-6));
    ed.setPictureAspect({}, 0); // the file's shape again
    CHECK(near(p.timeline().video[0].clips[0].transform.cropLeft, 100, 1e-6));
    CHECK_EQ(p.timeline().video[0].clips[0].pictureAspect, 0.0);

    // Project round trip
    ed.setPictureAspect({}, 4.0 / 3);
    const QString path = dir + "/stretched.schneidi";
    QString error;
    CHECK(ProjectFile::save(p.data(), path, &error));
    ProjectData d;
    CHECK(ProjectFile::load(path, &d, &error));
    Project back;
    back.load(d);
    CHECK(near(back.timeline().video[0].clips[0].pictureAspect, 4.0 / 3, 1e-9));
}

void testCli(const QString& dir, const QString& pillar, const QString& stretched)
{
    const QString path = dir + "/cli.schneidi";
    {
        Project p;
        setup(p, pillar, 75, QSize(1920, 1080));
        p.addMedia({stretched, "stretched.mp4", 50, true, false, false});
        p.edit("more", [&](Timeline& tl) {
            Clip v;
            v.id = p.newClipId();
            v.mediaPath = stretched;
            v.start = 75;
            v.out = 49;
            tl.video[0].clips << v;
        });
        QString error;
        CHECK(ProjectFile::save(p.data(), path, &error));
    }
    auto run = [&](const QString& name, const QJsonObject& args) -> const QJsonObject {
        const Cli::Command* cmd = Cli::find(name);
        Cli::Context ctx;
        return cmd->run(Cli::normalize(*cmd, args), ctx);
    };
    auto errorOf = [&](const QString& ops) {
        try {
            run("edit", {{"project", path}, {"ops", ops}});
        } catch (const Cli::Error& e) {
            return e.code;
        }
        return QString("none");
    };
    auto clip = [&](int i) {
        ProjectData d;
        QString error;
        ProjectFile::load(path, &d, &error);
        Project q;
        q.load(d);
        return q.timeline().video[0].clips.value(i);
    };
    int source = 0;
    {
        ProjectData d;
        QString error;
        ProjectFile::load(path, &d, &error);
        Project q;
        q.load(d);
        source = q.currentSequence();
    }
    const QJsonObject probe = run("probe", {{"file", pillar}, {"bars", true}});
    CHECK(probe.value("black_bars").toBool());
    CHECK(near(probe.value("content").toObject().value("x").toDouble(), 12.5, 0.6));
    // check: in a 9:16 copy (fitted, bars not cropped) the bars show; after crop_bars they do not
    const auto barsFound = [&](const QString& timeline) {
        const QJsonObject c = run("check", {{"source", path}, {"timeline", timeline}, {"sound", false}});
        int n = 0;
        for (const QJsonValue& i : c.value("issues").toArray()) n += i.toObject().value("check").toString() == "black_bars";
        return n;
    };
    run("edit", {{"project", path}, {"ops", "[{\"op\":\"new_timeline\",\"name\":\"Tall\",\"from\":"
                                                + QString::number(source) + ",\"aspect\":\"9:16\",\"switch\":false}]"}});
    CHECK_EQ(barsFound("Tall"), 1); // the pillarboxed clip, not the stretched one (no bars)
    CHECK_EQ(barsFound(QString::number(source)), 0); // a plain 16:9 edit keeps them as recorded
    run("edit", {{"project", path}, {"ops", "[{\"op\":\"timeline\",\"timeline\":\"Tall\"},{\"op\":\"crop_bars\"},"
                                            "{\"op\":\"timeline\",\"timeline\":" + QString::number(source) + "}]"}});
    CHECK_EQ(barsFound("Tall"), 0);
    const QJsonObject r = run("edit", {{"project", path}, {"ops", "[{\"op\":\"crop_bars\"}]"}});
    CHECK(r.value("notes").toArray().first().toString().contains("cropped the black bars of 1 of 2"));
    CHECK(clip(0).transform.cropLeft > 200);
    CHECK_EQ(clip(1).transform.cropLeft, 0.0); // no bars
    run("edit", {{"project", path}, {"ops", "[{\"op\":\"picture_aspect\",\"clips\":[" + QString::number(clip(1).id) + "],\"aspect\":\"4:3\"}]"}});
    CHECK(near(clip(1).pictureAspect, 4.0 / 3, 1e-9));
    run("edit", {{"project", path}, {"ops", "[{\"op\":\"picture_aspect\",\"aspect\":\"file\"}]"}});
    CHECK_EQ(clip(1).pictureAspect, 0.0);
    CHECK_EQ(errorOf("[{\"op\":\"picture_aspect\",\"aspect\":\"wide\"}]"), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf("[{\"op\":\"picture_aspect\",\"aspect\":50}]"), QString("BAD_ARGUMENT"));
    // reframe: bars cropped by default; bars:false leaves the crop
    run("edit", {{"project", path}, {"ops", "[{\"op\":\"transform\",\"clips\":[" + QString::number(clip(0).id)
                                                + "],\"crop_left\":0,\"crop_right\":0},{\"op\":\"new_timeline\","
                                                  "\"from\":" + QString::number(source) + ",\"aspect\":\"9:16\",\"reframe\":\"center\",\"bars\":false}]"}});
    CHECK_EQ(clip(0).transform.cropLeft, 0.0); // the copy is open now
    const QJsonObject s = run("edit", {{"project", path}, {"ops", "[{\"op\":\"reframe\",\"mode\":\"center\"}]"}});
    bool noted = false;
    for (const QJsonValue& n : s.value("notes").toArray()) noted = noted || n.toString().contains("black bars cropped");
    CHECK(noted);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("game43");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    const QStringList h264{"-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"};
    // Game picture 480x360 (4:3) in a 640x360 frame; a bright square runs along the left edge of the content
    const QStringList game{"-f", "lavfi", "-i", "testsrc2=s=480x360:r=25:d=3", "-f", "lavfi", "-i",
                           "color=c=white:s=40x40:r=25:d=3"};
    const QString pillar = Check::makeMedia(
        tmp.filePath("pillar.mp4"),
        game + QStringList{"-filter_complex", "[0][1]overlay=x=4:y='20+100*t',pad=640:360:80:0:black"} + h264);
    const QString letter = Check::makeMedia(
        tmp.filePath("letter.mp4"),
        QStringList{"-f", "lavfi", "-i", "testsrc2=s=640x270:r=25:d=3", "-vf", "pad=640:360:0:45:black"} + h264);
    // Dark scene: the picture at a fifth of its brightness, a bright moment only in the middle second
    const QString darkPillar = Check::makeMedia(
        tmp.filePath("dark.mp4"),
        QStringList{"-f", "lavfi", "-i", "testsrc2=s=480x360:r=25:d=3", "-vf",
                    "eq=brightness='if(between(t,1,2),0,-0.42)':eval=frame,pad=640:360:80:0:black"} + h264);
    const QString overlay = Check::makeMedia(
        tmp.filePath("overlay.mp4"),
        QStringList{"-f", "lavfi", "-i", "testsrc2=s=480x360:r=25:d=3", "-vf",
                    "pad=640:360:80:0:black,drawbox=x=10:y=10:w=10:h=10:color=white:t=fill"} + h264);
    const QString dark = Check::makeMedia(
        tmp.filePath("allDark.mp4"), QStringList{"-f", "lavfi", "-i", "color=c=0x0a0a0a:s=640x360:r=25:d=2"} + h264);
    // A circle of a 4:3 picture stretched to 16:9 (as a recorder set to stretch does)
    const QString stretched = Check::makeMedia(
        tmp.filePath("stretched.mp4"),
        QStringList{"-f", "lavfi", "-i", "color=c=white:s=480x360:r=25:d=2", "-vf",
                    "geq=lum='if(lt(hypot(X-240,Y-180),100),0,255)':cb=128:cr=128,scale=640:360,setsar=1"} + h264);
    if (!CHECK(!pillar.isEmpty() && !letter.isEmpty() && !darkPillar.isEmpty() && !overlay.isEmpty() && !dark.isEmpty()
               && !stretched.isEmpty()))
        return Check::result();
    Check::initEngine();
    std::setlocale(LC_NUMERIC, "C");
    testDetect(pillar, letter, darkPillar, overlay, dark);
    testReframe(pillar);
    testCropBars(pillar);
    testStretched(tmp.path(), stretched);
    testCli(tmp.path(), pillar, stretched);
    return Check::result();
}
