// SPDX-License-Identifier: AGPL-3.0-only
// Brand kit (core/BrandKit.h, 0.18): the kit as JSON (paths absolute + relative to a project), the global kit in the
// app data folder, the project's own kit and title references in the project file, Editor::applyBrand (references
// resolved on all timelines and subtitle tracks, fixed values untouched, a missing brand colour keeps the value, one
// undo step, nothing to do = no step), the project's own kit as one step, logo and intro/outro placed, and the CLI
// `brand` (show, set, apply, unset, logo) plus brand references in the `edit` title op; an AI chat's combined step keeps
// the project kit; a brand text colour ends a colour animation.

#include "check.h"

#include "app/BrandKitDialog.h"
#include "app/Theme.h"
#include "cli/Commands.h"
#include "core/BrandKit.h"
#include "core/Editor.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "ui/Inspector.h"

#include <QApplication>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QMenu>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QToolButton>

#include <chrono>
#include <thread>

namespace {

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        return Cli::execute(*cmd, Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        qWarning().noquote() << "unexpected error" << e.code << e.message;
        return {};
    }
}

QString errorOf(const QString& name, const QJsonObject& raw)
{
    try {
        const Cli::Command* cmd = Cli::find(name);
        Cli::Context ctx;
        Cli::execute(*cmd, Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        return e.code;
    }
    return "none";
}

BrandKit fullKit(const QString& dir)
{
    BrandKit k;
    k.logo = dir + "/logo.png";
    k.logoCorner = 4;
    k.logoSize = 0.2;
    k.colors = {QColor("#ff0000"), QColor("#00ff00"), QColor(0, 0, 255, 128)};
    k.fonts = {"Serif", "Monospace"};
    k.intro = dir + "/intro.mp4";
    k.outro = dir + "/outro.mp4";
    return k;
}

Clip titleClip(int id, int start, int len)
{
    Clip c;
    c.id = id;
    c.kind = ClipKind::Title;
    c.start = start;
    c.in = 0;
    c.out = len - 1;
    return c;
}

void testKitJson(const QString& dir)
{
    const BrandKit k = fullKit(dir);
    CHECK(Brand::fromJson(Brand::toJson(k)) == k);
    CHECK(Brand::toJson(BrandKit{}).isEmpty()); // only what is set
    // Relative to a base: a folder moved together (absolute path missing, relative one there)
    QDir().mkpath(dir + "/moved");
    QFile(dir + "/moved/logo.png").open(QIODevice::WriteOnly);
    const QDir oldBase(dir + "/old");
    BrandKit atOld;
    atOld.logo = dir + "/old/logo.png";
    QJsonObject o = Brand::toJson(atOld, &oldBase);
    CHECK_EQ(o.value("logoRel").toString(), QString("logo.png"));
    const QDir newBase(dir + "/moved");
    CHECK_EQ(Brand::fromJson(o, &newBase).logo, dir + "/moved/logo.png");
    // Windows paths stay as written (forward slashes)
    BrandKit win;
    win.logo = "C:\\Brand\\logo.png";
    CHECK_EQ(Brand::fromJson(Brand::toJson(win)).logo, QString("C:/Brand/logo.png"));
    // Limits: at most 4 colours, 2 fonts, corner 0..4, size 0.01..1
    BrandKit many;
    many.colors = {Qt::red, Qt::green, Qt::blue, Qt::white, Qt::black};
    many.fonts = {"A", "", "B", "C"};
    many.logoCorner = 9;
    many.logoSize = 5;
    const BrandKit c = Brand::fromJson(Brand::toJson(many));
    CHECK_EQ(c.colors.size(), 4);
    CHECK_EQ(c.fonts, QStringList({"A", "B"}));
    CHECK_EQ(c.logoCorner, 4);
    CHECK_EQ(c.logoSize, 1.0);
    CHECK(!c.color(5));
    CHECK(c.color(1) == QColor(Qt::red));
    CHECK(!BrandKit{}.font(1));
}

void testGlobal(const QString& dir)
{
    QFile::remove(Brand::globalPath());
    CHECK(Brand::global().isEmpty()); // neutral default: an empty kit
    CHECK(Brand::globalPath().contains("schneidi-test")); // the test's own app data, never the user's
    const BrandKit k = fullKit(dir);
    QString err;
    CHECK(Brand::setGlobal(k, &err));
    CHECK(QFileInfo::exists(Brand::globalPath()));
    CHECK(Brand::global() == k);
    ProjectData d;
    CHECK(Brand::effective(d) == k);
    d.brand = BrandKit{};
    CHECK(Brand::effective(d) == BrandKit{}); // the project's own wins, even an empty one
    CHECK(Brand::setGlobal({}, &err));
    CHECK(!QFileInfo::exists(Brand::globalPath()));
}

void testProjectFile(const QString& dir)
{
    ProjectData d;
    d.timeline = emptyTimeline();
    Clip t = titleClip(1, 0, 50);
    t.title.colorRef = 2;
    t.title.outlineRef = 1;
    t.title.boxRef = 4;
    t.title.fontRef = 2;
    d.timeline.video[0].clips << t;
    Clip plain = titleClip(2, 50, 50);
    d.timeline.video[0].clips << plain;
    SubtitleTrack st;
    st.style.colorRef = 3;
    d.timeline.subtitles << st;
    d.lastClipId = 2;
    const QString path = dir + "/p.schneidi";
    const QByteArray none = ProjectFile::toJson(d, path);
    CHECK(!none.contains("\"brand\""));
    d.brand = fullKit(dir);
    const QByteArray json = ProjectFile::toJson(d, path);
    CHECK(json.contains("\"logoRel\": \"logo.png\""));
    ProjectData back;
    QString err;
    CHECK(ProjectFile::fromJson(json, path, &back, &err));
    CHECK(back.brand && *back.brand == *d.brand);
    const TitleStyle& r = back.timeline.video[0].clips[0].title;
    CHECK_EQ(r.colorRef, 2);
    CHECK_EQ(r.outlineRef, 1);
    CHECK_EQ(r.boxRef, 4);
    CHECK_EQ(r.fontRef, 2);
    CHECK(!back.timeline.video[0].clips[1].title.hasBrandRef());
    CHECK_EQ(back.timeline.subtitles[0].style.colorRef, 3);
    // Refs are written only when set; a plain title keeps its old shape
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    const QJsonObject plainJson = root["timeline"].toObject()["video"].toArray()[0].toObject()["clips"].toArray()[1]
                                      .toObject()["title"].toObject();
    CHECK(!plainJson.contains("colorRef"));
    // The references alone need no newer format version (the values stay stored next to them)
    CHECK_EQ(root["version"].toInt(), 4);
}

void testApplyBrand()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({"/x/a.mp4", "a.mp4", 1000, true, true, false});
    // Open timeline: a referencing title, a fixed one, one referencing a colour the kit does not have
    Timeline tl = p.timeline();
    Clip ref = titleClip(p.newClipId(), 0, 50);
    ref.title.colorRef = 1;
    ref.title.boxRef = 2;
    ref.title.fontRef = 1;
    Clip fixed = titleClip(p.newClipId(), 50, 50);
    fixed.title.color = QColor("#123456");
    Clip missing = titleClip(p.newClipId(), 100, 50);
    missing.title.color = QColor("#abcdef");
    missing.title.colorRef = 3;
    tl.video[0].clips << ref << fixed << missing;
    SubtitleTrack st;
    st.style.outlineRef = 2;
    tl.subtitles << st;
    p.edit("setup", [&](Timeline& t) { t = tl; });
    // A second timeline with a referencing title
    const int first = p.currentSequence();
    const int second = p.addTimeline("Zwei");
    Clip other = titleClip(p.newClipId(), 0, 25);
    other.title.outlineRef = 1;
    p.edit("setup 2", [&](Timeline& t) { t.video[0].clips << other; });
    p.setCurrentSequence(first);

    // Nothing references an empty kit: no step
    QFile::remove(Brand::globalPath());
    const int steps0 = p.undoStack()->count();
    CHECK(!ed.applyBrand());
    CHECK_EQ(p.undoStack()->count(), steps0);

    BrandKit kit;
    kit.colors = {QColor("#ff0000"), QColor("#00ff00")};
    kit.fonts = {"Serif"};
    CHECK(Brand::setGlobal(kit));
    CHECK(ed.applyBrand());
    CHECK_EQ(p.undoStack()->count(), steps0 + 1); // one step for all timelines
    auto title = [&](int seq, int id) { return TimelineOps::findClip(p.sequence(seq)->timeline, id)->title; };
    CHECK(title(first, ref.id).color == QColor("#ff0000"));
    CHECK_EQ(title(first, ref.id).font, QString("Serif"));
    // The box keeps its transparency
    CHECK(title(first, ref.id).boxColor == QColor(0, 255, 0, TitleStyle().boxColor.alpha()));
    CHECK(title(first, fixed.id).color == QColor("#123456"));   // fixed: untouched
    CHECK(title(first, missing.id).color == QColor("#abcdef")); // brand colour 3 missing: keeps its value
    CHECK(title(second, other.id).outlineColor == QColor("#ff0000"));
    CHECK(p.sequence(first)->timeline.subtitles[0].style.outlineColor == QColor("#00ff00"));
    // Again: nothing to do, no step
    CHECK(!ed.applyBrand());
    CHECK_EQ(p.undoStack()->count(), steps0 + 1);
    // Undo: everything as before in one step
    p.undoStack()->undo();
    CHECK(title(first, ref.id).color == QColor(Qt::white));
    CHECK(title(second, other.id).outlineColor == QColor(Qt::black));
    p.undoStack()->redo();

    // The project's own kit: set + applied in one step; undo brings both back
    BrandKit own;
    own.colors = {QColor("#0000ff")};
    const int steps1 = p.undoStack()->index();
    ed.setProjectBrand(own);
    CHECK_EQ(p.undoStack()->index(), steps1 + 1);
    CHECK(p.brand() && *p.brand() == own);
    CHECK(title(first, ref.id).color == QColor("#0000ff"));
    CHECK_EQ(title(first, ref.id).font, QString("Serif")); // the own kit has no font: the value stays
    ed.setProjectBrand(own); // unchanged: no step
    CHECK_EQ(p.undoStack()->index(), steps1 + 1);
    p.undoStack()->undo();
    CHECK(!p.brand());
    CHECK(title(first, ref.id).color == QColor("#ff0000"));
    // Saved and loaded
    p.undoStack()->redo();
    Project q;
    q.load(p.data());
    CHECK(q.brand() && *q.brand() == own);
    CHECK(Brand::setGlobal({}));
}

