// SPDX-License-Identifier: AGPL-3.0-only
// The kept kill feed follows its lines: finding the lines of a game's kill feed in a picture (engine/KillFeedLines:
// boxes over a busy, bright or dark picture, text over black, an edge of the game picture that is no line, a smaller
// recording), the stretches and keyframes of the copy from a line timeline (Reframe::feedStates, keepCopy: hidden
// without lines, short gaps and blips ignored, as tall as its lines and as wide as the widest, text not blown up),
// and a short of a synthetic CS2 recording through the CLI (new_timeline with reframe): hidden before the first line,
// its crop fitting one and then three lines, the rendered frame showing the lines only then; a recording without any
// line gets no copy. Review fixes: a blip-only feed is none, a long clip without lines keeps the whole region, smoothing
// in file time for sped up and slowed clips, a freeze frame's nearest sample, compositing off, the base's opacity keys,
// a picture shape changed after the reframe, a measurement of a broken-off file not cached.
#include "render_check.h"
#include "timeline_guard.h"

#include "cli/Commands.h"
#include "core/GameHud.h"
#include "core/Keyframes.h"
#include "core/MediaAspect.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Reframe.h"
#include "engine/KillFeedLines.h"
#include "engine/Snapshot.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QPainter>
#include <QTemporaryDir>

#include <clocale>
#include <cmath>

