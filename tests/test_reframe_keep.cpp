// SPDX-License-Identifier: AGPL-3.0-only
// Auto-Reframe keeps the kill feed: game detection from folder/file name (core/GameHud), the kept-region copy's
// geometry (Reframe::keepCopy), the Editor op (copy on a free track above, one undo step, replaced on a new reframe,
// removed with none, never reframed itself, project round trip), the CLI (op reframe keep/game, new_timeline, info,
// errors), the rendered 9:16 picture (the kill feed line shows at the top) and highlights reading a game's kill feed
// by itself with the tighter CS2 region (game picture left of the lines is no kill).
#include "check.h"

#include "app/InputBindings.h"
#include "app/MainWindow.h"
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/Editor.h"
#include "core/GameHud.h"
#include "core/MediaAspect.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Reframe.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "engine/Snapshot.h"
#include "ui/MediaPool.h"

#include <QAction>
#include <QApplication>
#include <QSettings>
#include <QDir>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QUndoStack>
#include <clocale>
#include <cmath>

namespace {

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

void testGames()
{
    CHECK(GameHud::find("CS2") && GameHud::find("cs2")->id == "cs2");
    CHECK(GameHud::find(" valorant ") != nullptr);
    CHECK(!GameHud::find("tetris"));
    const GameHud::Game* g = GameHud::guess("/clips/cs2/clip_2026.mkv");
    CHECK(g && g->id == "cs2");
    g = GameHud::guess("/videos/cs2_clip_2026-10-22_23-15-53.mkv");
    CHECK(g && g->id == "cs2");
    g = GameHud::guess("/videos/Valorant-ace.mp4");
    CHECK(g && g->id == "valorant");
    CHECK(!GameHud::guess("/videos/cs2clip.mp4"));
    CHECK(!GameHud::guess("/videos/holiday.mp4"));
    CHECK_EQ(GameHud::regionText(QRectF(0.7, 0.055, 0.3, 0.19)), QString("0.7,0.055,0.3,0.19"));
    // Every game: the mark lies inside the kill feed that a short keeps
    for (const GameHud::Game& x : GameHud::games()) {
        CHECK(QRectF(0, 0, 1, 1).contains(x.feed));
        CHECK(x.feed.adjusted(-1e-9, -1e-9, 1e-9, 1e-9).intersects(x.mark));
    }
    // CS2: five kill feed lines (31 px tall, 34 px apart from y 75 in 1080p: the fifth ends at 241) fit, and the copy
    // in a 9:16 short stays at most 15 % of the frame height
    const QRectF cs2 = GameHud::find("cs2")->feed;
    CHECK(cs2.top() * 1080 <= 75 - 3 && cs2.bottom() * 1080 >= 241 + 3);
    const Clip copy = Reframe::keepCopy(Clip{}, {"killfeed", cs2}, 16.0 / 9.0, QSizeF(1080, 1920));
    CHECK(copy.transform.zoomY * cs2.height() * 607.5 <= 0.15 * 1920 + 1e-6);
}

void testGeometry()
{
    Clip base;
    base.id = 7;
    base.linkId = 3;
    base.mediaPath = "/x/cs2/a.mkv";
    base.in = 10;
    base.out = 109;
    base.effects << EffectInstance{"grade", {}, true};
    base.keys[AnimParam::PosX] = {Keyframe{10, 100, KeyEase::Linear}, Keyframe{50, -100, KeyEase::Linear}};
    base.keys[AnimParam::Opacity] = {Keyframe{10, 0, KeyEase::Linear}, Keyframe{20, 100, KeyEase::Linear}};
    const QSizeF frame(1080, 1920);
    const QRectF region(0.7, 0.055, 0.3, 0.19);
    const Reframe::KeepPlace place;
    const Clip c = Reframe::keepCopy(base, {"killfeed", region}, 16.0 / 9.0, frame);
    CHECK_EQ(c.id, 0);
    CHECK_EQ(c.linkId, 0); // no sound
    CHECK_EQ(c.keepRegion, QString("killfeed"));
    CHECK_EQ(c.in, 10);
    CHECK_EQ(c.out, 109);
    CHECK_EQ(c.effects.size(), 1); // looks like the picture below
    CHECK(!c.keys.contains(AnimParam::PosX));
    CHECK(c.keys.contains(AnimParam::Opacity)); // fades with it
    CHECK(!Reframe::canReframe(c));
    CHECK(Reframe::isKeepOf(c, base));
    Clip moved = base;
    moved.start = 5;
    CHECK(!Reframe::isKeepOf(c, moved));
    CHECK(!Reframe::isKeepOf(base, base)); // only copies count
    // The region on the fitted 1080x607.5 picture, cropped exactly
    const double fw = 1080, fh = 607.5;
    const ClipTransform& t = c.transform;
    CHECK(near(t.cropLeft, 0.7 * fw, 1e-6));
    CHECK(near(t.cropRight, 0, 1e-6));
    CHECK(near(t.cropTop, 0.055 * fh, 1e-6));
    CHECK(near(t.cropBottom, (1 - 0.245) * fh, 1e-6));
    CHECK(t.fill == FitFill::None);
    // Where the region ends up (frame pixels from the top left): centred, its top at place.top, width/height limits
    const double z = t.zoomX;
    CHECK_EQ(t.zoomY, z);
    const double w = z * region.width() * fw, h = z * region.height() * fh;
    CHECK(w <= place.width * frame.width() + 1e-6 && h <= place.maxHeight * frame.height() + 1e-6);
    CHECK(near(w, place.width * frame.width(), 1e-6) || near(h, place.maxHeight * frame.height(), 1e-6));
    const double centreX = frame.width() / 2 + t.posX + z * (region.center().x() - 0.5) * fw;
    const double top = frame.height() / 2 - (t.posY - z * (region.top() - 0.5) * fh);
    CHECK(near(centreX, frame.width() / 2, 1e-6));
    CHECK(near(top, place.top * frame.height(), 1e-6));
    // A game between black bars: its region inside them
    const QRectF in = Reframe::inContent(QRectF(0.5, 0, 0.5, 0.5), QRectF(0.125, 0, 0.75, 1));
    CHECK(near(in.x(), 0.5, 1e-9) && near(in.width(), 0.375, 1e-9) && near(in.height(), 0.5, 1e-9));
    // Only a crop of the sides loses the kill feed
    CHECK(Reframe::cropsSides(Reframe::Mode::Auto, 16.0 / 9.0, QSizeF(1080, 1920)));
    CHECK(Reframe::cropsSides(Reframe::Mode::Center, 16.0 / 9.0, QSizeF(1080, 1080)));
    CHECK(!Reframe::cropsSides(Reframe::Mode::Fit, 16.0 / 9.0, QSizeF(1080, 1920)));
    CHECK(!Reframe::cropsSides(Reframe::Mode::Auto, 16.0 / 9.0, QSizeF(1920, 1080)));
}

int videoClips(const Timeline& tl)
{
    int n = 0;
    for (const Track& t : tl.video) n += int(t.clips.size());
    return n;
}

const Clip* keptCopy(const Timeline& tl, int* track = nullptr)
{
    for (int i = 0; i < tl.video.size(); ++i)
        for (const Clip& c : tl.video[i].clips)
            if (!c.keepRegion.isEmpty()) {
                if (track) *track = i;
                return &c;
            }
    return nullptr;
}

void testEditor(const QString& dir)
{
    MediaAspect::setProbe([](const QString&) { return 16.0 / 9.0; });
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    const int seq = p.addTimeline("Short", 0, QSize(1080, 1920));
    p.setCurrentSequence(seq);
    p.addMedia({"/x/cs2/a.mkv", "a.mkv", 100, true, true, false});
    p.addMedia({"/x/holiday.mp4", "holiday.mp4", 100, true, true, false});
    int game = 0, other = 0;
    p.edit("setup", [&](Timeline& tl) {
        Clip v;
        v.id = game = p.newClipId();
        v.mediaPath = "/x/cs2/a.mkv";
        v.out = 49;
        v.linkId = 1;
        tl.video[0].clips << v;
        Clip a = v;
        a.id = p.newClipId();
        tl.audio[0].clips << a;
        Clip h;
        h.id = other = p.newClipId();
        h.mediaPath = "/x/holiday.mp4";
        h.start = 50;
        h.out = 49;
        tl.video[0].clips << h;
        // A title above the game clip: the copy must not overwrite it
        TimelineOps::ensureTracks(tl, TrackKind::Video, 2);
        Clip t;
        t.id = p.newClipId();
        t.kind = ClipKind::Title;
        t.start = 20;
        t.out = 9;
        tl.video[1].clips << t;
    });
    const QVector<int> ids = ed.reframeTargets({});
    CHECK_EQ(ids.size(), 2);
    QVector<int> noGame;
    auto keeps = ed.reframeKeeps(ids, Reframe::Mode::Auto, {}, {}, &noGame);
    CHECK_EQ(keeps.value(game).size(), 1);
    CHECK(keeps.value(other).isEmpty()); // not a game
    CHECK(noGame.isEmpty());
    if (!keeps.value(game).isEmpty()) {
        CHECK_EQ(keeps[game][0].id, QString("killfeed"));
        CHECK(keeps[game][0].region == GameHud::find("cs2")->feed);
    }
    const int before = p.undoStack()->index();
    CHECK_EQ(ed.reframeClips(ids, Reframe::Mode::Center, {}, {}, &keeps), 2);
    CHECK_EQ(p.undoStack()->index(), before + 1); // reframe and copy: one step
    int track = -1;
    const Clip* k = keptCopy(p.timeline(), &track);
    if (CHECK(k)) {
        CHECK_EQ(track, 2); // V2 holds the title over the clip's range: the next free track
        CHECK_EQ(k->start, 0);
        CHECK_EQ(k->out, 49);
        CHECK_EQ(k->linkId, 0);
        CHECK(k->id != game);
    }
    CHECK_EQ(p.timeline().audio[0].clips.size(), 1); // no sound added
    CHECK_EQ(ed.reframeTargets({}).size(), 2);       // the copy is never reframed itself
    // Again: the copy is replaced, not doubled
    keeps = ed.reframeKeeps(ed.reframeTargets({}), Reframe::Mode::Auto, {}, {});
    ed.reframeClips({}, Reframe::Mode::Center, {}, {}, &keeps);
    CHECK_EQ(videoClips(p.timeline()), 4);
    // The clip trimmed since (the copy is not linked to it), then reframed again: still one copy, as long as the clip
    p.edit("trim", [&](Timeline& tl) {
        Clip* g = TimelineOps::findClip(tl, game);
        g->start += 10;
        g->in += 10;
    });
    keeps = ed.reframeKeeps({game}, Reframe::Mode::Auto, {}, {});
    ed.reframeClips({game}, Reframe::Mode::Center, {}, {}, &keeps);
    {
        int copies = 0;
        for (const Track& t : p.timeline().video)
            for (const Clip& x : t.clips)
                if (!x.keepRegion.isEmpty()) {
                    ++copies;
                    CHECK(x.start == 10 && x.in == 10 && x.out == 49);
                }
        CHECK_EQ(copies, 1);
    }
    p.undoStack()->undo();
    p.undoStack()->undo();
    CHECK_EQ(videoClips(p.timeline()), 4);
    // Without keeps given the copy stays; none removes it
    ed.reframeClips({}, Reframe::Mode::Center);
    CHECK(keptCopy(p.timeline()));
    // Project round trip
    {
        const QString path = dir + "/keep.schneidi";
        QString error;
        CHECK(ProjectFile::save(p.data(), path, &error));
        ProjectData d;
        CHECK(ProjectFile::load(path, &d, &error));
        Project q;
        q.load(d);
        q.setCurrentSequence(seq);
        const Clip* l = keptCopy(q.timeline());
        if (CHECK(l)) CHECK(*l == *keptCopy(p.timeline()));
    }
    Reframe::KeepChoice none;
    none.kind = Reframe::KeepChoice::None;
    keeps = ed.reframeKeeps({game}, Reframe::Mode::Center, {}, none);
    ed.reframeClips({game}, Reframe::Mode::Center, {}, {}, &keeps);
    CHECK(!keptCopy(p.timeline()));
    CHECK_EQ(videoClips(p.timeline()), 3);
    p.undoStack()->undo();
    CHECK(keptCopy(p.timeline()));
    // A kill feed asked for where no game is known; the game given; a custom region
    Reframe::KeepChoice feed;
    feed.kind = Reframe::KeepChoice::KillFeed;
    noGame.clear();
    keeps = ed.reframeKeeps({other}, Reframe::Mode::Center, {}, feed, &noGame);
    CHECK_EQ(noGame, QVector<int>{other});
    feed.game = "valorant";
    keeps = ed.reframeKeeps({other}, Reframe::Mode::Center, {}, feed);
    CHECK(keeps.value(other).size() == 1 && keeps[other][0].region == GameHud::find("valorant")->feed);
    Reframe::KeepChoice custom;
    custom.kind = Reframe::KeepChoice::Custom;
    custom.region = QRectF(0, 0, 0.25, 0.25);
    keeps = ed.reframeKeeps({other}, Reframe::Mode::Fit, {}, custom);
    CHECK(keeps.value(other).size() == 1 && keeps[other][0].id == "custom");
    // Black bars around the game: the kill feed inside them
    keeps = ed.reframeKeeps({game}, Reframe::Mode::Center, {{"/x/cs2/a.mkv", QRectF(0.125, 0, 0.75, 1)}}, {});
    if (CHECK_EQ(keeps.value(game).size(), 1))
        CHECK(near(keeps[game][0].region.right(), 0.125 + 0.75 * GameHud::find("cs2")->feed.right(), 1e-9));
    // In the recording's own shape nothing is cut away: nothing kept
    const int wide = p.addTimeline("Wide", 0, QSize(1920, 1080));
    p.setCurrentSequence(wide);
    p.edit("clip", [&](Timeline& tl) {
        Clip v;
        v.id = p.newClipId();
        v.mediaPath = "/x/cs2/a.mkv";
        v.out = 49;
        tl.video[0].clips << v;
    });
    keeps = ed.reframeKeeps(ed.reframeTargets({}), Reframe::Mode::Auto, {}, {});
    CHECK(keeps.size() == 1 && keeps.begin()->isEmpty());
    MediaAspect::setProbe({});
}

void testPictureAspect()
{
    // A reframed clip set to another shape afterwards (stretched 4:3): its zoom and camera path follow the new shape
    // (still filling the 9:16 frame, the same window centres), the kill feed copy takes the shape and its region
    // again, all in one undo step
    MediaAspect::setProbe([](const QString&) { return 16.0 / 9.0; });
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    const QSizeF frame(1080, 1920);
    p.setCurrentSequence(p.addTimeline("Short", 0, frame.toSize()));
    p.addMedia({"/x/cs2/a.mkv", "a.mkv", 100, true, false, false});
    int game = 0;
    p.edit("setup", [&](Timeline& tl) {
        Clip v;
        v.id = game = p.newClipId();
        v.mediaPath = "/x/cs2/a.mkv";
        v.out = 49;
        tl.video[0].clips << v;
    });
    const auto keeps = ed.reframeKeeps({game}, Reframe::Mode::Auto, {}, {});
    const QHash<int, QVector<Reframe::Key>> paths{{game, {{0, 0.3, 0.5}, {30, 0.7, 0.5}}}};
    ed.reframeClips({game}, Reframe::Mode::Auto, paths, {}, &keeps);
    const QRectF feed = GameHud::find("cs2")->feed;
    const int before = p.undoStack()->index();
    CHECK_EQ(ed.setPictureAspect({game}, 4.0 / 3), 1);
    CHECK_EQ(p.undoStack()->index(), before + 1);
    const Clip& c = *TimelineOps::findClip(p.timeline(), game);
    const Reframe::Fill f = Reframe::fill(4.0 / 3, frame);
    CHECK(near(c.transform.zoomX, f.zoom, 1e-6));
    CHECK(near(c.transform.zoomY, f.zoom, 1e-6));
    const KeyTrack px = c.keys.value(AnimParam::PosX);
    if (CHECK_EQ(px.size(), 2)) {
        CHECK(near(px[0].value, Reframe::position(f, frame, 0.3, 0.5).x(), 1e-6));
        CHECK(near(px[1].value, Reframe::position(f, frame, 0.7, 0.5).x(), 1e-6));
    }
    CHECK(near(c.transform.posX, Reframe::position(f, frame, 0.3, 0.5).x(), 1e-6));
    const Clip* k = keptCopy(p.timeline());
    if (CHECK(k)) {
        CHECK(near(k->pictureAspect, 4.0 / 3, 1e-9));
        const Clip want = Reframe::keepCopy(c, {"killfeed", feed}, 4.0 / 3, frame);
        CHECK(near(k->transform.zoomX, want.transform.zoomX, 1e-6));
        CHECK(near(k->transform.posX, want.transform.posX, 1e-6));
        CHECK(near(k->transform.posY, want.transform.posY, 1e-6));
        CHECK(near(k->transform.cropLeft, want.transform.cropLeft, 1e-6));
        CHECK(near(k->transform.cropBottom, want.transform.cropBottom, 1e-6));
    }
    // Back to the file's shape: as reframed
    ed.setPictureAspect({game}, 0);
    const Reframe::Fill wide = Reframe::fill(16.0 / 9, frame);
    CHECK(near(TimelineOps::findClip(p.timeline(), game)->transform.zoomX, wide.zoom, 1e-6));
    if (const Clip* back = keptCopy(p.timeline()); CHECK(back)) {
        CHECK_EQ(back->pictureAspect, 0.0);
        CHECK(near(back->transform.zoomX, Reframe::keepCopy(c, {"killfeed", feed}, 16.0 / 9, frame).transform.zoomX, 1e-6));
    }
    // A copy moved by hand keeps its place (only the shape and the crop's share follow)
    p.edit("move", [&](Timeline& tl) {
        for (Track& t : tl.video)
            for (Clip& x : t.clips)
                if (!x.keepRegion.isEmpty()) x.transform.posY -= 100;
    });
    const double moved = keptCopy(p.timeline())->transform.posY;
    ed.setPictureAspect({game}, 4.0 / 3);
    CHECK(near(keptCopy(p.timeline())->transform.posY, moved, 1e-6));
    CHECK(near(keptCopy(p.timeline())->pictureAspect, 4.0 / 3, 1e-9));
    // One undo: the clip and the copy as before
    p.undoStack()->undo();
    CHECK_EQ(keptCopy(p.timeline())->pictureAspect, 0.0);
    CHECK(near(TimelineOps::findClip(p.timeline(), game)->transform.zoomX, wide.zoom, 1e-6));
    MediaAspect::setProbe({});
}

// Red pixels (the kill feed line) in a part of a picture
int redIn(const QImage& img, const QRect& r)
{
    int n = 0;
    for (int y = std::max(0, r.top()); y <= std::min(img.height() - 1, r.bottom()); ++y)
        for (int x = std::max(0, r.left()); x <= std::min(img.width() - 1, r.right()); ++x) {
            const QRgb c = img.pixel(x, y);
            n += qRed(c) > 170 && qGreen(c) < 110 && qBlue(c) < 110;
        }
    return n;
}

void testCli(const QString& dir, const QString& game, const QString& plain)
{
    auto run = [&](const QString& name, const QJsonObject& args) -> const QJsonObject {
        const Cli::Command* cmd = Cli::find(name);
        Cli::Context ctx;
        return cmd->run(Cli::normalize(*cmd, args), ctx);
    };
    auto errorOf = [&](const QString& name, const QJsonObject& args) {
        try {
            run(name, args);
        } catch (const Cli::Error& e) {
            return e.code;
        }
        return QString("none");
    };

    // highlights: the game from the folder, its kill feed read without region or colour. The red patch left of
    // the lines (game picture, at 3.5 s) was a kill with the old wide CS2 region; the tight one sees one kill
    const QJsonObject h = run("highlights", {{"file", game}});
    CHECK_EQ(h["game"].toString(), QString("cs2"));
    CHECK_EQ(h["region"].toString(), GameHud::regionText(GameHud::find("cs2")->mark));
    int kills = 0;
    for (const QJsonValue& c : h["candidates"].toArray()) kills += c["kills"].toInt();
    CHECK_EQ(kills, 1);
    if (CHECK(!h["candidates"].toArray().isEmpty())) CHECK(near(h["candidates"][0]["kill_s"][0].toDouble(), 2.0, 0.2));
    CHECK(h["kill_feed_most_pct"].toDouble() < 0.3);
    int wideKills = 0;
    for (const QJsonValue& c : run("highlights", {{"file", game}, {"region", "0.72,0.04,0.28,0.2"}, {"color", "#d8444a"},
                                                  {"tolerance", 40}})["candidates"].toArray())
        wideKills += c["kills"].toInt();
    CHECK_EQ(wideKills, 2);
    const QJsonObject loud = run("highlights", {{"file", game}, {"game", "none"}});
    CHECK(!loud.contains("game") && !loud.contains("kill_feed_most_pct")); // the sound only
    const QJsonObject forced = run("highlights", {{"file", plain}, {"game", "cs2"}});
    CHECK_EQ(forced["game"].toString(), QString("cs2"));
    CHECK(!run("highlights", {{"file", plain}}).contains("game"));
    CHECK_EQ(errorOf("highlights", {{"file", game}, {"game", "tetris"}}), QString("BAD_ARGUMENT"));

    // A 16:9 edit of both files, copied as a 9:16 short: the game clip keeps its kill feed at the top
    const QString path = dir + "/keep-cli.schneidi";
    int source = 0;
    {
        Project p;
        p.addMedia({game, "clip.mkv", 150, true, true, false});
        p.addMedia({plain, "plain.mkv", 150, true, true, false});
        p.edit("setup", [&](Timeline& tl) {
            Clip v;
            v.id = p.newClipId();
            v.mediaPath = game;
            v.out = 149;
            tl.video[0].clips << v;
            Clip w;
            w.id = p.newClipId();
            w.mediaPath = plain;
            w.start = 150;
            w.out = 149;
            tl.video[0].clips << w;
        });
        source = p.currentSequence();
        QString error;
        CHECK(ProjectFile::save(p.data(), path, &error));
    }
    const QJsonObject made = run("edit", {{"project", path}, {"ops", QString(R"([{"op":"new_timeline","from":%1,
        "name":"Short","aspect":"9:16","reframe":"center"}])").arg(source)}});
    bool noted = false;
    for (const QJsonValue& n : made["notes"].toArray()) noted |= n.toString().startsWith("kept readable");
    CHECK(noted);
    const auto copies = [&] {
        int n = 0;
        for (const QJsonValue& t : run("info", {{"project", path}, {"timeline", "Short"}})["timeline"]["tracks"].toArray())
            for (const QJsonValue& c : t["clips"].toArray()) n += c["keep_region"].toString() == "killfeed";
        return n;
    };
    CHECK_EQ(copies(), 1);
    // The rendered short: the kill feed line (top right of the recording, cut away by the crop) at the top
    const auto picture = [&] {
        ProjectData d;
        QString error;
        CHECK(ProjectFile::load(path, &d, &error));
        Project q;
        q.load(d);
        for (const Sequence& s : q.sequences())
            if (s.name == "Short") q.setCurrentSequence(s.id);
        return Snapshot::Source::timeline(q.timelineFormat(), q.timeline())->frame(75).convertToFormat(QImage::Format_RGB32);
    };
    QImage img = picture();
    if (CHECK(!img.isNull())) {
        const int W = img.width(), H = img.height();
        const QRect top(0, int(H * 0.1) - 2, W, int(H * 0.15) + 4);
        CHECK(redIn(img, top) > W * 2); // the line, scaled up
        CHECK_EQ(redIn(img, QRect(0, int(H * 0.3), W, H - int(H * 0.3))), 0); // the crop below shows no kill feed
        img.save(dir + "/keep-short.png");
        // A thin dark frame around the copy (the same grey as the picture behind it): above its top edge (at 10 % of the
        // frame height) and left of it, about 0.6 % of the frame width; the grey inside and further out untouched.
        // (This 640x360 recording is scaled up ~4.5x, so the copy's own edge is soft over a few pixels.)
        const double frameWidth = 0.006 * std::min(W, H);
        const int edge = int(H * 0.1);
        const auto grey = [&](int x, int y) { return qGreen(img.pixel(x, y)); };
        int darkest = 255;
        for (int y = int(edge - frameWidth - 1); y <= edge; ++y) darkest = std::min(darkest, grey(W / 2, y));
        CHECK(darkest < 30);
        CHECK(near(grey(W / 2, int(edge - 2 * frameWidth - 2)), 0x40, 4));
        CHECK(near(grey(W / 2, int(edge + 2 * frameWidth)), 0x40, 4));
        const int row = int(H * 0.15);
        int darkX = -1;
        for (int x = 0; x < W / 2 && darkX < 0; ++x)
            if (grey(x, row) < 40) darkX = x;
        CHECK(darkX > 0 && near(grey(darkX - 3, row), 0x40, 4) && near(grey(int(darkX + 3 * frameWidth), row), 0x40, 4));
    }
    // none removes it, a custom region brings one back, killfeed on a file of no known game fails
    run("edit", {{"project", path}, {"ops", R"([{"op":"timeline","timeline":"Short"},{"op":"reframe","mode":"center","keep":"none"}])"}});
    CHECK_EQ(copies(), 0);
    img = picture();
    if (CHECK(!img.isNull())) CHECK_EQ(redIn(img, img.rect()), 0);
    run("edit", {{"project", path}, {"ops", R"([{"op":"timeline","timeline":"Short"},{"op":"reframe","mode":"center"}])"}});
    CHECK_EQ(copies(), 1); // auto again
    run("edit", {{"project", path}, {"ops", R"([{"op":"timeline","timeline":"Short"},
        {"op":"reframe","mode":"center","keep":"0.5,0,0.5,0.3"}])"}});
    int customs = 0;
    for (const QJsonValue& t : run("info", {{"project", path}, {"timeline", "Short"}})["timeline"]["tracks"].toArray())
        for (const QJsonValue& c : t["clips"].toArray()) customs += c["keep_region"].toString() == "custom";
    CHECK_EQ(customs, 2); // both clips
    CHECK_EQ(copies(), 0);
    const QString ops = R"([{"op":"timeline","timeline":"Short"},{"op":"reframe","mode":"center","keep":%1}])";
    CHECK_EQ(errorOf("edit", {{"project", path}, {"ops", ops.arg("\"killfeed\"")}}), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf("edit", {{"project", path}, {"ops", ops.arg("\"0.5,0.5,0.8,0.1\"")}}), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf("edit", {{"project", path}, {"ops", ops.arg("\"wild\"")}}), QString("BAD_ARGUMENT"));
    // "nan" reads as a number but is none (it passed every range check and cropped the copy by NaN)
    CHECK_EQ(errorOf("edit", {{"project", path}, {"ops", ops.arg("\"nan,0,0.5,0.5\"")}}), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf("edit", {{"project", path}, {"ops", ops.arg("\"0,0,nan,0.5\"")}}), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf("edit", {{"project", path}, {"ops", R"([{"op":"reframe","game":"tetris"}])"}}), QString("BAD_ARGUMENT"));
    run("edit", {{"project", path}, {"ops", R"([{"op":"timeline","timeline":"Short"},
        {"op":"reframe","mode":"center","keep":"killfeed","game":"valorant"}])"}});
    CHECK_EQ(copies(), 2);
    bool helped = false;
    for (const QJsonValue& v : Cli::detail::editOpsHelp())
        helped |= v["op"].toString() == "reframe" && v["usage"].toString().contains("keep");
    CHECK(helped);
}

// The app: the clip menu's Auto-Reframe and the Media Pool's 9:16 copy keep the kill feed of game footage (one undo
// step); the menu switch turns it off
void testApp(const QString& game)
{
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return;
    std::setlocale(LC_NUMERIC, "C");
    QSettings().remove("reframe/keepKillFeed");
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1400, 900);
    w.show();
    w.importFiles({game}, true);
    auto* p = w.findChild<Project*>();
    auto* pool = w.findChild<MediaPool*>();
    if (!CHECK(p && pool) || !CHECK(p->timeline().video.size() > 0 && p->timeline().video[0].clips.size() == 1)) return;
    const int steps = p->undoStack()->index();
    emit pool->reframedCopyRequested(p->currentSequence());
    CHECK_EQ(p->undoStack()->index(), steps + 1); // copy, reframe and kill feed: one step
    CHECK_EQ(p->timelineFormat().size(), QSize(1080, 1920));
    CHECK(keptCopy(p->timeline()));
    QAction* keep = InputBindings::instance().action("reframe_keep_killfeed");
    QAction* reframe = InputBindings::instance().action("auto_reframe");
    if (CHECK(keep && reframe)) {
        CHECK(keep->isChecked()); // on by default
        keep->setChecked(false);
        CHECK(!QSettings().value("reframe/keepKillFeed", true).toBool());
        reframe->trigger();
        CHECK(!keptCopy(p->timeline())); // off: the copy goes
        CHECK_EQ(p->undoStack()->index(), steps + 2);
        keep->setChecked(true);
        reframe->trigger();
        CHECK(keptCopy(p->timeline()));
        CHECK_EQ(videoClips(p->timeline()), 2);
    }
    QSettings().remove("reframe/keepKillFeed");
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("reframe_keep");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    testGames();
    testGeometry();
    testEditor(tmp.path());
    testPictureAspect();
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    // A CS2-like recording (640x360): a kill feed line of the player (a thin red frame, top right, inside the CS2 mark region) at
    // 2 s, and at 3.5 s a red patch of game picture left of the lines (inside the old wide region only); shots in
    // the sound for the loud moments
    QDir(tmp.path()).mkpath("cs2");
    const QStringList args{"-f", "lavfi", "-i",
                           "color=c=0x404040:s=640x360:d=6:r=25,"
                           "drawbox=x=470:y=26:w=165:h=14:c=0xd8444a:t=2:enable='gte(t,2)',"
                           "drawbox=x=470:y=60:w=40:h=20:c=0xd8444a:t=fill:enable='gte(t,3.5)'",
                           "-f", "lavfi", "-i",
                           "aevalsrc='0.01*(random(0)-0.5)+if(between(t,1.9,1.95)+between(t,4,4.05),0.8*sin(2*PI*900*t),0)'"
                           ":s=48000:d=6",
                           "-c:v", "ffv1", "-pix_fmt", "bgr0", "-c:a", "pcm_s16le", "-shortest"};
    const QString game = Check::makeMedia(tmp.filePath("cs2/clip.mkv"), args);
    const QString plain = Check::makeMedia(tmp.filePath("plain.mkv"), args);
    if (!CHECK(!game.isEmpty() && !plain.isEmpty())) return Check::result();
    Check::initEngine();
    std::setlocale(LC_NUMERIC, "C");
    testCli(tmp.path(), game, plain);
    testApp(game);
    return Check::result();
}