// An AI chat turn combined into one step (Project::combineSteps, MainWindowChat) keeps a change of the project's own
// kit with the titles it coloured; undo/redo of the combined step take both along
void testCombinedBrand()
{
    std::printf("combined steps keep the project kit\n");
    QFile::remove(Brand::globalPath());
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    Clip t = titleClip(p.newClipId(), 0, 50);
    t.title.colorRef = 1;
    p.edit("setup", [&](Timeline& tl) { tl.video[0].clips << t; });
    const int from = p.undoStack()->index();
    BrandKit own;
    own.colors = {QColor("#0000ff")};
    ed.setProjectBrand(own);
    ed.addTitle(60, -1, 25);
    p.combineSteps(from, "KI-Chat: Markenfarben");
    CHECK_EQ(p.undoStack()->index(), from + 1);
    CHECK(p.brand() && *p.brand() == own);
    CHECK(TimelineOps::findClip(p.timeline(), t.id)->title.color == QColor("#0000ff"));
    p.undoStack()->undo();
    CHECK(!p.brand());
    CHECK(TimelineOps::findClip(p.timeline(), t.id)->title.color == QColor(Qt::white));
    p.undoStack()->redo();
    CHECK(p.brand() && *p.brand() == own);
}

// A brand text colour on a title whose colour is animated: the animation would draw over it (the renderer takes the
// keyframes), so resolving the reference ends it; animating the colour again makes it a fixed one
void testAnimatedColor()
{
    std::printf("brand colour over an animated text colour\n");
    QFile::remove(Brand::globalPath());
    Project p;
    ProjectData d;
    d.timeline = emptyTimeline();
    BrandKit kit;
    kit.colors = {QColor("#00ff00")};
    d.brand = kit;
    Clip t = titleClip(1, 0, 50);
    Keys::setKey(t, AnimParam::TitleColor, 0, Keys::fromColor(QColor("#ff0000")));
    Keys::setKey(t, AnimParam::TitleColor, 40, Keys::fromColor(QColor("#0000ff")));
    t.title.colorRef = 1;
    d.timeline.video[0].clips << t;
    d.lastClipId = 1;
    p.load(d);
    Selection sel;
    Editor ed(&p, &sel);
    CHECK(ed.applyBrand());
    const Clip* c = TimelineOps::findClip(p.timeline(), 1);
    CHECK(!Keys::animated(*c, AnimParam::TitleColor));
    CHECK(Keys::toColor(Keys::valueAt(*c, AnimParam::TitleColor, 20)) == QColor("#00ff00"));
    // A keyframe set by hand: the colour is fixed again, the next applyBrand keeps the animation
    ed.setKeyframes({1}, {AnimParam::TitleColor}, 10, true);
    c = TimelineOps::findClip(p.timeline(), 1);
    CHECK_EQ(c->title.colorRef, 0);
    CHECK(Keys::animated(*c, AnimParam::TitleColor));
    CHECK(!ed.applyBrand());
    CHECK(Keys::animated(*TimelineOps::findClip(p.timeline(), 1), AnimParam::TitleColor));
}