namespace {

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

const GameHud::Game& cs2() { return *GameHud::find("cs2"); }

// A game picture: a smooth gradient with some texture, or flat
QImage background(QColor top, QColor bottom, bool texture = true, QSize size = {1920, 1080})
{
    QImage img(size, QImage::Format_RGB32);
    QPainter p(&img);
    QLinearGradient g(0, 0, size.width() * 0.3, size.height());
    g.setColorAt(0, top);
    g.setColorAt(1, bottom);
    p.fillRect(img.rect(), g);
    if (texture)
        for (int y = 0; y < size.height(); y += 7)
            for (int x = (y * 13) % 11; x < size.width(); x += 11)
                p.fillRect(x, y, 3, 2, QColor(0, 0, 0, 40 + (x * 7 + y * 3) % 50));
    return img;
}

// A kill feed line as CS2 draws it at 1080p: a box `width` wide ending at x 1909 from row `top`, 31 rows tall, with
// text (white bars) inside; box = false draws the text only (a black picture hides the box)
void line(QImage& img, int top, int width, bool box = true, QColor fill = QColor(0, 0, 0, 140))
{
    QPainter p(&img);
    const double s = img.height() / 1080.0;
    const QRectF r((1909 - width) * s, top * s, width * s, 31 * s);
    if (box) p.fillRect(r, fill);
    for (double x = r.left() + 10 * s; x < r.right() - 10 * s; x += 9 * s)
        p.fillRect(QRectF(x, r.top() + 10 * s, 5 * s, 12 * s), QColor(235, 235, 235));
}

void testDetect()
{
    const GameHud::Game& g = cs2();
    CHECK(KillFeedLines::canMeasure(g));
    CHECK(!KillFeedLines::canMeasure(*GameHud::find("valorant"))); // line geometry unknown: the region stays fixed
    // No line
    QImage img = background(QColor(200, 170, 120), QColor(120, 90, 60));
    CHECK_EQ(KillFeedLines::detect(img, g).lines, 0);
    // One line, then three (the second the widest); left edge and bottom as drawn
    line(img, 75, 250);
    KillFeedLines::Sample s = KillFeedLines::detect(img, g);
    CHECK_EQ(s.lines, 1);
    CHECK(near(s.left * 1920, 1659, 3));
    CHECK(near(s.bottom * 1080, 106, 2));
    line(img, 109, 380);
    line(img, 142, 200);
    s = KillFeedLines::detect(img, g);
    CHECK_EQ(s.lines, 3);
    CHECK(near(s.left * 1920, 1529, 3));
    CHECK(near(s.bottom * 1080, 173, 2));
    // A bright sky behind (the box darkens it), and the other HUD layout (first line at 95)
    img = background(QColor(230, 240, 250), QColor(200, 220, 240), false);
    line(img, 95, 300);
    s = KillFeedLines::detect(img, g);
    CHECK_EQ(s.lines, 1);
    CHECK(near(s.left * 1920, 1609, 3));
    // The player's own death: a red box
    img = background(QColor(90, 90, 90), QColor(60, 60, 60));
    line(img, 75, 260, true, QColor(150, 10, 10, 200));
    CHECK_EQ(KillFeedLines::detect(img, g).lines, 1);
    // A black picture (death screen): no box shows, the text is the line
    img = background(Qt::black, Qt::black, false);
    line(img, 75, 320, false);
    line(img, 109, 240, true, QColor(120, 10, 10, 220));
    s = KillFeedLines::detect(img, g);
    CHECK_EQ(s.lines, 2);
    CHECK(near(s.left * 1920, 1909 - 320 + 10, 4)); // its text's left edge
    // An edge of the game picture across the rows of a line (a roof against the sky) is no line: it runs on past
    // the box's right edge; nor is a dark band of the picture there
    img = background(QColor(220, 230, 240), QColor(220, 230, 240), false);
    {
        QPainter p(&img);
        p.fillRect(0, 75, 1920, 400, QColor(110, 90, 70));
        p.fillRect(0, 75, 1920, 31, QColor(60, 50, 40));
    }
    CHECK_EQ(KillFeedLines::detect(img, g).lines, 0);
    // A 720p recording: the same lines at its scale
    img = background(QColor(200, 170, 120), QColor(120, 90, 60), true, QSize(1280, 720));
    line(img, 75, 250);
    line(img, 109, 300);
    s = KillFeedLines::detect(img, g);
    CHECK_EQ(s.lines, 2);
    CHECK(near(s.left * 1920, 1609, 6));
}

// A line timeline (10 samples a second at 30 fps): none, one line (missing in one sample: a flash), three lines,
// none with a single sample of a line (a sign in the picture); one sample reads a line too wide
Reframe::Feed timeline()
{
    Reframe::Feed f;
    f.measured = true;
    f.fps = 30;
    f.lineHeight = 31.0 / 1080;
    for (int frame = 0; frame < 300; frame += 3) {
        Reframe::FeedSample s;
        s.frame = frame;
        if (frame >= 60 && frame < 120 && frame != 90) s.lines = 1, s.left = 1659 / 1920.0, s.bottom = 106 / 1080.0;
        if (frame >= 120 && frame < 240) s.lines = 3, s.left = 1529 / 1920.0, s.bottom = 173 / 1080.0;
        if (frame == 180) s.left = 1400 / 1920.0; // one sample took game picture left of a line for its box
        if (frame == 270) s.lines = 1, s.left = 1500 / 1920.0, s.bottom = 106 / 1080.0;
        f.samples << s;
    }
    return f;
}

void testStates()
{
    const Reframe::Feed f = timeline();
    const QVector<Reframe::FeedState> st = Reframe::feedStates(f, 300);
    if (CHECK_EQ(st.size(), 4)) {
        CHECK_EQ(st[0].lines, 0);
        CHECK_EQ(st[0].from, 0);
        CHECK(near(st[0].to, 59, 2));
        CHECK_EQ(st[1].lines, 1); // the flash at 90 did not split it
        CHECK(near(st[1].left * 1920, 1659, 1e-6));
        CHECK(near(st[2].from, 119, 2));
        CHECK_EQ(st[2].lines, 3);
        CHECK(near(st[2].bottom * 1080, 173, 1e-6));
        CHECK(near(st[2].left * 1920, 1529, 1e-6)); // not the one wide sample
        CHECK_EQ(st[3].lines, 0); // the blip at 270 is no line
        CHECK_EQ(st[3].to, 300);
    }
    CHECK(Reframe::feedStates(Reframe::Feed{}, 300).isEmpty()); // not measured
    Reframe::Feed none = f;
    for (Reframe::FeedSample& s : none.samples) s.lines = 0;
    CHECK(Reframe::feedUse(none) == Reframe::FeedUse::None);
    CHECK(Reframe::feedUse(f) == Reframe::FeedUse::Follow);
    CHECK(Reframe::feedUse(Reframe::Feed{}) == Reframe::FeedUse::Whole); // not measured
    // A single sample of a line (a blip) is no line either
    Reframe::Feed blip = none;
    blip.samples[50].lines = 1, blip.samples[50].left = 0.8, blip.samples[50].bottom = 0.1;
    CHECK(Reframe::feedUse(blip) == Reframe::FeedUse::None);
    CHECK(Reframe::feedLineless(blip));
    CHECK(!Reframe::feedLineless(f));
    CHECK(!Reframe::feedLineless(Reframe::Feed{}));
}

// Samples of a file as KillFeedLines::measure gives them: 10 a second over `seconds`, one line from 1 s to 4 s
// except one sample (a flash), and one sample with a line at 7.5 s (a sign in the picture)
KillFeedLines::Samples fileSamples(double seconds)
{
    KillFeedLines::Samples out;
    for (int i = 0; i < int(seconds * 10); ++i) {
        KillFeedLines::Sample s;
        s.seconds = i / 10.0;
        if ((i >= 10 && i < 40 && i != 25) || i == 75) s.lines = 1, s.left = 0.86, s.bottom = 0.1;
        out << s;
    }
    return out;
}

// A sped up or slowed clip: the flash and the blip are moments of the file, whether the clip shows them shorter or
// longer (a slowed clip has its samples further apart than the moments: one sample each)
void testSpeed()
{
    const FrameRate rate{30, 1};
    const int fileLength = 10 * 30;
    for (double speed : {4.0, 1.0, 0.25}) {
        Clip c;
        c.mediaPath = "/x/cs2/a.mkv";
        c.speed = speed;
        c.in = 0;
        c.out = int(std::floor(fileLength / speed)) - 1;
        const Reframe::Feed f = KillFeedLines::feedOf(c, rate, fileLength, fileSamples(10), QRectF(0, 0, 1, 1), cs2());
        CHECK(!f.samples.isEmpty() && f.samples.first().file >= 0);
        const QVector<Reframe::FeedState> st = Reframe::feedStates(f, c.length());
        if (!CHECK_EQ(st.size(), 3)) {
            for (const auto& x : st) std::printf("speed %.2f: %d..%d lines %d\n", speed, x.from, x.to, x.lines);
            continue;
        }
        CHECK_EQ(st[0].lines, 0);
        CHECK_EQ(st[1].lines, 1); // not split by the flash
        CHECK(near(st[1].from, 0.95 * 30 / speed, 0.06 * 30 / speed + 1));
        CHECK(near(st[1].to, 3.95 * 30 / speed, 0.06 * 30 / speed + 1));
        CHECK_EQ(st[2].lines, 0); // the blip at 7.5 s is none
        CHECK_EQ(st[2].to, c.length());
    }
    // A freeze frame: the sample nearest its picture (a line at 2 s), not the last one
    Clip still;
    still.mediaPath = "/x/cs2/a.mkv";
    still.in = 60;
    still.out = 89;
    still.freeze = true;
    const Reframe::Feed f = KillFeedLines::feedOf(still, rate, fileLength, fileSamples(10), QRectF(0, 0, 1, 1), cs2());
    if (CHECK_EQ(f.samples.size(), 1)) {
        CHECK_EQ(f.samples[0].lines, 1);
        CHECK(near(f.samples[0].file, 2, 1e-9));
    }
}

// A measured clip without any line: a short one gets no copy, one of 20 s or more keeps the whole region (the game's
// HUD may look other than the analysis knows)
void testFallback()
{
    const auto empty = [](double seconds, bool fileTimes) {
        Reframe::Feed f;
        f.measured = true;
        f.fps = 30;
        for (int i = 0; i < int(seconds * 10); ++i) {
            Reframe::FeedSample s;
            s.frame = i * 3;
            if (fileTimes) s.file = 100 + i / 10.0;
            f.samples << s;
        }
        return f;
    };
    CHECK(Reframe::feedUse(empty(10, true)) == Reframe::FeedUse::None);
    CHECK(Reframe::feedUse(empty(19, true)) == Reframe::FeedUse::None);
    CHECK(Reframe::feedUse(empty(20, true)) == Reframe::FeedUse::Whole);
    CHECK(Reframe::feedUse(empty(25, false)) == Reframe::FeedUse::Whole);
    CHECK(Reframe::feedLineless(empty(25, true)));
    // The copy of the long one: the whole region, no keys, shown as the base is
    Clip base;
    base.mediaPath = "/x/cs2/a.mkv";
    base.in = 0;
    base.out = 25 * 30 - 1;
    const QSizeF frame(1080, 1920);
    const Clip whole = Reframe::keepCopy(base, {"killfeed", cs2().feed, empty(25, true)}, 16.0 / 9, frame);
    const Clip fixed = Reframe::keepCopy(base, {"killfeed", cs2().feed, {}}, 16.0 / 9, frame);
    CHECK(whole.keys.isEmpty());
    CHECK(whole.transform == fixed.transform);
    // The short one, if made anyway: hidden (with compositing, else its opacity would not apply)
    base.out = 10 * 30 - 1;
    const Clip hidden = Reframe::keepCopy(base, {"killfeed", cs2().feed, empty(10, true)}, 16.0 / 9, frame);
    CHECK(!hidden.keys.contains(AnimParam::Opacity));
    CHECK(near(hidden.transform.opacity, 0, 1e-9));
    CHECK(hidden.transform.compositeOn);
}

void testKeys()
{
    Clip base;
    base.mediaPath = "/x/cs2/a.mkv";
    base.in = 100;
    base.out = 399; // 300 frames
    const QSizeF frame(1080, 1920);
    const double aspect = 16.0 / 9.0;
    const QSizeF fitted = MediaAspect::fit(aspect, frame); // 1080 x 607.5
    const QRectF region = cs2().feed;
    // Not measured: one fixed region, no keys (as before)
    const Clip fixed = Reframe::keepCopy(base, {"killfeed", region, {}}, aspect, frame);
    CHECK(fixed.keys.isEmpty());
    const Clip c = Reframe::keepCopy(base, {"killfeed", region, timeline()}, aspect, frame);
    const auto at = [&](AnimParam p, int t) { return Keys::valueAt(c, p, t); };
    // (the zoom, and with it the place of the top edge, stays when the text size limit sets it for both shapes)
    for (AnimParam p : {AnimParam::Opacity, AnimParam::CropLeft, AnimParam::CropBottom, AnimParam::PosX})
        CHECK(c.keys.contains(p));
    CHECK(!c.keys.contains(AnimParam::CropTop) && !c.keys.contains(AnimParam::CropRight)); // top and right stay
    CHECK(near(c.transform.cropTop, region.top() * fitted.height(), 1e-6));
    // Hidden before the first line and after the last (the blip does not show it), fading in just before the line
    CHECK(near(at(AnimParam::Opacity, 20), 0, 1e-9));
    CHECK(near(at(AnimParam::Opacity, 59), 100, 1e-9));
    CHECK(at(AnimParam::Opacity, 56) < 100 && at(AnimParam::Opacity, 56) >= 0);
    CHECK(near(at(AnimParam::Opacity, 90), 100, 1e-9));
    CHECK(near(at(AnimParam::Opacity, 180), 100, 1e-9));
    CHECK(near(at(AnimParam::Opacity, 270), 0, 1e-9));
    // One line: from its left edge (a little room) to the region's right edge, down to just below the line
    const double line = 31.0 / 1080 * fitted.height();
    const double left1 = at(AnimParam::CropLeft, 90), bottom1 = fitted.height() - at(AnimParam::CropBottom, 90);
    CHECK(left1 < 1659 / 1920.0 * fitted.width() && left1 > 1659 / 1920.0 * fitted.width() - line);
    CHECK(bottom1 > 106 / 1080.0 * fitted.height() && bottom1 < 106 / 1080.0 * fitted.height() + line / 2);
    // Three lines: wider and taller
    const double left3 = at(AnimParam::CropLeft, 180), bottom3 = fitted.height() - at(AnimParam::CropBottom, 180);
    CHECK(left3 < 1529 / 1920.0 * fitted.width() && left3 > 1529 / 1920.0 * fitted.width() - line);
    CHECK(near(bottom3 - bottom1, (173 - 106) / 1080.0 * fitted.height(), 1));
    // Growing is done when the lines show (frame 119), eased over 0.2 s before
    CHECK(near(at(AnimParam::CropLeft, 119), left3, 0.5));
    CHECK(at(AnimParam::CropLeft, 116) < left1 - 1 && at(AnimParam::CropLeft, 116) > left3 + 1);
    CHECK(near(at(AnimParam::CropLeft, 110), left1, 1e-6));
    // The text is bigger than in the fixed region, but a line at most maxLine of the frame height; the copy at most
    // the width and height a kept region may take; its top edge stays at 10 % of the frame
    const Reframe::KeepPlace place;
    for (int t : {90, 180}) {
        const double z = at(AnimParam::ZoomX, t);
        CHECK(z * line <= place.maxLine * frame.height() + 1e-6);
        CHECK(z > fixed.transform.zoomX);
        const double w = fitted.width() - at(AnimParam::CropLeft, t) - c.transform.cropRight;
        const double h = fitted.height() - c.transform.cropTop - at(AnimParam::CropBottom, t);
        CHECK(z * w <= place.width * frame.width() + 1e-6);
        CHECK(z * h <= place.maxHeight * frame.height() + 1e-6);
        // Top edge of the shown region in the frame (y down from the top): centre - posY - zoom * (region top - middle)
        const double topEdge = frame.height() / 2 - at(AnimParam::PosY, t)
                               + z * (c.transform.cropTop - fitted.height() / 2);
        CHECK(near(topEdge, place.top * frame.height(), 0.5));
        // Centred
        const double leftEdge = frame.width() / 2 + at(AnimParam::PosX, t) + z * (at(AnimParam::CropLeft, t) - fitted.width() / 2);
        CHECK(near(leftEdge + z * w / 2, frame.width() / 2, 0.5));
    }
    // The copy's static values are its first shown shape (a copy with its keys removed shows one line)
    CHECK(near(c.transform.cropLeft, left1, 1e-6));
}

// Compositing off on the base: a copy shown all along has no opacity track (the base's keys, which do not apply
// without compositing, are not carried), a hidden one turns compositing on
void testCompositingOff()
{
    Clip base;
    base.mediaPath = "/x/cs2/a.mkv";
    base.in = 0;
    base.out = 299;
    base.transform.compositeOn = false;
    base.keys[AnimParam::Opacity] = {Keyframe{0, 100, KeyEase::Linear}, Keyframe{299, 20, KeyEase::Linear}};
    const QSizeF frame(1080, 1920);
    Reframe::Feed all = timeline();
    for (Reframe::FeedSample& s : all.samples) s.lines = 1, s.left = 0.86, s.bottom = 0.1;
    const Clip shown = Reframe::keepCopy(base, {"killfeed", cs2().feed, all}, 16.0 / 9, frame);
    CHECK(!shown.keys.contains(AnimParam::Opacity));
    CHECK(near(Keys::valueAt(shown, AnimParam::Opacity, 150), 100, 1e-9));
    // Lines and none: keyed, with compositing
    const Clip c = Reframe::keepCopy(base, {"killfeed", cs2().feed, timeline()}, 16.0 / 9, frame);
    CHECK(c.transform.compositeOn);
    CHECK(near(Keys::valueAt(c, AnimParam::Opacity, 20), 0, 1e-9));
    CHECK(near(Keys::valueAt(c, AnimParam::Opacity, 180), 100, 1e-9));
}

// The base's own opacity keys carry into the copy: its opacity where the copy is shown, at every key of the base
void testBaseOpacity()
{
    Clip base;
    base.mediaPath = "/x/cs2/a.mkv";
    base.in = 100;
    base.out = 399;
    base.transform.compositeOn = true;
    // 100 until clip frame 150, 50 from 160
    base.keys[AnimParam::Opacity] = {Keyframe{250, 100, KeyEase::Linear}, Keyframe{260, 50, KeyEase::Linear}};
    const Clip c = Reframe::keepCopy(base, {"killfeed", cs2().feed, timeline()}, 16.0 / 9, QSizeF(1080, 1920));
    const auto at = [&](int t) { return Keys::valueAt(c, AnimParam::Opacity, t); };
    CHECK(near(at(20), 0, 1e-9));
    CHECK(near(at(90), 100, 1e-9));
    CHECK(near(at(150), 100, 1e-9));
    CHECK(near(at(160), 50, 1e-9));
    CHECK(near(at(180), 50, 1e-9));
    CHECK(near(at(270), 0, 1e-9));
}

// A changed picture shape after the reframe (a stretched 4:3 recording): the copy that follows its lines is placed
// for the new shape as a new copy would be; one placed by hand stays
void testReshape()
{
    Clip base;
    base.mediaPath = "/x/cs2/a.mkv";
    base.in = 100;
    base.out = 399;
    const QSizeF frame(1080, 1920);
    const QRectF region = cs2().feed;
    const Reframe::Keep keep{"killfeed", region, timeline()};
    for (double aspect : {4.0 / 3, 21.0 / 9}) {
        Clip copy = Reframe::keepCopy(base, keep, 16.0 / 9, frame);
        // As Editor::setPictureAspect gives it the new shape: the crop keeps its share of the picture
        const QSizeF a = MediaAspect::fit(16.0 / 9, frame), b = MediaAspect::fit(aspect, frame);
        copy.pictureAspect = aspect;
        for (AnimParam p : {AnimParam::CropLeft, AnimParam::CropRight, AnimParam::CropTop, AnimParam::CropBottom}) {
            const double k = p == AnimParam::CropLeft || p == AnimParam::CropRight ? b.width() / a.width()
                                                                                    : b.height() / a.height();
            Keys::convert(copy, p, [k](double v) { return v * k; }, k);
        }
        Clip byHand = copy;
        CHECK(Reframe::reshapeKeep(copy, 16.0 / 9, aspect, frame));
        // About the size a new copy for the new shape has (its padding left of the lines differs a little: a part
        // of the line height), at the top of the frame and centred, within the limits of a kept region
        Clip shaped = base;
        shaped.pictureAspect = aspect;
        const Clip want = Reframe::keepCopy(shaped, keep, aspect, frame);
        const Reframe::KeepPlace place;
        const double line = 31.0 / 1080 * b.height();
        for (int t : {0, 56, 59, 90, 119, 150, 180, 240, 299}) { // (keys: not halfway through an eased change)
            const auto at = [&](AnimParam p) { return Keys::valueAt(copy, p, t); };
            const double z = at(AnimParam::ZoomX);
            CHECK(near(z, at(AnimParam::ZoomY), 1e-9));
            if (!CHECK(near(z, Keys::valueAt(want, AnimParam::ZoomX, t), 0.015 * z)))
                std::printf("aspect %.3f frame %d: zoom %.4f, want %.4f\n", aspect, t, z, Keys::valueAt(want, AnimParam::ZoomX, t));
            const double w = b.width() - at(AnimParam::CropLeft) - at(AnimParam::CropRight);
            const double h = b.height() - at(AnimParam::CropTop) - at(AnimParam::CropBottom);
            CHECK(z * line <= place.maxLine * frame.height() + 1e-6);
            CHECK(z * w <= place.width * frame.width() + 1e-6);
            CHECK(z * h <= place.maxHeight * frame.height() + 1e-6);
            const double topEdge = frame.height() / 2 - at(AnimParam::PosY) + z * (at(AnimParam::CropTop) - b.height() / 2);
            CHECK(near(topEdge, place.top * frame.height(), 0.5));
            const double leftEdge = frame.width() / 2 + at(AnimParam::PosX) + z * (at(AnimParam::CropLeft) - b.width() / 2);
            if (!CHECK(near(leftEdge + z * w / 2, frame.width() / 2, 0.5)))
                std::printf("aspect %.3f frame %d: centre %.3f\n", aspect, t, leftEdge + z * w / 2);
        }
        // Moved by hand: left as it is
        KeyTrack& x = byHand.keys[AnimParam::PosX];
        x[x.size() / 2].value += 40;
        const Clip before = byHand;
        CHECK(!Reframe::reshapeKeep(byHand, 16.0 / 9, aspect, frame));
        CHECK(byHand.keys == before.keys);
    }
}

QJsonObject run(const QString& name, const QJsonObject& args)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    return cmd->run(Cli::normalize(*cmd, args), ctx);
}

