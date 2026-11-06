// SPDX-License-Identifier: AGPL-3.0-only
// Auto-Reframe keeps the kill feed: game detection from folder/file name (core/GameHud), the kept-region copy's
// geometry (Reframe::keepCopy), the Editor op (copy on a free track above, one undo step, replaced on a new reframe,
// removed with none, never reframed itself, project round trip), the CLI (op reframe keep/game, new_timeline, info,
// errors), the rendered 9:16 picture (the kill feed line shows at the top; its frame stays cheap zoomed far in) and
// highlights reading a game's kill feed by itself with the tighter CS2 region (game picture left of the lines is no
// kill).
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
#include "ui/timeline/TimelineView.h"

#include <QAction>
#include <QApplication>
#include <QStatusBar>
#include <QSettings>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QUndoStack>
#include <array>
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
        // A feed at the edge (0.7 + 0.3) can end a rounding step past 1 (x87 on 32-bit x86)
        CHECK(QRectF(-1e-9, -1e-9, 1 + 2e-9, 1 + 2e-9).contains(x.feed));
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
        CHECK_EQ(k->linkId, 1); // linked to its clip (and its sound): it follows the clip
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

// The kill feed copies of a timeline (track index, clip), by start
QVector<QPair<int, Clip>> keptCopies(const Timeline& tl)
{
    QVector<QPair<int, Clip>> out;
    for (int i = 0; i < tl.video.size(); ++i)
        for (const Clip& c : tl.video[i].clips)
            if (!c.keepRegion.isEmpty()) out << qMakePair(i, c);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second.start < b.second.start; });
    return out;
}

// Does the copy show the same time of the file as its clip?
bool inSync(const Clip& copy, const Clip& base)
{
    return copy.start == base.start && copy.in == base.in && copy.out == base.out && copy.speed == base.speed
           && copy.reverse == base.reverse && copy.freeze == base.freeze && copy.freezeFrame == base.freezeFrame
           && copy.ramp == base.ramp && copy.interpolate == base.interpolate && copy.enabled == base.enabled;
}