void testLogoAndIntro(const QString& dir)
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    // Logo 200x100 (aspect 2) in a 1920x1080 timeline
    const QString logo = dir + "/logo2.png";
    QImage img(200, 100, QImage::Format_ARGB32);
    img.fill(Qt::red);
    CHECK(img.save(logo));
    p.addMedia({logo, "logo2.png", 125, true, false, true});
    p.addMedia({"/x/intro.mp4", "intro.mp4", 50, true, true, false});
    p.addMedia({"/x/outro.mp4", "outro.mp4", 30, true, true, false});
    p.addMedia({"/x/a.mp4", "a.mp4", 1000, true, true, false});
    ed.addMediaAt({"/x/a.mp4"}, 0, 0);
    p.edit("trim", [](Timeline& t) { t.video[0].clips[0].out = 99, t.audio[0].clips[0].out = 99; });

    BrandKit kit;
    CHECK(Brand::setGlobal(kit));
    CHECK_EQ(ed.addBrandLogo(), 0); // no logo: nothing
    CHECK(!ed.addIntroOutro());
    kit.logo = logo;
    kit.intro = "/x/intro.mp4";
    kit.outro = "/x/outro.mp4";
    CHECK(Brand::setGlobal(kit));

    const int tracks = p.timeline().video.size();
    const int steps = p.undoStack()->count();
    const int id = ed.addBrandLogo();
    CHECK(id > 0);
    CHECK_EQ(p.undoStack()->count(), steps + 1);
    CHECK_EQ(p.timeline().video.size(), tracks + 1);
    TrackRef r;
    const Clip* c = TimelineOps::findClip(p.timeline(), id, &r);
    CHECK(c && r.index == tracks);
    if (c) {
        CHECK_EQ(c->start, 0);
        CHECK_EQ(c->end(), 100); // over the whole timeline
        CHECK(std::abs(c->transform.zoomX - 0.12) < 1e-9);
        // 1920x960 fitted, at 0.12: 230.4 x 115.2; margin 43.2 from the top-right corner
        CHECK(std::abs(c->transform.posX - 801.6) < 0.06);
        CHECK(std::abs(c->transform.posY - 439.2) < 0.06);
    }
    p.undoStack()->undo();
    CHECK_EQ(p.timeline().video.size(), tracks);

    CHECK(ed.addIntroOutro());
    CHECK_EQ(p.undoStack()->count(), steps + 1);
    const Track& v1 = p.timeline().video[0];
    CHECK_EQ(v1.clips.size(), 3);
    if (v1.clips.size() == 3) {
        CHECK_EQ(v1.clips[0].mediaPath, QString("/x/intro.mp4"));
        CHECK_EQ(v1.clips[0].start, 0);
        CHECK_EQ(v1.clips[1].mediaPath, QString("/x/a.mp4"));
        CHECK_EQ(v1.clips[1].start, 50); // rippled by the intro
        CHECK_EQ(v1.clips[2].mediaPath, QString("/x/outro.mp4"));
        CHECK_EQ(v1.clips[2].start, 150);
        CHECK_EQ(v1.clips[2].end(), 180);
    }
    CHECK_EQ(p.timeline().audio[0].clips.size(), 3); // with their sound
    p.undoStack()->undo();
    CHECK_EQ(p.timeline().video[0].clips.size(), 1);
    CHECK(Brand::setGlobal({}));
}