int whiteIn(const QImage& img, const QRect& r)
{
    int n = 0;
    for (int y = std::max(0, r.top()); y <= std::min(img.height() - 1, r.bottom()); ++y)
        for (int x = std::max(0, r.left()); x <= std::min(img.width() - 1, r.right()); ++x) {
            const QRgb p = img.pixel(x, y);
            n += qRed(p) > 200 && qGreen(p) > 200 && qBlue(p) > 200;
        }
    return n;
}

// A CS2-like 1080p recording: no line until 2 s, one line from 2 s, three from 4 s; and one without any line
void testCli(const QString& dir)
{
    QDir(dir).mkpath("cs2");
    const QString text = [] {
        // White bars as text inside the boxes
        QStringList parts;
        const struct { int y, width; const char* when; } lines[] = {
            {75, 250, "gte(t,2)"}, {109, 380, "gte(t,4)"}, {142, 200, "gte(t,4)"}};
        for (const auto& l : lines) {
            parts << QString("drawbox=x=%1:y=%2:w=%3:h=31:c=black@0.55:t=fill:enable='%4'")
                         .arg(1909 - l.width).arg(l.y).arg(l.width).arg(l.when);
            for (int x = 1909 - l.width + 10; x < 1909 - 10; x += 9)
                parts << QString("drawbox=x=%1:y=%2:w=5:h=12:c=0xebebeb:t=fill:enable='%3'").arg(x).arg(l.y + 10).arg(l.when);
        }
        return parts.join(',');
    }();
    const QString back = "gradients=s=1920x1080:c0=0xc8a878:c1=0x705030:d=6:r=30:speed=0.002,drawgrid=w=97:h=61:t=2:c=0x403020@0.6";
    const QString game = Check::makeMedia(dir + "/cs2/feed.mkv", {"-f", "lavfi", "-i", back + "," + text, "-c:v",
                                                                  "libx264", "-qp", "8", "-pix_fmt", "yuv420p", "-g", "30"});
    const QString empty = Check::makeMedia(dir + "/cs2/empty.mkv", {"-f", "lavfi", "-i", back, "-t", "3", "-c:v", "libx264",
                                                                    "-qp", "8", "-pix_fmt", "yuv420p"});
    if (!CHECK(!game.isEmpty() && !empty.isEmpty())) return;
    // The lines as the analysis reads them from the file
    {
        QString error;
        const auto samples = KillFeedLines::measure(game, 0, 6, QRectF(0, 0, 1, 1), cs2(), &error);
        if (qEnvironmentVariableIsSet("KILLFEED_DEBUG") && samples) for (const auto& s : *samples) std::printf("%.3f %d %.4f %.4f\n", s.seconds, s.lines, s.left, s.bottom);
        if (CHECK(samples.has_value())) {
            int at1 = -1, at3 = -1, at5 = -1;
            for (const KillFeedLines::Sample& s : *samples) {
                if (near(s.seconds, 1, 0.06)) at1 = s.lines;
                if (near(s.seconds, 3, 0.06)) at3 = s.lines;
                if (near(s.seconds, 5, 0.06)) at5 = s.lines;
            }
            CHECK_EQ(at1, 0);
            CHECK_EQ(at3, 1);
            CHECK_EQ(at5, 3);
            CHECK(samples->size() >= 55 && samples->size() <= 62); // 10 a second
        }
        // Cached: the same again without reading
        CHECK(QDir(KillFeedLines::cacheDir()).entryList({"*.killfeed"}, QDir::Files).size() >= 1);
        CHECK(KillFeedLines::measure(game, 1, 5, QRectF(0, 0, 1, 1), cs2()) == [&] {
            auto all = *KillFeedLines::measure(game, 0, 6, QRectF(0, 0, 1, 1), cs2());
            return std::optional<KillFeedLines::Samples>(all);
        }());
    }

    // A file that breaks off (cut short after it was written: its header still names the whole length): what was read
    // is not cached as the whole range
    {
        const QString cut = dir + "/cs2/cut.mkv";
        if (CHECK(QFile::copy(game, cut))) {
            QFile f(cut);
            CHECK(f.resize(f.size() / 2));
            const auto count = [] { return QDir(KillFeedLines::cacheDir()).entryList({"*.killfeed"}, QDir::Files).size(); };
            const qsizetype before = count();
            QString error;
            const auto samples = KillFeedLines::measure(cut, 0, 6, QRectF(0, 0, 1, 1), cs2(), &error);
            if (samples) CHECK(samples->size() < 50); // a part only
            CHECK_EQ(count(), before);
        }
    }

    const QString path = dir + "/fit.schneidi";
    run("new", {{"project", path}, {"media", QJsonArray{game, empty}}});
    run("edit", {{"project", path}, {"ops", QString(R"([{"op":"append","media":"%1"},{"op":"append","media":"%2"}])")
                                                .arg(game, empty)}});
    const QJsonObject out = run("edit", {{"project", path}, {"ops", R"([{"op":"new_timeline","from":"Timeline 1",
        "name":"Short","aspect":"9:16","reframe":"center"}])"}});
    bool followed = false, noLines = false;
    for (const QJsonValue& n : out["notes"].toArray()) {
        followed |= n.toString().startsWith("the kill feed copy follows its lines");
        noLines |= n.toString().startsWith("no kill feed line in clips");
    }
    CHECK(followed);
    CHECK(noLines);

    ProjectData d;
    QString error;
    if (!CHECK(ProjectFile::load(path, &d, &error))) return;
    Project q;
    TimelineGuard guard_q{q};
    q.load(d);
    for (const Sequence& s : q.sequences())
        if (s.name == "Short") q.setCurrentSequence(s.id);
    const Timeline& tl = q.timeline();
    QVector<const Clip*> copies;
    for (const Track& t : tl.video)
        for (const Clip& c : t.clips)
            if (c.keepRegion == "killfeed") copies << &c;
    if (!CHECK_EQ(copies.size(), 1)) return; // the recording without lines has none
    const Clip& c = *copies[0];
    CHECK_EQ(c.mediaPath, game);
    const double fps = q.timelineFormat().rate.fps();
    const QSizeF fitted = MediaAspect::fit(16.0 / 9.0, QSizeF(1080, 1920));
    const auto at = [&](AnimParam p, double s) { return Keys::valueAt(c, p, s * fps - c.start); };
    CHECK(near(at(AnimParam::Opacity, 1), 0, 1e-9));
    CHECK(near(at(AnimParam::Opacity, 3), 100, 1e-9));
    CHECK(near(at(AnimParam::Opacity, 5), 100, 1e-9));
    const double line = 31.0 / 1080 * fitted.height();
    const auto leftAt = [&](double s) { return at(AnimParam::CropLeft, s) / fitted.width() * 1920; };
    const auto bottomAt = [&](double s) { return (fitted.height() - at(AnimParam::CropBottom, s)) / fitted.height() * 1080; };
    CHECK(leftAt(3) < 1659 && leftAt(3) > 1659 - 31);
    CHECK(bottomAt(3) > 105 && bottomAt(3) < 106 + 16);
    CHECK(leftAt(5) < 1529 && leftAt(5) > 1529 - 31);
    CHECK(bottomAt(5) > 172 && bottomAt(5) < 173 + 16);
    (void)line;

    // Rendered: the lines' text shows at the top only while there are lines
    const auto frame = [&](double s) {
        return Snapshot::Source::timeline(q.timelineFormat(), tl)->frame(int(std::lround(s * fps))).convertToFormat(QImage::Format_RGB32);
    };
    const QImage f1 = frame(1), f3 = frame(3), f5 = frame(5);
    if (CHECK(!f1.isNull() && !f3.isNull() && !f5.isNull())) {
        const QRect top(0, 0, f1.width(), int(f1.height() * 0.3));
        CHECK_EQ(whiteIn(f1, top), 0);
        const int one = whiteIn(f3, top), three = whiteIn(f5, top);
        CHECK(one > 200);
        CHECK(three > one * 2);
        if (qEnvironmentVariableIsSet("KILLFEED_DUMP")) {
            const QString d = qEnvironmentVariable("KILLFEED_DUMP");
            QDir().mkpath(d);
            f1.save(d + "/1s.png");
            f3.save(d + "/3s.png");
            f5.save(d + "/5s.png");
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("killfeed_fit");
    std::setlocale(LC_NUMERIC, "C");
    testDetect();
    testStates();
    testKeys();
    testSpeed();
    testFallback();
    testCompositingOff();
    testBaseOpacity();
    testReshape();
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    Check::initEngine();
    std::setlocale(LC_NUMERIC, "C");
    testCli(tmp.path());
    return Check::result();
}