// The kill feed copy follows its clip (linked to it): moving (in time, to another track, with ripple), trimming,
// splitting, deleting, retiming - each one undo step; selecting the clip does not select the copy, moving the copy by
// hand still moves it alone; the link survives the project file
void testFollow(const QString& dir)
{
    MediaAspect::setProbe([](const QString&) { return 16.0 / 9.0; });
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    const int seq = p.addTimeline("Short", 0, QSize(1080, 1920));
    p.setCurrentSequence(seq);
    p.addMedia({"/x/cs2/a.mkv", "a.mkv", 400, true, true, false});
    int lead = 0, game = 0, sound = 0, after = 0;
    p.edit("setup", [&](Timeline& tl) {
        Clip l; // a clip before it on V1 (ripple)
        l.id = lead = p.newClipId();
        l.mediaPath = "/x/cs2/a.mkv";
        l.in = 300;
        l.out = 319;
        tl.video[0].clips << l;
        Clip v;
        v.id = game = p.newClipId();
        v.mediaPath = "/x/cs2/a.mkv";
        v.start = 20;
        v.in = 100;
        v.out = 159;
        v.linkId = p.newLinkId();
        tl.video[0].clips << v;
        Clip a = v;
        a.id = sound = p.newClipId();
        tl.audio[0].clips << a;
        Clip w = l; // after it on V1
        w.id = after = p.newClipId();
        w.start = 80;
        tl.video[0].clips << w;
    });
    const auto keeps = ed.reframeKeeps({game}, Reframe::Mode::Center, {}, {});
    ed.reframeClips({game}, Reframe::Mode::Center, {}, {}, &keeps);
    const auto base = [&] { return *TimelineOps::findClip(p.timeline(), game); };
    const auto copies = [&] { return keptCopies(p.timeline()); };
    if (!CHECK_EQ(copies().size(), 1)) return;
    const int copy = copies()[0].second.id;
    CHECK_EQ(copies()[0].first, 1);
    CHECK(copies()[0].second.linkId != 0 && copies()[0].second.linkId == base().linkId);
    CHECK_EQ(TimelineOps::findClip(p.timeline(), sound)->linkId, base().linkId); // the sound stays linked too
    // Selecting the clip takes its sound, not the copy; the copy alone is just the copy
    const QVector<int> group = ed.withLinked({game});
    CHECK(group.contains(sound) && !group.contains(copy));
    CHECK_EQ(ed.withLinked({copy}), QVector<int>{copy});
    const auto step = [&](const char* what, const std::function<void()>& change, const std::function<void()>& verify) {
        const Timeline was = p.timeline();
        const int index = p.undoStack()->index();
        change();
        if (!CHECK_EQ(p.undoStack()->index(), index + 1)) qWarning("  %s: not one undo step", what);
        verify();
        p.undoStack()->undo();
        if (!CHECK(p.timeline() == was)) qWarning("  %s: undo left another timeline", what);
    };
    const auto synced = [&](const char* what) {
        const auto k = copies();
        if (!CHECK_EQ(k.size(), 1) || !CHECK(inSync(k[0].second, base())))
            qWarning("  %s: the copy did not follow", what);
    };
    // Move in time: the copy stays on its own track
    step("move", [&] { ed.moveClips(group, 30, TrackKind::Video, 0); }, [&] {
        synced("move");
        CHECK_EQ(base().start, 50);
        CHECK_EQ(copies().value(0).first, 1);
    });
    // To another track: it stays above (V3, a new track)
    step("move up", [&] { ed.moveClips(group, 0, TrackKind::Video, 1); }, [&] {
        synced("move up");
        TrackRef ref;
        TimelineOps::findClip(p.timeline(), game, &ref);
        CHECK_EQ(ref.index, 1);
        CHECK_EQ(copies().value(0).first, 2);
    });
    // Ripple delete of the clip before: everything moves up
    step("ripple before", [&] { sel.set({lead}); ed.rippleDeleteSelection(); }, [&] {
        synced("ripple before");
        CHECK_EQ(base().start, 0);
    });
    // Trim both ends
    step("trim end", [&] { ed.trimClip(game, TimelineOps::Edge::End, -15); }, [&] { synced("trim end"); });
    step("trim start", [&] { ed.trimClip(game, TimelineOps::Edge::Start, 10); }, [&] {
        synced("trim start");
        CHECK_EQ(base().start, 30);
    });
    // Ripple trim (trim mode)
    step("ripple trim", [&] {
        ed.applyTrimEdit(ed.trimEdit(TimelineOps::TrimKind::Ripple, game, TimelineOps::Edge::End), -20);
    }, [&] {
        synced("ripple trim");
        CHECK_EQ(TimelineOps::findClip(p.timeline(), after)->start, 60);
    });
    // Split: each part keeps its own copy, linked to that part
    step("split", [&] { ed.bladeAt(game, 45); }, [&] {
        const auto k = copies();
        if (CHECK_EQ(k.size(), 2)) {
            CHECK(inSync(k[0].second, base()));
            const Clip* right = nullptr;
            for (const Clip& c : p.timeline().video[0].clips)
                if (c.start == 45) right = &c;
            if (CHECK(right)) {
                CHECK(inSync(k[1].second, *right));
                CHECK(k[1].second.linkId != 0 && k[1].second.linkId == right->linkId && right->linkId != base().linkId);
            }
        }
    });
    step("split at playhead", [&] {
        sel.set({game, sound});
        ed.splitAtPlayhead(45);
    }, [&] { CHECK_EQ(copies().size(), 2); });
    // Delete, with and without ripple; also with linked selection off (the copy is no sound partner)
    step("delete", [&] { sel.set({game, sound}); ed.deleteSelection(); }, [&] { CHECK(copies().isEmpty()); });
    step("ripple delete", [&] { sel.set({game, sound}); ed.rippleDeleteSelection(); }, [&] {
        CHECK(copies().isEmpty());
        CHECK_EQ(TimelineOps::findClip(p.timeline(), after)->start, 20);
    });
    ed.setLinkedSelection(false);
    step("delete alone", [&] { sel.set({game}); ed.deleteSelection(); }, [&] {
        CHECK(copies().isEmpty());
        CHECK(TimelineOps::findClip(p.timeline(), sound)); // the sound stays (linked selection off)
    });
    ed.setLinkedSelection(true);
    // Speed, reverse, freeze, ramp, in-between pictures
    step("speed", [&] { ed.setClipSpeed({game}, {0.5}, true); }, [&] {
        synced("speed");
        CHECK_EQ(base().speed, 0.5);
    });
    step("reverse", [&] { ed.setClipSpeed({game}, {1.0, true}, true); }, [&] { synced("reverse"); });
    step("freeze", [&] { ed.setClipSpeed({game}, {1.0, false, true}, true); }, [&] {
        synced("freeze");
        CHECK(base().freeze);
    });
    step("ramp", [&] { ed.addSpeedPoint(game, 40); }, [&] {
        synced("ramp");
        CHECK_EQ(base().ramp.size(), 1);
    });
    step("interpolate", [&] { ed.setFrameInterpolation({game}, FrameInterp::Blend); }, [&] {
        synced("interpolate");
        CHECK(base().interpolate == FrameInterp::Blend);
    });
    step("disable", [&] { sel.set({game, sound}); ed.toggleSelectionEnabled(); }, [&] { synced("disable"); });
    // The copy by hand: moved alone in time, placed and zoomed alone
    step("copy alone", [&] { ed.moveClips(ed.withLinked({copy}), 5, TrackKind::Video, 0); }, [&] {
        CHECK_EQ(base().start, 20);
        CHECK_EQ(copies().value(0).second.start, 25);
    });
    step("copy placed", [&] { ed.modifyClips({copy}, "place", [](Clip& c) { c.transform.posY -= 50; }); }, [&] {
        CHECK_EQ(base().transform.posY, 0.0);
    });
    // The clip's own transform (Inspector on the clip's selection) leaves the copy's place alone
    const ClipTransform placed = copies()[0].second.transform;
    step("clip placed", [&] {
        sel.set(QSet<int>(group.begin(), group.end()));
        ed.modifyClips(ed.editable(sel.ids().values().toVector()), "zoom", [](Clip& c) { c.transform.zoomX = 3; });
    }, [&] { CHECK(copies().value(0).second.transform == placed); });
    // Project round trip: still linked, still following
    const QString path = dir + "/follow.schneidi";
    QString error;
    CHECK(ProjectFile::save(p.data(), path, &error));
    ProjectData d;
    CHECK(ProjectFile::load(path, &d, &error));
    Project q;
    q.load(d);
    q.setCurrentSequence(seq);
    Selection qs;
    Editor qe(&q, &qs);
    const QVector<int> loaded = qe.withLinked({game});
    CHECK(!loaded.contains(copy));
    qe.moveClips(loaded, 7, TrackKind::Video, 0);
    const auto k = keptCopies(q.timeline());
    CHECK(k.size() == 1 && inSync(k[0].second, *TimelineOps::findClip(q.timeline(), game)));
    q.undoStack()->undo();
    CHECK_EQ(TimelineOps::findClip(q.timeline(), game)->start, 20);
    MediaAspect::setProbe({});
}

