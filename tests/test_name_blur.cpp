// SPDX-License-Identifier: AGPL-3.0-only
// Test hiding player names (engine/TextRegions, core/NameBlur, Editor::blurNames, edit op blur_names): text found on
// synthetic game pictures (kill feed, a moving name tag, ammo, crosshair; recall/precision printed), templates,
// masks over time that follow the tag, the Editor op with undo, project round trip, the CLI op on a rendered video.
#include "check.h"

#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/EffectRegistry.h"
#include "core/Editor.h"
#include "core/GradeWindows.h"
#include "core/Keyframes.h"
#include "core/NameBlur.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/ColorGrade.h"
#include "engine/Snapshot.h"
#include "engine/TextRegions.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QJsonArray>
#include <QPainter>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QUndoStack>
#include <clocale>
#include <cmath>

namespace {

constexpr int kFrames = 40;

struct Truth {
    QVector<QRectF> names; // kill feed entries and the name tag, 0..1
    QRectF tag;
    QRectF ammo;
};

QRectF norm(const QRectF& r, int w, int h) { return QRectF(r.x() / w, r.y() / h, r.width() / w, r.height() / h); }

// A game-like picture: sand wall with low-contrast bricks, a dark window, a wire, the crosshair, ammo bottom right,
// a kill feed top right on dark plates and a name tag above a "head" that moves with t
QImage gamePicture(int W, int H, int t, Truth* truth)
{
    QImage img(W, H, QImage::Format_RGB32);
    QPainter p(&img);
    QLinearGradient sky(0, 0, 0, H);
    sky.setColorAt(0, QColor(150, 190, 230));
    sky.setColorAt(0.4, QColor(205, 185, 150));
    sky.setColorAt(1, QColor(170, 150, 120));
    p.fillRect(img.rect(), sky);
    QRandomGenerator rng(7);
    for (int i = 0; i < 260; ++i) { // bricks: edges of ~20 levels, not letters
        const int x = rng.bounded(W), y = H / 3 + rng.bounded(H * 2 / 3);
        const int d = rng.bounded(-12, 12);
        p.fillRect(QRect(x, y, W / 30, H / 50), QColor(190 + d, 168 + d, 132 + d));
    }
    p.fillRect(QRect(W * 0.1, H * 0.45, W * 0.05, H * 0.12), QColor(40, 35, 30)); // window
    p.setPen(QPen(QColor(30, 30, 30), 2));
    p.drawLine(QPointF(0, H * 0.3), QPointF(W, H * 0.22)); // wire
    p.setPen(QPen(QColor(0, 255, 0), 2)); // crosshair
    p.drawLine(QPointF(W / 2 - 12, H / 2), QPointF(W / 2 - 4, H / 2));
    p.drawLine(QPointF(W / 2 + 4, H / 2), QPointF(W / 2 + 12, H / 2));
    p.drawLine(QPointF(W / 2, H / 2 - 12), QPointF(W / 2, H / 2 - 4));
    p.drawLine(QPointF(W / 2, H / 2 + 4), QPointF(W / 2, H / 2 + 12));

    QFont f("DejaVu Sans");
    f.setBold(true);
    f.setPixelSize(std::max(9, int(H * 0.024)));
    p.setFont(f);
    const QFontMetrics fm(f);
    // Kill feed: two entries, right aligned
    const QStringList feed{"Kristijan  >  Ejderha07", "wunfel  >  No_Name"};
    for (int i = 0; i < feed.size(); ++i) {
        const QRect tb = fm.tightBoundingRect(feed[i]);
        const double x = W * 0.985 - tb.width(), base = H * (0.08 + 0.045 * i);
        p.fillRect(QRectF(x - 8, base + tb.top() - 5, tb.width() + 16, tb.height() + 10), QColor(20, 20, 20, 170));
        p.setPen(Qt::white);
        p.drawText(QPointF(x, base), feed[i]);
        truth->names << norm(QRectF(tb).translated(x, base), W, H);
    }
    // Ammo bottom right
    {
        const QString ammo = "30 / 90";
        const QRect tb = fm.tightBoundingRect(ammo);
        const QPointF at(W * 0.68, H * 0.96);
        p.setPen(Qt::white);
        p.drawText(at, ammo);
        truth->ammo = norm(QRectF(tb).translated(at), W, H);
    }
    // Name tag: smaller, light blue with a dark outline, gliding across the view
    {
        QFont g = f;
        g.setPixelSize(std::max(8, int(H * 0.018)));
        p.setFont(g);
        const QFontMetrics gm(g);
        const QString name = "Onkel Theo";
        const QRect tb = gm.tightBoundingRect(name);
        const QPointF at(W * (0.2 + 0.5 * t / kFrames), H * (0.42 + 0.05 * std::sin(t / 6.0)));
        p.setPen(QColor(10, 10, 20));
        for (const QPointF d : {QPointF(-1, 0), QPointF(1, 0), QPointF(0, -1), QPointF(0, 1)}) p.drawText(at + d, name);
        p.setPen(QColor(140, 200, 255));
        p.drawText(at, name);
        truth->tag = norm(QRectF(tb).translated(at), W, H);
        truth->names << truth->tag;
    }
    return img;
}

// Share of the box covered by the found ones (words of a line come as separate boxes; they do not overlap)
double coverage(const QRectF& truth, const QVector<QRectF>& found)
{
    double sum = 0;
    for (const QRectF& r : found) {
        const QRectF i = truth.intersected(r);
        sum += i.width() * i.height();
    }
    return std::min(1.0, sum / (truth.width() * truth.height()));
}

void testDetection()
{
    // Recall: names found (half of the box covered); precision: found boxes that lie on any drawn text
    int names = 0, hits = 0, boxes = 0, onText = 0, ammoFound = 0;
    QVector<NameBlur::Sample> samples;
    for (int t = 0; t < kFrames; ++t) {
        Truth truth;
        const QImage img = gamePicture(1280, 720, t, &truth);
        const QVector<QRectF> found = TextRegions::detect(img);
        samples << NameBlur::Sample{t, found};
        for (const QRectF& n : truth.names) {
            ++names;
            hits += coverage(n, found) >= 0.5;
        }
        ammoFound += coverage(truth.ammo, found) >= 0.5;
        QVector<QRectF> all = truth.names;
        all << truth.ammo;
        for (const QRectF& r : found) {
            ++boxes;
            onText += std::any_of(all.begin(), all.end(), [&](const QRectF& a) {
                const double m = a.height();
                return a.adjusted(-m, -m, m, m).contains(r.center());
            });
        }
    }
    const double recall = double(hits) / names, precision = boxes ? double(onText) / boxes : 0;
    std::printf("text detection on %d synthetic frames: recall %.3f (%d/%d names), precision %.3f (%d/%d boxes), "
                "ammo found %d/%d\n", kFrames, recall, hits, names, precision, onText, boxes, ammoFound, kFrames);
    CHECK(recall >= 0.9);
    CHECK(precision >= 0.9);
    CHECK(ammoFound >= kFrames * 0.8); // all text is found; the template leaves the ammo out

    // Template cs2: a kill feed mask and one moving mask on the tag; ammo and crosshair never covered
    int dropped = -1;
    const QVector<NameBlur::Mask> masks = NameBlur::plan(samples, NameBlur::Template::Cs2, kFrames, {}, &dropped);
    CHECK_EQ(dropped, 0);
    int feed = 0, moving = 0;
    for (const NameBlur::Mask& m : masks) {
        feed += m.zone == "killfeed";
        moving += m.moving;
    }
    CHECK_EQ(feed, 1);
    CHECK_EQ(moving, 1);
    int tagCovered = 0, ammoCovered = 0, crossCovered = 0, feedCovered = 0;
    for (int t = 0; t < kFrames; ++t) {
        Truth truth;
        gamePicture(1280, 720, t, &truth);
        QVector<QRectF> rects;
        for (const NameBlur::Mask& m : masks) rects << NameBlur::rectAt(m, t);
        const auto covered = [&](const QRectF& r) {
            return std::any_of(rects.begin(), rects.end(), [&](const QRectF& m) { return m.contains(r); });
        };
        tagCovered += covered(truth.tag);
        if (!covered(truth.tag)) std::printf("  tag not covered at %d\n", t);
        feedCovered += covered(truth.names[0]) && covered(truth.names[1]);
        ammoCovered += covered(truth.ammo);
        crossCovered += std::any_of(rects.begin(), rects.end(), [](const QRectF& m) { return m.contains(QPointF(0.5, 0.5)); });
    }
    std::printf("cs2 masks: %lld, tag covered %d/%d frames, kill feed %d/%d\n", qlonglong(masks.size()), tagCovered,
                kFrames, feedCovered, kFrames);
    CHECK(tagCovered >= kFrames - 2);
    CHECK(feedCovered >= kFrames - 2);
    CHECK_EQ(ammoCovered, 0);
    CHECK_EQ(crossCovered, 0);
    // Template all: the ammo is text too
    const QVector<NameBlur::Mask> all = NameBlur::plan(samples, NameBlur::Template::All, kFrames);
    int ammoAll = 0;
    for (int t = 0; t < kFrames; ++t) {
        Truth truth;
        gamePicture(1280, 720, t, &truth);
        ammoAll += std::any_of(all.begin(), all.end(), [&](const NameBlur::Mask& m) {
            return NameBlur::rectAt(m, t).intersects(truth.ammo);
        });
    }
    CHECK(ammoAll >= kFrames - 2);
}

void testPlan()
{
    // Static kill feed text from frame 10 to 30, samples every 2 frames: the mask is off before, on a sample early,
    // off after; between two samples it never slides
    QVector<NameBlur::Sample> s;
    for (int f = 0; f < 50; f += 2) {
        NameBlur::Sample x{f, {}};
        if (f >= 10 && f <= 30) x.boxes << QRectF(0.8, 0.1, 0.15, 0.02);
        s << x;
    }
    NameBlur::Options every2;
    every2.samplesPerSecond = 15;
    QVector<NameBlur::Mask> m = NameBlur::plan(s, NameBlur::Template::Valorant, 50, every2);
    if (!CHECK_EQ(m.size(), 1)) return;
    CHECK(!m[0].moving);
    CHECK(NameBlur::rectAt(m[0], 0).isEmpty());
    CHECK(NameBlur::rectAt(m[0], 6).isEmpty());
    CHECK(NameBlur::rectAt(m[0], 8).contains(QRectF(0.8, 0.1, 0.15, 0.02)));
    CHECK(NameBlur::rectAt(m[0], 20).contains(QRectF(0.8, 0.1, 0.15, 0.02)));
    CHECK(NameBlur::rectAt(m[0], 32).contains(QRectF(0.8, 0.1, 0.15, 0.02)));
    CHECK(NameBlur::rectAt(m[0], 34).isEmpty());
    CHECK(m[0].keys.size() <= 6); // standing still: no key per sample
    // A single flicker in the kill feed zone is not text
    QVector<NameBlur::Sample> one{{0, {}}, {2, {QRectF(0.8, 0.2, 0.1, 0.02)}}, {4, {}}, {6, {}}};
    CHECK(NameBlur::plan(one, NameBlur::Template::Valorant, 8, every2).isEmpty());
    // CS2: the player name bottom left of the avatar is hidden over the whole clip, found or not
    const QVector<NameBlur::Mask> cs = NameBlur::plan(one, NameBlur::Template::Cs2, 8, every2);
    CHECK(cs.size() == 1 && cs[0].zone == "player" && NameBlur::rectAt(cs[0], 7).contains(QPointF(0.42, 0.96)));
    // Two tags one after the other share a window; three at once with one window: the longest stays
    QVector<NameBlur::Sample> tags;
    for (int f = 0; f < 60; ++f) {
        NameBlur::Sample x{f, {}};
        if (f < 20) x.boxes << QRectF(0.2 + f * 0.005, 0.4, 0.06, 0.015);
        if (f >= 30) x.boxes << QRectF(0.6, 0.6, 0.06, 0.015);
        tags << x;
    }
    m = NameBlur::plan(tags, NameBlur::Template::Valorant, 60);
    CHECK_EQ(m.size(), 1);
    if (!m.isEmpty()) {
        CHECK(NameBlur::rectAt(m[0], 10).contains(QRectF(0.25, 0.4, 0.06, 0.015)));
        CHECK(NameBlur::rectAt(m[0], 45).contains(QRectF(0.6, 0.6, 0.06, 0.015)));
        CHECK(NameBlur::rectAt(m[0], 25).isEmpty());
    }
    for (int f = 0; f < 60; ++f) tags[f].boxes << QRectF(0.4, 0.3, 0.05, 0.015) << QRectF(0.5, 0.7, 0.05, 0.015);
    NameBlur::Options o;
    o.maxMasks = 1;
    int dropped = 0;
    m = NameBlur::plan(tags, NameBlur::Template::Valorant, 60, o, &dropped);
    CHECK_EQ(m.size(), 1);
    CHECK_EQ(dropped, 3);
    NameBlur::Template t;
    CHECK(NameBlur::templateFromId("valorant", &t) && t == NameBlur::Template::Valorant);
    CHECK(!NameBlur::templateFromId("fortnite", &t));
}

// CS2 weapon list on the right: icons and short weapon names never count (not as HUD text, not as name tags in the
// view either), a long line there does ("AK-47 | <skin> von <name>")
void testWeaponList()
{
    const QRectF rifle(0.925, 0.715, 0.055, 0.022), pistol(0.948, 0.785, 0.03, 0.022), label(0.955, 0.758, 0.022, 0.011);
    // Measured on a CS2 recording (1080p): "AK-47 | Smaragdgrüner Nadelstreifen" 243 px, with " von Onkel Theo" 339 px
    const QRectF skin(0.863, 0.762, 0.127, 0.010);
    const QRectF dropped(0.812, 0.762, 0.177, 0.010);
    QVector<NameBlur::Sample> s;
    for (int f = 0; f < 90; ++f) {
        NameBlur::Sample x{f, {rifle, pistol, label}};
        if (f < 25) x.boxes << skin;
        if (f >= 30 && f < 70) x.boxes << dropped;
        s << x;
    }
    QVector<NameBlur::Mask> m = NameBlur::plan(s, NameBlur::Template::Cs2, 90);
    QStringList zones;
    for (const NameBlur::Mask& x : m) zones << x.zone;
    CHECK_EQ(zones, (QStringList{"player", "items"}));
    for (const NameBlur::Mask& x : m) {
        if (x.zone != "items") continue;
        CHECK(NameBlur::rectAt(x, 10).isEmpty());
        CHECK(NameBlur::rectAt(x, 50).contains(dropped));
        CHECK(!NameBlur::rectAt(x, 50).intersects(rifle));
    }
}

// The templates on other screens: a zone keeps its size and distance to its edge in heights (4:3 stretched to the
// clip's 16:9 picture), and lies inside the game picture between black bars
void testScreens()
{
    const auto zone = [](const NameBlur::Layout& l, const char* name) {
        for (const NameBlur::Zone& z : l.include)
            if (QLatin1String(z.name) == QLatin1String(name)) return z.area;
        return QRectF();
    };
    const auto near = [](double a, double b) { return std::abs(a - b) < 1e-9; };
    const NameBlur::Layout wide = NameBlur::layout(NameBlur::Template::Cs2);
    CHECK(near(zone(wide, "killfeed").left(), 0.6) && near(zone(wide, "killfeed").right(), 1));
    NameBlur::Screen four;
    four.aspect = 4.0 / 3.0;
    const NameBlur::Layout stretched = NameBlur::layout(NameBlur::Template::Cs2, four);
    CHECK(near(zone(stretched, "killfeed").right(), 1));
    CHECK(near(zone(stretched, "killfeed").width(), 0.4 * 4 / 3)); // 0.4 of 16:9 = 0.71 heights = 0.53 of 4:3
    CHECK(near(zone(stretched, "chat").left(), 0) && near(zone(stretched, "chat").width(), 0.42 * 4 / 3));
    CHECK(near(zone(stretched, "team").center().x(), 0.5) && near(zone(stretched, "team").width(), 0.36 * 4 / 3));
    CHECK(near(zone(stretched, "view").width(), 1));
    NameBlur::Screen bars = four;
    bars.content = QRectF(0.125, 0, 0.75, 1); // 4:3 between black bars in a 16:9 file
    const NameBlur::Layout boxed = NameBlur::layout(NameBlur::Template::Cs2, bars);
    CHECK(near(zone(boxed, "killfeed").right(), 0.875) && near(zone(boxed, "killfeed").width(), 0.4));
    CHECK(near(zone(boxed, "chat").left(), 0.125));
    CHECK(near(zone(boxed, "view").left(), 0.125) && near(zone(boxed, "view").right(), 0.875));
    // the middle stays the middle: same place in the file as on a 16:9 screen
    CHECK(near(zone(boxed, "player").center().x(), zone(wide, "player").center().x()));
    // A kill feed entry of the 4:3 picture between bars counts; the same place in a full 16:9 picture is the view
    QVector<NameBlur::Sample> s;
    for (int f = 0; f < 30; ++f) s << NameBlur::Sample{f, {QRectF(0.7, 0.1, 0.15, 0.02)}};
    NameBlur::Options o;
    o.screen = bars;
    bool feed = false;
    for (const NameBlur::Mask& m : NameBlur::plan(s, NameBlur::Template::Cs2, 30, o)) feed |= m.zone == "killfeed";
    CHECK(feed);
}

QVector<NameBlur::Mask> twoMasks()
{
    NameBlur::Mask feed;
    feed.zone = "killfeed";
    feed.keys = {{0, true, QRectF(0.8, 0.05, 0.18, 0.04)}, {39, true, QRectF(0.8, 0.05, 0.18, 0.08)}};
    NameBlur::Mask tag;
    tag.zone = "view";
    tag.moving = true;
    tag.keys = {{9, false, QRectF(0.25, 0.41, 0, 0)}, {10, true, QRectF(0.2, 0.4, 0.1, 0.02)},
                {30, true, QRectF(0.6, 0.5, 0.1, 0.02)}, {31, false, QRectF(0.65, 0.51, 0, 0)}};
    return {feed, tag};
}

void testEditor(const QString& dir)
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({"/x/a.mp4", "a.mp4", 100, true, false, false});
    p.edit("setup", [&](Timeline& tl) {
        Clip v;
        v.id = p.newClipId();
        v.mediaPath = "/x/a.mp4";
        v.in = 10;
        v.out = 49;
        v.start = 100;
        tl.video[0].clips << v;
    });
    const int id = p.timeline().video[0].clips[0].id;
    // One window already there (a face): the masks take the next numbers
    ed.addGradeWindow({id}, 1, "ellipse");
    const int before = p.undoStack()->index();
    QHash<int, QVector<NameBlur::Mask>> masks;
    masks[id] = twoMasks();
    const QHash<int, QVector<int>> added = ed.blurNames(masks, "pixelate", 80);
    CHECK_EQ(p.undoStack()->index(), before + 1);
    CHECK_EQ(added.value(id), (QVector<int>{2, 3}));
    const Clip* c = TimelineOps::findClip(p.timeline(), id);
    CHECK_EQ(GradeWindows::list(*c), (QVector<int>{1, 2, 3}));
    CHECK(GradeWindows::fillAt(*c, 3, 20).fill == GradeWindows::Fill::Pixelate);
    CHECK_EQ(GradeWindows::fillAt(*c, 3, 20).amount, 80.0);
    CHECK_EQ(GradeWindows::fillAt(*c, 3, 5).amount, 0.0); // off before the tag
    CHECK_EQ(GradeWindows::geometryAt(*c, 3, 5).width, 0.0);
    const GradeWindows::Geometry g = GradeWindows::geometryAt(*c, 3, 20);
    CHECK(GradeWindows::geometryAt(*c, 3, 20).shape == GradeWindows::Shape::Rectangle);
    CHECK(std::abs(g.x - 45) < 1e-6); // half way from 25 to 65 (centres)
    CHECK(std::abs(g.width - 10) < 1e-6);
    // A full clip: masks beyond the free windows are left out
    for (int n = 4; n <= GradeWindows::kMax; ++n) ed.addGradeWindow({id}, n, "ellipse");
    CHECK(ed.blurNames(masks, "blur", 50).value(id).isEmpty());