void testCli(const QString& dir)
{
    QFile::remove(Brand::globalPath());
    // A project with a title that references brand colour 1
    ProjectData d;
    d.timeline = emptyTimeline();
    Clip t = titleClip(1, 0, 50);
    t.title.colorRef = 1;
    d.timeline.video[0].clips << t;
    d.lastClipId = 1;
    const QString path = dir + "/cli.schneidi";
    QString err;
    CHECK(ProjectFile::save(d, path, &err));

    QJsonObject o = run("brand", {});
    CHECK(o["global"].toObject()["colors"].toArray().isEmpty());
    CHECK_EQ(errorOf("brand", {{"action", "apply"}}), QString("BAD_ARGUMENT")); // needs a project
    CHECK_EQ(errorOf("brand", {{"action", "set"}, {"colors", "#nothex"}}), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf("brand", {{"action", "set"}, {"logo_corner", "middle"}}), QString("BAD_ARGUMENT"));

    // Setting the global kit with the project: it gets applied there
    o = run("brand", {{"project", path}, {"action", "set"}, {"colors", "#ff0000, #00ff00"}, {"fonts", "Serif"}});
    CHECK_EQ(o["global"].toObject()["colors"].toArray().size(), 2);
    CHECK(o["applied"].toBool());
    CHECK_EQ(o["references"].toInt(), 1);
    ProjectData back;
    CHECK(ProjectFile::load(path, &back, &err));
    CHECK(back.timeline.video[0].clips[0].title.color == QColor("#ff0000"));
    CHECK(!back.brand);
    // Apply again: nothing to do
    o = run("brand", {{"project", path}, {"action", "apply"}});
    CHECK(!o["applied"].toBool());

    // The project's own kit
    o = run("brand", {{"project", path}, {"action", "set"}, {"global", false}, {"colors", "#0000ff"}});
    CHECK(o["project_kit"].isObject());
    CHECK_EQ(o["effective"].toObject()["colors"].toArray()[0].toString(), QString("#0000ff"));
    CHECK_EQ(o["effective"].toObject()["fonts"].toArray()[0].toString(), QString("Serif")); // started from the global
    CHECK(ProjectFile::load(path, &back, &err));
    CHECK(back.brand.has_value());
    CHECK(back.timeline.video[0].clips[0].title.color == QColor("#0000ff"));
    // Brand references in the title op
    run("edit", {{"project", path}, {"ops", R"([{"op":"title","clip":1,"box_color":"brand1","font":"brand1"}])"}});
    CHECK(ProjectFile::load(path, &back, &err));
    const TitleStyle& ts = back.timeline.video[0].clips[0].title;
    CHECK_EQ(ts.boxRef, 1);
    CHECK_EQ(ts.fontRef, 1);
    CHECK_EQ(ts.boxColor.rgb(), QColor("#0000ff").rgb());
    CHECK_EQ(ts.font, QString("Serif"));
    CHECK_EQ(errorOf("edit", {{"project", path}, {"ops", R"([{"op":"title","clip":1,"color":"brand5"}])"}}),
             QString("BAD_ARGUMENT"));
    // A fixed colour ends the reference
    run("edit", {{"project", path}, {"ops", R"([{"op":"title","clip":1,"color":"#ffffff"}])"}});
    CHECK(ProjectFile::load(path, &back, &err));
    CHECK_EQ(back.timeline.video[0].clips[0].title.colorRef, 0);
    // info shows the references
    o = run("info", {{"project", path}});
    CHECK(QJsonDocument(o).toJson().contains("\"box_ref\": \"brand1\""));

    // Unset: the global kit again (applied)
    o = run("brand", {{"project", path}, {"action", "unset"}});
    CHECK(o["project_kit"].isNull());
    CHECK(ProjectFile::load(path, &back, &err));
    CHECK(!back.brand);
    CHECK_EQ(back.timeline.video[0].clips[0].title.boxColor.rgb(), QColor("#ff0000").rgb());
    // Logo without one in the kit
    CHECK_EQ(errorOf("brand", {{"project", path}, {"action", "logo"}}), QString("NOT_FOUND"));
    CHECK(Brand::setGlobal({}));
}

// BRAND_DUMP=<folder>: pictures of the dialog and the title Inspector (brand-dialog.png, brand-inspector.png)
void dump(QWidget* w, const QString& name)
{
    const QString dir = qEnvironmentVariable("BRAND_DUMP");
    if (dir.isEmpty()) return;
    QDir().mkpath(dir);
    w->grab().save(QDir(dir).filePath(name));
}

void testDialog(const QString& dir)
{
    BrandKit global;
    global.colors = {QColor("#ff0000")};
    BrandKitDialog dlg(global, std::nullopt);
    dlg.setColor(1, QColor("#00ff00"));
    dlg.setFont(0, "Serif");
    dlg.setFile("logo", dir + "/logo.png");
    CHECK(!dlg.project());
    CHECK_EQ(dlg.global().colors.size(), 2);
    CHECK_EQ(dlg.global().fonts, QStringList({"Serif"}));
    CHECK_EQ(dlg.global().logo, dir + "/logo.png");
    // This project only: starts as a copy of the global kit, the global one stays as it is
    dlg.setProjectOnly(true);
    dlg.setColor(0, QColor("#0000ff"));
    CHECK(dlg.project() && dlg.project()->colors.first() == QColor("#0000ff"));
    CHECK(dlg.global().colors.first() == QColor("#ff0000"));
    // Removing a colour removes the ones after it (numbers never shift)
    dlg.setColor(0, {});
    CHECK(dlg.project()->colors.isEmpty());
    dlg.setColor(0, QColor("#0000ff"));
    dlg.setColor(1, QColor("#ffcc00"));
    dlg.setFile("intro", dir + "/intro.mp4");
    // Back to the global kit: no own kit (the project had none: "this project only" starts from a copy again)
    dlg.setProjectOnly(false);
    CHECK(!dlg.project());
    dlg.setProjectOnly(true);
    dlg.resize(dlg.sizeHint());
    dlg.show();
    QCoreApplication::processEvents();
    dump(&dlg, "brand-dialog.png");
    // A project with its own kit opens on it
    BrandKitDialog own(global, BrandKit{{}, 3, 0.2, {QColor("#00ffff")}, {}, {}, {}});
    CHECK(own.project() && own.project()->logoCorner == 3);
}

void testInspector()
{
    Project p;
    ProjectData d;
    d.timeline = emptyTimeline();
    BrandKit kit;
    kit.colors = {QColor("#ffcc00"), QColor("#202020")};
    kit.fonts = {"Serif"};
    d.brand = kit;
    p.load(d);
    Selection sel;
    Editor e(&p, &sel);
    Inspector insp(&e);
    e.addTitle(0, -1, 50);
    const int id = *sel.ids().begin();
    insp.resize(380, 900);
    insp.show();
    QCoreApplication::processEvents();
    // The brand buttons: text colour, outline, box, font
    QVector<QToolButton*> buttons;
    for (QToolButton* b : insp.findChildren<QToolButton*>())
        if (b->menu() && b->text().startsWith('B') && b->isVisibleTo(&insp)) buttons << b;
    CHECK_EQ(buttons.size(), 4);
    // Brand colour 1 for the first one (the font button comes first, then text colour): its menu, entry "Brand colour 1"
    bool picked = false;
    for (QToolButton* b : buttons) {
        if (!b->toolTip().contains(T("Fester Wert (nicht aus dem Branding)"))) continue;
        emit b->menu()->aboutToShow();
        const QList<QAction*> actions = b->menu()->actions();
        for (QAction* a : actions)
            if (a->text().startsWith(T("Brand-Farbe %1").arg(1))) {
                a->trigger();
                picked = true;
                break;
            }
        if (picked) break;
    }
    CHECK(picked);
    const Clip* c = TimelineOps::findClip(p.timeline(), id);
    if (CHECK(c)) {
        CHECK_EQ(c->title.colorRef, 1);
        CHECK(c->title.color == QColor("#ffcc00"));
    }
    QCoreApplication::processEvents();
    dump(&insp, "brand-inspector.png");
}

} // namespace

// Two `brand set` on the global kit at once (two AIs, two MCP servers): the later one waits until the other has read,
// changed and written back the kit, then changes that one - neither one's fields get lost
void testGlobalLock()
{
    QFile::remove(Brand::globalPath());
    std::unique_ptr<QLockFile> held = Brand::lockGlobal(0);
    if (!CHECK(held)) return;
    bool wrote = false;
    std::thread other([&] { // the other writer: read the kit, busy for a while, writes it back with its colour
        BrandKit k = Brand::global();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        k.colors = {QColor("#123456")};
        wrote = Brand::setGlobal(k);
        held.reset();
    });
    run("brand", {{"action", "set"}, {"fonts", "DejaVu Sans"}});
    other.join();
    CHECK(wrote);
    const BrandKit k = Brand::global();
    CHECK_EQ(k.colors.size(), 1);
    CHECK_EQ(k.fonts, QStringList{"DejaVu Sans"});
    CHECK(Brand::setGlobal({}));
}

// The app's brand kit dialog: what the user changed there lands on the kit as it is now (a CLI `brand set` made while
// the dialog was open keeps its fields), under the lock; a kit locked for too long is reported, not overwritten
void testChangeGlobal()
{
    BrandKit start;
    start.colors = {QColor("#111111")};
    CHECK(Brand::setGlobal(start));
    // The dialog opened with `start`; meanwhile a CLI sets a font
    BrandKit cli = start;
    cli.fonts = {"DejaVu Sans"};
    CHECK(Brand::setGlobal(cli));
    // The user changed the colour only
    BrandKit edited = start;
    edited.colors = {QColor("#222222")};
    QString err;
    CHECK(Brand::changeGlobal(start, edited, &err));
    BrandKit k = Brand::global();
    CHECK_EQ(k.colors.value(0).name(), QString("#222222"));
    CHECK_EQ(k.fonts, QStringList{"DejaVu Sans"});
    // Nothing changed in the dialog: the file stays as it is
    CHECK(Brand::changeGlobal(edited, edited, &err));
    CHECK_EQ(Brand::global().fonts, QStringList{"DejaVu Sans"});
    // Locked by another schneidi: false + why, the kit untouched
    {
        std::unique_ptr<QLockFile> held = Brand::lockGlobal(0);
        if (CHECK(held)) {
            BrandKit other = edited;
            other.logoSize = 0.3;
            CHECK(!Brand::changeGlobal(edited, other, &err, 100));
            CHECK(!err.isEmpty());
            CHECK_EQ(Brand::global().logoSize, k.logoSize);
        }
    }
    CHECK(Brand::setGlobal({}));
}

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("brand-kit");
    Theme::apply(app); // as in the app (pictures)
    Check::initEngine();
    QTemporaryDir dir;
    testKitJson(dir.path());
    testGlobal(dir.path());
    testProjectFile(dir.path());
    testApplyBrand();
    testCombinedBrand();
    testAnimatedColor();
    testLogoAndIntro(dir.path());
    testCli(dir.path());
    testGlobalLock();
    testChangeGlobal();
    testDialog(dir.path());
    testInspector();
    return Check::result();
}