// The copy follows its picture, not the sound it shares the link with: the sound edited alone (Alt-click, linked
// selection off) leaves the copy in sync with the clip; unlinking or relinking the sound keeps the copy following
void testFollowNotSound()
{
    MediaAspect::setProbe([](const QString&) { return 16.0 / 9.0; });
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.setCurrentSequence(p.addTimeline("Short", 0, QSize(1080, 1920)));
    p.addMedia({"/x/cs2/a.mkv", "a.mkv", 400, true, true, false});
    int game = 0, sound = 0;
    p.edit("setup", [&](Timeline& tl) {
        Clip v;
        v.id = game = p.newClipId();
        v.mediaPath = "/x/cs2/a.mkv";
        v.start = 20;
        v.in = 100;
        v.out = 159;
        v.linkId = p.newLinkId();
        tl.video[0].clips << v;
        Clip a = v;
        a.id = sound = p.newClipId();
        tl.audio[0].clips << a;
    });
    const auto keeps = ed.reframeKeeps({game}, Reframe::Mode::Center, {}, {});
    ed.reframeClips({game}, Reframe::Mode::Center, {}, {}, &keeps);
    const auto base = [&] { return *TimelineOps::findClip(p.timeline(), game); };
    const auto copies = [&] { return keptCopies(p.timeline()); };
    if (!CHECK_EQ(copies().size(), 1)) return;
    const auto inPlace = [&](const char* what) {
        const auto k = copies();
        if (!CHECK_EQ(k.size(), 1) || !CHECK(inSync(k[0].second, base())))
            qWarning("  %s: the copy went with the sound", what);
    };
    ed.setLinkedSelection(false);
    CHECK(ed.followingCopies({sound}).isEmpty());
    ed.moveClips({sound}, 7, TrackKind::Audio, 0);
    inPlace("sound moved");
    p.undoStack()->undo();
    ed.trimClip(sound, TimelineOps::Edge::End, -10);
    inPlace("sound trimmed");
    p.undoStack()->undo();
    ed.bladeAt(sound, 40);
    inPlace("sound split");
    p.undoStack()->undo();
    sel.set({sound});
    ed.deleteSelection();
    inPlace("sound deleted");
    p.undoStack()->undo();
    ed.setClipSpeed({sound}, {0.5}, false);
    inPlace("sound retimed");
    p.undoStack()->undo();
    ed.setLinkedSelection(true);
    // Unlinked from its sound: the copy still follows the clip, not the sound
    sel.set({game, sound});
    ed.toggleLinkSelection();
    CHECK_EQ(TimelineOps::findClip(p.timeline(), sound)->linkId, 0);
    CHECK(!ed.withLinked({game}).contains(sound));
    ed.moveClips({game}, 5, TrackKind::Video, 0);
    inPlace("unlinked, clip moved");
    CHECK_EQ(TimelineOps::findClip(p.timeline(), sound)->start, 20);
    p.undoStack()->undo();
    // Linked again: the copy follows the clip and is still no partner
    ed.toggleLinkSelection();
    CHECK(ed.withLinked({game}).contains(sound));
    ed.moveClips(ed.withLinked({game}), 5, TrackKind::Video, 0);
    inPlace("relinked, clip moved");
    CHECK(!ed.withLinked({game}).contains(copies()[0].second.id));
    p.undoStack()->undo();
    p.undoStack()->undo();
    p.undoStack()->undo();
    CHECK_EQ(TimelineOps::findClip(p.timeline(), sound)->linkId, base().linkId);
    // Cut and paste elsewhere: the copy goes with the clip (as with delete) and comes back linked to the pasted clip;
    // Copy and paste gives the pasted clip a copy of its own
    sel.set({game, sound});
    ed.cutSelection();
    CHECK(copies().isEmpty());
    ed.paste(200);
    const auto pastedPair = [&](const char* what) {
        const Clip* pasted = nullptr;
        for (const Clip& c : p.timeline().video[0].clips)
            if (c.start == 200) pasted = &c;
        const auto k = copies();
        bool ok = pasted && !k.isEmpty() && k.last().second.start == 200 && inSync(k.last().second, *pasted)
                  && k.last().second.linkId == pasted->linkId && pasted->linkId != 0;
        if (!CHECK(ok)) qWarning("  %s: no kill feed copy with the pasted clip", what);
        return ok ? k.last().second.id : 0;
    };
    pastedPair("cut, paste");
    p.undoStack()->undo();
    p.undoStack()->undo();
    CHECK_EQ(copies().size(), 1);
    sel.set({game, sound});
    ed.copySelection();
    CHECK_EQ(ed.pasteAttributesSource(), p.clipName(base())); // the clip is still the attributes' source
    ed.paste(200);
    if (const int pastedCopy = pastedPair("copy, paste")) {
        CHECK_EQ(copies().size(), 2);
        CHECK(pastedCopy != copies()[0].second.id);
    }
    p.undoStack()->undo();
    MediaAspect::setProbe({});
}