    // Project round trip
    const QString path = dir + "/names.schneidi";
    QString error;
    CHECK(ProjectFile::save(p.data(), path, &error));
    ProjectData d;
    CHECK(ProjectFile::load(path, &d, &error));
    Project q;
    q.load(d);
    const Clip* l = TimelineOps::findClip(q.timeline(), id);
    if (CHECK(l)) {
        CHECK_EQ(GradeWindows::list(*l).size(), GradeWindows::kMax);
        for (int t : {0, 9, 10, 20, 30, 31, 39}) {
            const GradeWindows::Geometry a = GradeWindows::geometryAt(*c, 3, t), b = GradeWindows::geometryAt(*l, 3, t);
            CHECK(std::abs(a.x - b.x) < 1e-6 && std::abs(a.width - b.width) < 1e-6 && std::abs(a.y - b.y) < 1e-6);
            CHECK_EQ(GradeWindows::fillAt(*l, 3, t).amount, GradeWindows::fillAt(*c, 3, t).amount);
        }
    }
    // Undo: the masks go, the face window stays
    while (p.undoStack()->index() > before) p.undoStack()->undo();
    CHECK_EQ(GradeWindows::list(*TimelineOps::findClip(p.timeline(), id)), (QVector<int>{1}));
    // The clip's grade switched off: hiding names switches it on, else the windows would not render
    p.edit("grade off", [&](Timeline& tl) { EffectRegistry::instance(*TimelineOps::findClip(tl, id), "grade")->enabled = false; });
    ed.blurNames(masks, "blur", 50);
    const Clip* off = TimelineOps::findClip(p.timeline(), id);
    CHECK(EffectRegistry::instance(*off, "grade")->enabled);
    CHECK(ColorGrade::active(*off));
}