// Reframing a whole timeline leaves its overlays (a small webcam picture on V2) where they are; asked for by id, one
// is reframed
void testOverlays()
{
    MediaAspect::setProbe([](const QString&) { return 16.0 / 9.0; });
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    const QSizeF frame(1080, 1920);
    p.setCurrentSequence(p.addTimeline("Short", 0, frame.toSize()));
    p.addMedia({"/x/cs2/a.mkv", "a.mkv", 100, true, false, false});
    p.addMedia({"/x/cam.mkv", "cam.mkv", 100, true, false, false});
    int game = 0, cam = 0, cropped = 0, full = 0;
    p.edit("setup", [&](Timeline& tl) {
        Clip v;
        v.id = game = p.newClipId();
        v.mediaPath = "/x/cs2/a.mkv";
        v.out = 49;
        tl.video[0].clips << v;
        TimelineOps::ensureTracks(tl, TrackKind::Video, 3);
        Clip c; // webcam in a corner
        c.id = cam = p.newClipId();
        c.mediaPath = "/x/cam.mkv";
        c.out = 49;
        c.transform.zoomX = c.transform.zoomY = 0.3;
        c.transform.posX = -350;
        c.transform.posY = -700;
        tl.video[1].clips << c;
        Clip r = c; // the webcam cut out of a full picture by its crop
        r.id = cropped = p.newClipId();
        r.start = 50;
        r.transform = {};
        r.transform.cropLeft = 700;
        r.transform.cropTop = 300;
        tl.video[1].clips << r;
        Clip f = c; // a picture of its own on V3 (B-roll at full size): reframed
        f.id = full = p.newClipId();
        f.transform = {};
        tl.video[2].clips << f;
    });
    CHECK(Reframe::isOverlay(*TimelineOps::findClip(p.timeline(), cam), 1, 16.0 / 9, frame));
    CHECK(Reframe::isOverlay(*TimelineOps::findClip(p.timeline(), cropped), 1, 16.0 / 9, frame));
    CHECK(!Reframe::isOverlay(*TimelineOps::findClip(p.timeline(), cam), 0, 16.0 / 9, frame)); // V1 is the picture
    CHECK(!Reframe::isOverlay(*TimelineOps::findClip(p.timeline(), full), 2, 16.0 / 9, frame));
    const ClipTransform camWas = TimelineOps::findClip(p.timeline(), cam)->transform;
    const ClipTransform croppedWas = TimelineOps::findClip(p.timeline(), cropped)->transform;
    CHECK_EQ(ed.reframeClips({}, Reframe::Mode::Center), 2);
    CHECK(TimelineOps::findClip(p.timeline(), cam)->transform == camWas);
    CHECK(TimelineOps::findClip(p.timeline(), cropped)->transform == croppedWas);
    CHECK(TimelineOps::findClip(p.timeline(), game)->transform.zoomX > 1.5);
    CHECK(TimelineOps::findClip(p.timeline(), full)->transform.zoomX > 1.5);
    // Asked for by id: reframed
    CHECK_EQ(ed.reframeClips({cam}, Reframe::Mode::Center), 1);
    CHECK(TimelineOps::findClip(p.timeline(), cam)->transform.zoomX > 1.5);
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

// CLI: a 9:16 copy of an edit with a webcam overlay leaves the webcam in its corner (noted), clips:[…] reframes it;
// the kill feed copy follows its clip through move, speed and delete ops
void testCliFollow(const QString& dir, const QString& game, const QString& plain)
{
    auto run = [&](const QString& name, const QJsonObject& args) -> const QJsonObject {
        const Cli::Command* cmd = Cli::find(name);
        Cli::Context ctx;
        return cmd->run(Cli::normalize(*cmd, args), ctx);
    };
    const QString path = dir + "/overlay-cli.schneidi";
    int source = 0;
    {
        Project p;
        p.addMedia({game, "clip.mkv", 150, true, true, false});
        p.addMedia({plain, "plain.mkv", 150, true, true, false});
        p.edit("setup", [&](Timeline& tl) {
            Clip v;
            v.id = p.newClipId();
            v.mediaPath = game;
            v.out = 99;
            tl.video[0].clips << v;
            TimelineOps::ensureTracks(tl, TrackKind::Video, 2);
            Clip w;
            w.id = p.newClipId();
            w.mediaPath = plain;
            w.out = 99;
            w.transform.zoomX = w.transform.zoomY = 0.3;
            w.transform.posX = 600;
            w.transform.posY = -300;
            tl.video[1].clips << w;
        });
        source = p.currentSequence();
        QString error;
        CHECK(ProjectFile::save(p.data(), path, &error));
    }
    const QJsonObject made = run("edit", {{"project", path}, {"ops", QString(R"([{"op":"new_timeline","from":%1,
        "name":"Short","aspect":"9:16","reframe":"center"}])").arg(source)}});
    bool noted = false;
    for (const QJsonValue& n : made["notes"].toArray()) noted |= n.toString().startsWith("overlays not reframed");
    CHECK(noted);
    const auto clips = [&] {
        QVector<QJsonObject> out;
        for (const QJsonValue& t : run("info", {{"project", path}, {"timeline", "Short"}})["timeline"]["tracks"].toArray())
            for (const QJsonValue& c : t["clips"].toArray()) out << c.toObject();
        return out;
    };
    const auto find = [&](const std::function<bool(const QJsonObject&)>& f) {
        for (const QJsonObject& c : clips())
            if (f(c)) return c;
        return QJsonObject{};
    };
    const auto camOf = [&] { return find([&](const QJsonObject& c) { return c["media"].toString() == plain; }); };
    const auto gameOf = [&] {
        return find([&](const QJsonObject& c) { return c["media"].toString() == game && c["track"] == "V1"; });
    };
    const auto copyOf = [&] { return find([&](const QJsonObject& c) { return c["keep_region"].toString() == "killfeed"; }); };
    QJsonObject cam = camOf();
    CHECK(near(cam["transform"].toObject()["zoom_x"].toDouble(), 0.3, 1e-9));
    CHECK(near(cam["transform"].toObject()["x"].toDouble(), 600.0 * 1080 / 1920, 1e-6)); // same place in the frame
    CHECK(near(cam["transform"].toObject()["y"].toDouble(), -300.0 * 1920 / 1080, 1e-6));
    CHECK(gameOf()["transform"].toObject()["zoom_x"].toDouble() > 1.5);
    if (!CHECK(!copyOf().isEmpty())) return;
    CHECK_EQ(copyOf()["link"].toInt(), gameOf()["link"].toInt());
    // Named: reframed
    run("edit", {{"project", path}, {"ops", QString(R"([{"op":"timeline","timeline":"Short"},
        {"op":"reframe","mode":"center","clips":[%1]}])").arg(cam["id"].toInt())}});
    CHECK(camOf()["transform"].toObject()["zoom_x"].toDouble() > 1.5);
    // The copy follows
    const int id = gameOf()["id"].toInt();
    const auto edit = [&](const QString& op) {
        run("edit", {{"project", path}, {"ops", QString(R"([{"op":"timeline","timeline":"Short"},%1])").arg(op)}});
    };
    edit(QString(R"({"op":"move","clips":[%1],"by":10})").arg(id));
    CHECK_EQ(gameOf()["start"].toInt(), 10);
    CHECK_EQ(copyOf()["start"].toInt(), 10);
    edit(QString(R"({"op":"speed","clips":[%1],"speed":0.5})").arg(id));
    CHECK_EQ(copyOf()["speed"].toDouble(), 0.5);
    CHECK_EQ(copyOf()["end"].toInt(), gameOf()["end"].toInt());
    edit(QString(R"({"op":"trim","clip":%1,"edge":"end","by":-20})").arg(id));
    CHECK_EQ(copyOf()["end"].toInt(), gameOf()["end"].toInt());
    // Unlinked (from its sound, if any): the copy still follows
    edit(QString(R"({"op":"unlink","clips":[%1]})").arg(id));
    edit(QString(R"({"op":"move","clips":[%1],"by":5})").arg(id));
    CHECK_EQ(gameOf()["start"].toInt(), 15);
    CHECK_EQ(copyOf()["start"].toInt(), 15);
    edit(QString(R"({"op":"split","clip":%1,"at":40})").arg(id));
    int feeds = 0;
    for (const QJsonObject& c : clips()) feeds += c["keep_region"].toString() == "killfeed";
    CHECK_EQ(feeds, 2);
    edit(QString(R"({"op":"delete","clips":[%1]})").arg(id));
    feeds = 0;
    for (const QJsonObject& c : clips()) feeds += c["keep_region"].toString() == "killfeed";
    CHECK_EQ(feeds, 1); // the right part's copy stays with the right part
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
    // A webcam-like overlay of the same recording in a corner on V2: not reframed by the 9:16 copy
    p->edit("overlay", [&](Timeline& tl) {
        TimelineOps::ensureTracks(tl, TrackKind::Video, 2);
        Clip c = tl.video[0].clips[0];
        c.id = p->newClipId();
        c.linkId = 0;
        c.transform.zoomX = c.transform.zoomY = 0.25;
        c.transform.posX = -600;
        c.transform.posY = -350;
        tl.video[1].clips << c;
    });
    const int overlay = p->timeline().video[1].clips[0].id;
    const double fromWidth = p->timelineFormat().width;
    const int steps = p->undoStack()->index();
    emit pool->reframedCopyRequested(p->currentSequence());
    CHECK_EQ(p->undoStack()->index(), steps + 1); // copy, reframe and kill feed: one step
    CHECK_EQ(p->timelineFormat().size(), QSize(1080, 1920));
    CHECK(keptCopy(p->timeline()));
    if (CHECK_EQ(p->timeline().video.value(1).clips.size(), 1)) { // the copy's clip ids are new: the one on V2
        const Clip& o = p->timeline().video[1].clips[0];
        CHECK(o.id != overlay && o.keepRegion.isEmpty());
        CHECK(near(o.transform.zoomX, 0.25, 1e-9));
        CHECK(near(o.transform.posX, -600.0 * 1080 / fromWidth, 1e-6)); // same place in the frame
        CHECK(!o.keys.contains(AnimParam::PosX));
    }
    CHECK(w.statusBar()->currentMessage().contains("Overlay"));
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
        CHECK_EQ(videoClips(p->timeline()), 3);
        CHECK(near(p->timeline().video[1].clips.value(0).transform.zoomX, 0.25, 1e-9)); // the overlay again
        CHECK(w.statusBar()->currentMessage().contains("Overlay"));
    }
    // A speed point on a selection of the clip and its copy (rubber band): the clip and its copy both get it (the copy
    // first in the selection took the link's turn: only the copy was ramped, its kill feed out of sync)
    auto* sel = w.findChild<Selection*>();
    auto* tv = w.findChild<TimelineView*>();
    QAction* point = InputBindings::instance().action("speed_point_add");
    if (const Clip* k = keptCopy(p->timeline()); CHECK(sel && tv && point && k)) {
        const int base = p->timeline().video[0].clips.value(0).id, copy = k->id;
        QSet<int> all{base, copy};
        // Both orders of the two in the selection (QSet's order depends on the hash seed and its size)
        for (int round = 0; round < 16; ++round) {
            sel->set(all);
            tv->setPlayhead(40);
            point->trigger();
            const Clip* b = TimelineOps::findClip(p->timeline(), base);
            const Clip* c = TimelineOps::findClip(p->timeline(), copy);
            if (!CHECK(b && c && b->ramp.size() == 1 && c->ramp == b->ramp)) break;
            p->undoStack()->undo();
            all.insert(100000 + round); // another order next round
        }
    }
    QSettings().remove("reframe/keepKillFeed");
}

} // namespace

// The dark frame behind a kept region is made only for the part that can show. Zoomed far in it was a picture of
// the region's whole zoomed size: a facecam-sized region at zoom 80 took many times as long as the same clip without
// the frame (hundreds of MB per frame and thread; zoom 100 took seconds per frame)
void testZoomedFrame(const QString& plain)
{
    const ProjectFormat fmt{1080, 1920, {25, 1}};
    // crop: left, right, top, bottom in timeline pixels of the picture fitted at zoom 1 (1080 x 607.5)
    const auto timeline = [&](std::array<double, 4> crop, double zoom, double posX, double posY, bool kept) {
        Timeline tl;
        tl.video.resize(2);
        Clip base;
        base.id = 1;
        base.mediaPath = plain;
        base.out = 24;
        tl.video[0].clips << base;
        Clip keep = base;
        keep.id = 2;
        if (kept) keep.keepRegion = "killfeed";
        keep.transform.cropLeft = crop[0];
        keep.transform.cropRight = crop[1];
        keep.transform.cropTop = crop[2];
        keep.transform.cropBottom = crop[3];
        keep.transform.zoomX = keep.transform.zoomY = zoom;
        keep.transform.posX = posX;
        keep.transform.posY = posY;
        tl.video[1].clips << keep;
        return tl;
    };
    // Still framed where it shows: zoom 2, the region (600 x 95 px) moved to x 240..840, y 852..948 over the clip
    const QImage framed = Check::renderFrame(timeline({760, 20, 40, 520}, 2, -740, -420, true), fmt, 0);
    if (!CHECK(!framed.isNull())) return;
    const int ring = qGray(framed.pixel(540, 849)), beside = qGray(framed.pixel(540, 800));
    if (!CHECK(ring + 20 < beside)) std::printf("       frame %d, picture beside it %d\n", ring, beside);
    // Zoomed far in (the region's middle in the middle of the frame): about as fast as without the frame
    const auto timed = [&](bool kept) {
        QElapsedTimer t;
        t.start();
        CHECK(!Check::renderFrame(timeline({540, 20, 40, 267.5}, 80, -20800, -9100, kept), fmt, 0).isNull());
        return t.elapsed();
    };
    timed(false); // decoder warm
    const qint64 without = timed(false), with = timed(true);
    std::printf("zoom 80: %lld ms framed, %lld ms without\n", with, without);
    CHECK(with < 3 * without + 200);
}

// Another clip overwriting the reframed clip (dropped onto V1): the copy above is cut like its clip - the part over
// the new clip goes (it showed the old footage's kill feed there), each piece linked to its clip's piece; one undo
// step. Overwriting an end or the whole clip likewise; a copy on a locked track stays.
void testOverwriteCopy()
{
    MediaAspect::setProbe([](const QString&) { return 16.0 / 9.0; });
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    const int seq = p.addTimeline("Short", 0, QSize(1080, 1920));
    p.setCurrentSequence(seq);
    p.addMedia({"/x/cs2/a.mkv", "a.mkv", 400, true, true, false});
    p.addMedia({"/x/other.mkv", "other.mkv", 10, true, false, false});
    int game = 0;
    p.edit("setup", [&](Timeline& tl) {
        Clip v;
        v.id = game = p.newClipId();
        v.mediaPath = "/x/cs2/a.mkv";
        v.start = 20;
        v.in = 100;
        v.out = 159; // 20..80
        v.linkId = p.newLinkId();
        tl.video[0].clips << v;
        Clip a = v;
        a.id = p.newClipId();
        tl.audio[0].clips << a;
    });
    const auto keeps = ed.reframeKeeps({game}, Reframe::Mode::Center, {}, {});
    ed.reframeClips({game}, Reframe::Mode::Center, {}, {}, &keeps);
    if (!CHECK_EQ(keptCopies(p.timeline()).size(), 1)) return;
    const int copyTrack = keptCopies(p.timeline())[0].first;
    const int copyId = keptCopies(p.timeline())[0].second.id;
    // Each copy lies exactly on a piece of the clip below it, linked to it
    const auto matched = [&](int pieces) {
        const auto k = keptCopies(p.timeline());
        if (!CHECK_EQ(k.size(), pieces)) return;
        for (const auto& [track, c] : k) {
            CHECK_EQ(track, copyTrack);
            const Clip* b = nullptr;
            for (const Clip& x : p.timeline().video[0].clips)
                if (x.mediaPath == c.mediaPath && x.start == c.start) b = &x;
            if (!CHECK(b)) continue;
            CHECK(inSync(c, *b));
            CHECK(c.linkId != 0 && c.linkId == b->linkId);
        }
    };
    const auto step = [&](const char* what, int frame, int pieces) {
        const Timeline was = p.timeline();
        const int index = p.undoStack()->index();
        ed.addMediaAt({"/x/other.mkv"}, frame, 0);
        if (!CHECK_EQ(p.undoStack()->index(), index + 1)) qWarning("  %s: not one undo step", what);
        matched(pieces);
        for (const auto& k : keptCopies(p.timeline())) // nothing over the new clip
            CHECK(k.second.end() <= frame || k.second.start >= frame + 10);
        p.undoStack()->undo();
        if (!CHECK(p.timeline() == was)) qWarning("  %s: undo left another timeline", what);
    };
    step("middle", 40, 2);
    step("start", 15, 1);
    step("end", 75, 1);
    p.edit("short", [&](Timeline& tl) { // both 20..28
        tl.video[0].clips[0].out = tl.video[0].clips[0].in + 7;
        tl.video[copyTrack].clips[0].out = tl.video[copyTrack].clips[0].in + 7;
    });
    step("whole", 19, 0);
    p.undoStack()->undo();
    // A locked copy track: the lock holds
    p.edit("lock", [&](Timeline& tl) { tl.video[copyTrack].locked = true; });
    ed.addMediaAt({"/x/other.mkv"}, 40, 0);
    if (CHECK_EQ(keptCopies(p.timeline()).size(), 1)) CHECK_EQ(keptCopies(p.timeline())[0].second.id, copyId);
}

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
    testFollow(tmp.path());
    testFollowNotSound();
    testOverwriteCopy();
    testOverlays();
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
    testZoomedFrame(plain);
    testCli(tmp.path(), game, plain);
    testCliFollow(tmp.path(), game, plain);
    testApp(game);
    return Check::result();
}