void testCli(const QString& dir)
{
    // A rendered game video: frames drawn like testDetection, 25 fps
    QDir().mkpath(dir + "/frames");
    for (int t = 0; t < kFrames; ++t) {
        Truth truth;
        gamePicture(960, 540, t, &truth).save(QString("%1/frames/f%2.png").arg(dir).arg(t, 3, 10, QChar('0')));
    }
    const QString video = Check::makeMedia(dir + "/game.mp4", {"-framerate", "25", "-i", dir + "/frames/f%03d.png", "-c:v",
                                                               "libx264", "-preset", "ultrafast", "-crf", "12", "-pix_fmt",
                                                               "yuv420p"});
    if (!CHECK(!video.isEmpty())) return;
    const QString path = dir + "/cli.schneidi";
    {
        Project p;
        p.setFormat(ProjectFormat{960, 540, FrameRate{25, 1}});
        p.addMedia({video, "game.mp4", kFrames, true, false, false});
        p.edit("setup", [&](Timeline& tl) {
            Clip v;
            v.id = p.newClipId();
            v.mediaPath = video;
            v.out = kFrames - 1;
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
    CHECK_EQ(errorOf(R"([{"op":"blur_names","template":"fortnite"}])"), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf(R"([{"op":"blur_names","style":"smear"}])"), QString("BAD_ARGUMENT"));
    const QJsonObject done =
        run("edit", {{"project", path}, {"ops", R"([{"op":"blur_names","template":"cs2","style":"blur","strength":70}])"}});
    QString notes;
    for (const QJsonValue& n : done["notes"].toArray()) notes += n.toString() + "\n";
    std::printf("cli notes: %s", qPrintable(notes));
    CHECK(notes.contains("killfeed"));
    CHECK(notes.contains("view"));
    ProjectData d;
    QString error;
    CHECK(ProjectFile::load(path, &d, &error));
    Project q;
    q.load(d);
    const Clip& c = q.timeline().video[0].clips[0];
    CHECK(GradeWindows::list(c).size() >= 2);
    // The tag is blurred in the render: its area changed, a far corner of the wall did not
    const ProjectFormat f = q.format();
    Timeline plain = q.timeline();
    plain.video[0].clips[0].effects.clear();
    plain.video[0].clips[0].keys.clear();
    for (int t : {5, 20, 35}) {
        Truth truth;
        gamePicture(960, 540, t, &truth);
        const QImage a = Snapshot::timeline(f, plain, t), b = Snapshot::timeline(f, q.timeline(), t);
        if (!CHECK(!a.isNull() && a.size() == b.size())) return;
        const auto diff = [&](const QRectF& r) {
            const QRect px(int(r.x() * a.width()), int(r.y() * a.height()), std::max(1, int(r.width() * a.width())),
                           std::max(1, int(r.height() * a.height())));
            double sum = 0;
            for (int y = px.top(); y <= px.bottom(); ++y)
                for (int x = px.left(); x <= px.right(); ++x)
                    sum += std::abs(qGray(a.pixel(x, y)) - qGray(b.pixel(x, y)));
            return sum / (px.width() * px.height());
        };
        CHECK(diff(truth.tag) > 8);
        CHECK(diff(truth.names[0]) > 8);
        CHECK(diff(QRectF(0.02, 0.6, 0.05, 0.05)) < 1);
        CHECK(diff(truth.ammo) < 1);
    }
    bool helped = false;
    for (const QJsonValue& v : Cli::detail::editOpsHelp()) helped |= v["op"].toString() == "blur_names";
    CHECK(helped);
}

// The game video in a 9:16 timeline (a short): text is found on the clip's own picture and given in the frame,
// where the picture lies in a band in the middle; the templates follow it there
void testOtherFrame(const QString& dir)
{
    const QString video = dir + "/game.mp4";
    if (!QFileInfo::exists(video)) return; // testCli failed already
    const ProjectFormat tall{540, 960, FrameRate{25, 1}};
    Clip c;
    c.id = 1;
    c.mediaPath = video;
    c.out = kFrames - 1;
    const TextRegions::Result r = TextRegions::analyze(tall, c);
    if (!CHECK(r.error.isEmpty() && r.samples.size() == kFrames)) return;
    const double band = (960 - 540 * 9.0 / 16) / 2 / 960; // picture from here to 1 - band
    Truth truth;
    gamePicture(960, 540, 10, &truth);
    const QRectF feed = truth.names[0];
    const QRectF inFrame(feed.x(), band + feed.y() * (1 - 2 * band), feed.width(), feed.height() * (1 - 2 * band));
    CHECK(coverage(inFrame, r.samples[10].boxes) > 0.6);
    for (const QRectF& b : r.samples[10].boxes) CHECK(b.top() >= band - 1e-6 && b.bottom() <= 1 - band + 1e-6);
    const NameBlur::Screen screen = TextRegions::screenOf(tall, c);
    CHECK(std::abs(screen.aspect - 16.0 / 9) < 0.01);
    CHECK(std::abs(screen.content.top() - band) < 1e-3 && std::abs(screen.content.width() - 1) < 1e-6);
    NameBlur::Options o;
    o.screen = screen;
    o.samplesPerSecond = 25;
    bool killfeed = false;
    for (const NameBlur::Mask& m : NameBlur::plan(r.samples, NameBlur::Template::Cs2, kFrames, o))
        if (m.zone == "killfeed") killfeed = NameBlur::rectAt(m, 20).intersects(inFrame);
    CHECK(killfeed);
    // The same clip (in/out in project frames) at 30 fps covers other moments of the file: not the 25 fps result
    // from the cache
    const auto cached = [] { return QDir(TextRegions::cacheDir()).entryList(QDir::Files).size(); };
    const qsizetype before = cached();
    const ProjectFormat tall30{540, 960, FrameRate{30, 1}};
    const TextRegions::Result r30 = TextRegions::analyze(tall30, c);
    CHECK(r30.error.isEmpty());
    CHECK_EQ(cached(), before + 1);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("name_blur");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    testPlan();
    testWeaponList();
    testScreens();
    testDetection();
    testEditor(tmp.path());
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    Check::initEngine();
    std::setlocale(LC_NUMERIC, "C");
    testCli(tmp.path());
    testOtherFrame(tmp.path());
    return Check::result();
}
