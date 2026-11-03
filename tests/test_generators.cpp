// SPDX-License-Identifier: AGPL-3.0-only
// Shorts building blocks: push/whip transitions and eased wipes (pixels at 0/50/100 % per direction), the animated
// counter (value and text at given frames, easing, decimals, German vs English separators, drawn like a text title
// with that text), the progress bar (filled width at given frames in both spans), project file round trip, the CLI
// ops and `check` on a progress bar at the bottom of a vertical timeline.
// SCHNEIDI_PICTURE_DUMP=<folder>: saves the rendered pictures there to look at.
#include "check.h"

#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/Editor.h"
#include "core/ExportCheck.h"
#include "core/Generators.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/ProjectFormat.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "ui/Inspector.h"

#include <QApplication>
#include <QComboBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QTest>
#include <QSlider>
#include <QWheelEvent>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <clocale>
#include <cmath>

namespace {

const ProjectFormat kFormat{320, 180, {25, 1}};
QString g_a, g_b; // stills: A red with a white stripe at its left edge and top, B blue with green at right/bottom

QImage render(const Timeline& tl, int frame, const ProjectFormat& fmt = kFormat)
{
    const QImage img = Check::renderFrame(tl, fmt, frame);
    if (const QString dump = qEnvironmentVariable("SCHNEIDI_PICTURE_DUMP"); !dump.isEmpty() && !img.isNull()) {
        static int n = 0;
        img.save(QString("%1/generators-%2.png").arg(dump).arg(n++, 3, 10, QChar('0')));
    }
    return img;
}

QString still(const QString& path, QColor fill, QColor mark, bool leftTop)
{
    QImage img(320, 180, QImage::Format_RGB32);
    img.fill(fill);
    for (int y = 0; y < 180; ++y)
        for (int x = 0; x < 320; ++x) {
            const bool edge = leftTop ? (x < 10 || y < 10) : (x >= 310 || y >= 170);
            if (edge) img.setPixel(x, y, mark.rgb());
        }
    img.save(path);
    return path;
}

Clip media(int id, const QString& path, int start, int length)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.in = 0;
    c.out = length - 1;
    return c;
}

// A (frames 0..24) and B (25..49) with a 10-frame transition centred on the cut: frames 20..29
Timeline cut(TransitionType type, const std::function<void(TransitionStyle&)>& style = {})
{
    Clip a = media(1, g_a, 0, 25), b = media(2, g_b, 25, 25);
    a.in = 10, a.out = 34; // handles for a centred transition
    b.in = 10, b.out = 34;
    a.transOut = b.transIn = 10;
    TransitionStyle st;
    st.type = type;
    if (style) style(st);
    a.transOutStyle = b.transInStyle = st;
    Timeline tl;
    tl.video.resize(1);
    tl.video[0].clips << a << b;
    return tl;
}

bool near(QRgb p, QColor c, int tol = 3)
{
    return std::abs(qRed(p) - c.red()) <= tol && std::abs(qGreen(p) - c.green()) <= tol && std::abs(qBlue(p) - c.blue()) <= tol;
}

// Every pixel in r has colour c
bool all(const QImage& img, QRect r, QColor c)
{
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x)
            if (!near(img.pixel(x, y), c)) {
                std::printf("       (%d,%d) = %08x, wanted %s\n", x, y, img.pixel(x, y), qPrintable(c.name()));
                return false;
            }
    return true;
}

const QColor kRed(220, 30, 30), kBlue(30, 60, 220), kWhite(Qt::white), kGreen(30, 200, 60);

void testPush()
{
    std::printf("push\n");
    // 0 %: A untouched; 100 % (the frame after the span): B
    for (TransitionType t : {TransitionType::PushRight, TransitionType::PushLeft, TransitionType::PushDown,
                             TransitionType::PushUp}) {
        const Timeline tl = cut(t);
        const QImage first = render(tl, 20), after = render(tl, 30);
        CHECK(all(first, QRect(20, 20, 280, 140), kRed));
        CHECK(all(first, QRect(0, 0, 10, 180), kWhite));
        CHECK(all(after, QRect(0, 0, 300, 160), kBlue));
        CHECK(all(after, QRect(310, 0, 10, 180), kGreen));
    }
    // 50 % (frame 25 = 5 of 10): both moved half the frame
    {
        const QImage img = render(cut(TransitionType::PushRight), 25);
        CHECK(all(img, QRect(0, 20, 150, 140), kBlue));    // B's right half on the left …
        CHECK(all(img, QRect(150, 20, 10, 140), kGreen));  // … with its right edge
        CHECK(all(img, QRect(160, 20, 10, 140), kWhite));  // A's left edge moved to the middle
        CHECK(all(img, QRect(170, 20, 150, 140), kRed));
    }
    {
        const QImage img = render(cut(TransitionType::PushLeft), 25);
        CHECK(all(img, QRect(0, 20, 160, 140), kRed));     // A's right half on the left
        CHECK(all(img, QRect(160, 20, 150, 140), kBlue));  // B's left half on the right
    }
    {
        const QImage img = render(cut(TransitionType::PushDown), 25);
        CHECK(all(img, QRect(20, 0, 280, 80), kBlue));
        CHECK(all(img, QRect(20, 80, 280, 10), kGreen));   // B's bottom edge above the middle
        CHECK(all(img, QRect(20, 90, 280, 10), kWhite));   // A's top edge below it
        CHECK(all(img, QRect(20, 100, 280, 80), kRed));
    }
    {
        const QImage img = render(cut(TransitionType::PushUp), 25);
        CHECK(all(img, QRect(20, 0, 280, 90), kRed));
        CHECK(all(img, QRect(20, 90, 280, 80), kBlue));
    }
    // Ease in: at half the time a cubic ease has moved 1/8 (40 px)
    {
        const QImage img = render(cut(TransitionType::PushRight, [](TransitionStyle& s) { s.ease = MotionEase::In; }), 25);
        CHECK(all(img, QRect(40, 20, 10, 140), kWhite));
        CHECK(all(img, QRect(30, 20, 10, 140), kGreen));
        CHECK(all(img, QRect(60, 20, 250, 140), kRed));
    }
}

void testWipes()
{
    std::printf("wipes\n");
    struct Case { TransitionType type; QRect b, a; };
    const Case cases[] = {
        {TransitionType::WipeRight, QRect(20, 20, 120, 140), QRect(180, 20, 120, 140)},
        {TransitionType::WipeLeft, QRect(180, 20, 120, 140), QRect(20, 20, 120, 140)},
        {TransitionType::WipeDown, QRect(20, 20, 280, 50), QRect(20, 110, 280, 50)},
        {TransitionType::WipeUp, QRect(20, 110, 280, 50), QRect(20, 20, 280, 50)},
    };
    for (const Case& c : cases) {
        const Timeline tl = cut(c.type);
        CHECK(all(render(tl, 20), QRect(20, 20, 280, 140), kRed));
        const QImage half = render(tl, 25);
        CHECK(all(half, c.b, kBlue));
        CHECK(all(half, c.a, kRed));
        CHECK(all(render(tl, 30), QRect(20, 20, 280, 140), kBlue));
    }
    // Eased (out): at half the time 7/8 of the way
    const QImage eased = render(cut(TransitionType::WipeRight, [](TransitionStyle& s) { s.ease = MotionEase::Out; }), 25);
    CHECK(all(eased, QRect(20, 20, 240, 140), kBlue));
    CHECK(all(eased, QRect(300, 20, 20, 140), kRed));
}

void testWhip()
{
    std::printf("whip\n");
    // Without blur a whip is a push with the same curve
    const auto noBlur = [](TransitionStyle& s) {
        s.blur = 0;
        s.ease = MotionEase::InOut;
    };
    const QImage whip0 = render(cut(TransitionType::WhipLeft, noBlur), 24);
    const QImage push = render(cut(TransitionType::PushLeft, [](TransitionStyle& s) { s.ease = MotionEase::InOut; }), 24);
    CHECK(whip0 == push);
    // With blur: smeared along the direction only (rows stay alike, columns mix red and blue at the seam)
    const QImage whip = render(cut(TransitionType::WhipLeft, [](TransitionStyle& s) { s.ease = MotionEase::InOut; }), 25);
    CHECK(whip != render(cut(TransitionType::PushLeft, [](TransitionStyle& s) { s.ease = MotionEase::InOut; }), 25));
    const QRgb seam = whip.pixel(160, 90);
    CHECK(qRed(seam) > 60 && qBlue(seam) > 60); // a mix
    CHECK(whip.pixel(160, 60) == whip.pixel(160, 120)); // no vertical blur
    // Ends untouched
    CHECK(all(render(cut(TransitionType::WhipLeft), 20), QRect(20, 20, 280, 140), kRed));
    CHECK(all(render(cut(TransitionType::WhipLeft), 30), QRect(20, 20, 280, 140), kBlue));
    // From nothing (alone at the timeline's start): the clip slides in over black
    Clip only = media(1, g_b, 0, 25);
    only.transIn = 10;
    only.transInAlone = true;
    only.transInStyle.type = TransitionType::PushUp;
    Timeline tl;
    tl.video.resize(1);
    tl.video[0].clips << only;
    const QImage in = render(tl, 5);
    CHECK(all(in, QRect(20, 0, 280, 80), Qt::black));
    CHECK(all(in, QRect(20, 100, 280, 60), kBlue));
    // A whip from nothing on a transparent frame: the smeared edge fades out in the clip's own colour (blurred
    // premultiplied: transparency does not darken it), the frame's far edge stays opaque (edge pixels repeat)
    tl.video[0].clips[0].transInStyle.type = TransitionType::WhipLeft;
    tl.video[0].clips[0].transInStyle.ease = MotionEase::InOut;
    const QImage smear = Check::renderFrame(tl, kFormat, 5, {}, true);
    int soft = 0;
    for (int x = 0; x < 320; ++x) {
        const QRgb p = smear.pixel(x, 90);
        if (qAlpha(p) > 10 && qAlpha(p) < 245) {
            ++soft;
            CHECK(qBlue(p) > 200 && qRed(p) < 45);
        }
    }
    CHECK(soft > 10);
    CHECK_EQ(qAlpha(smear.pixel(300, 90)), 255);
}

void testCounterValues()
{
    std::printf("counter values\n");
    CHECK_EQ(Generators::ease(MotionEase::Linear, 0.25), 0.25);
    CHECK_EQ(Generators::ease(MotionEase::In, 0.5), 0.125);
    CHECK_EQ(Generators::ease(MotionEase::Out, 0.5), 0.875);
    CHECK_EQ(Generators::ease(MotionEase::InOut, 0.5), 0.5);
    CHECK_EQ(Generators::ease(MotionEase::InOut, 0.25), 0.0625);
    CHECK_EQ(Generators::ease(MotionEase::Out, 2.0), 1.0);

    TitleStyle s;
    s.kind = TitleKind::Counter;
    s.countFrom = 0;
    s.countTo = 1000;
    s.countEase = MotionEase::Linear;
    // 101 frames: frame k = 10 k
    CHECK_EQ(Generators::counterValue(s, 0, 101), 0.0);
    CHECK_EQ(Generators::counterValue(s, 50, 101), 500.0);
    CHECK_EQ(Generators::counterValue(s, 100, 101), 1000.0);
    CHECK_EQ(Generators::counterValue(s, 33, 101), 330.0);
    s.countEase = MotionEase::Out;
    CHECK_EQ(Generators::counterValue(s, 50, 101), 875.0);
    s.countEase = MotionEase::Linear;
    s.countTo = 1;
    s.decimals = 2;
    CHECK_EQ(Generators::counterValue(s, 33, 101), 0.33);
    s.decimals = 0;
    CHECK_EQ(Generators::counterValue(s, 33, 101), 0.0);
    CHECK_EQ(Generators::counterValue(s, 50, 101), 1.0); // 0.5 rounds away from zero

    CHECK_EQ(Generators::formatNumber(1234567.891, 2, true, ',', '.'), QString("1.234.567,89"));
    CHECK_EQ(Generators::formatNumber(1234567.891, 2, true, '.', ','), QString("1,234,567.89"));
    CHECK_EQ(Generators::formatNumber(1234567, 0, false, '.', ','), QString("1234567"));
    CHECK_EQ(Generators::formatNumber(-1234.5, 1, true, '.', ','), QString("-1,234.5"));
    CHECK_EQ(Generators::formatNumber(-0.001, 1, true, '.', ','), QString("0.0"));
    CHECK_EQ(Generators::formatNumber(999, 0, true, '.', ','), QString("999"));

    s.countFrom = 0;
    s.countTo = 2500;
    s.decimals = 1;
    s.prefix = "$";
    s.suffix = " k";
    // Separators are the counter's own, whatever the UI language of the process rendering it
    s.numbers = NumberStyle::CommaPoint;
    I18n::install("de");
    CHECK_EQ(Generators::counterText(s, 100, 101), QString("$2,500.0 k"));
    CHECK_EQ(Generators::counterText(s, 50, 101), QString("$1,250.0 k"));
    I18n::install("en");
    s.numbers = NumberStyle::PointComma;
    CHECK_EQ(Generators::counterText(s, 100, 101), QString("$2.500,0 k"));
    CHECK_EQ(Generators::counterText(s, 0, 101), QString("$0,0 k"));
    s.numbers = NumberStyle::SpaceComma;
    CHECK_EQ(Generators::counterText(s, 100, 101), QString("$2\u00a0500,0 k"));
    s.grouping = false;
    CHECK_EQ(Generators::counterText(s, 100, 101), QString("$2500,0 k"));
    // A new counter takes the UI language's separators (and keeps them)
    I18n::install("en");
    CHECK(Generators::numberStyleOfLanguage() == NumberStyle::CommaPoint);
    I18n::install("de");
    CHECK(Generators::numberStyleOfLanguage() == NumberStyle::PointComma);
    I18n::install("fr");
    CHECK(Generators::numberStyleOfLanguage() == NumberStyle::SpaceComma);
    I18n::install("en");
    // Negative numbers, 0.5 cases (away from zero both ways), huge numbers
    s.prefix.clear();
    s.suffix.clear();
    s.grouping = true;
    s.numbers = NumberStyle::CommaPoint;
    s.decimals = 0;
    s.countFrom = -1000;
    s.countTo = 0;
    CHECK_EQ(Generators::counterText(s, 0, 101), QString("-1,000"));
    CHECK_EQ(Generators::counterText(s, 100, 101), QString("0"));
    s.countFrom = 0;
    s.countTo = -1;
    CHECK_EQ(Generators::counterText(s, 50, 101), QString("-1"));
    s.countTo = 1e12;
    CHECK_EQ(Generators::counterText(s, 100, 101), QString("1,000,000,000,000"));
}

void testCounterPicture()
{
    std::printf("counter picture\n");
    I18n::install("en");
    Clip counter;
    counter.id = 1;
    counter.kind = ClipKind::Title;
    counter.out = 24;
    counter.title.kind = TitleKind::Counter;
    counter.title.countFrom = 0;
    counter.title.countTo = 240;
    counter.title.countEase = MotionEase::Linear;
    counter.title.size = 60;
    counter.title.suffix = " kills";
    Timeline tl;
    tl.video.resize(1);
    tl.video[0].clips << counter;
    // Frame 12 of 25 shows "120 kills": drawn exactly like a text title with that text (centred block)
    Clip text = counter;
    text.title.kind = TitleKind::Text;
    text.title.text = "120 kills";
    Timeline ref;
    ref.video.resize(1);
    ref.video[0].clips << text;
    CHECK(render(tl, 12) == render(ref, 12));
    CHECK(render(tl, 0) != render(tl, 12));
    // The same counter as the last frame of a 25 frame title: "240 kills"
    text.title.text = "240 kills";
    ref.video[0].clips[0] = text;
    CHECK(render(tl, 24) == render(ref, 24));
}

// Width of the yellow run in row y
int barWidth(const QImage& img, int y)
{
    int w = 0;
    for (int x = 0; x < img.width(); ++x) {
        const QRgb p = img.pixel(x, y);
        if (qRed(p) > 200 && qGreen(p) > 150 && qBlue(p) < 80) ++w;
    }
    return w;
}

void testProgress()
{
    std::printf("progress bar\n");
    // Timeline of 101 frames (a clip 0..100 underneath), bar title over frames 50..99 only
    Clip under = media(1, g_b, 0, 101);
    Clip bar;
    bar.id = 2;
    bar.kind = ClipKind::Title;
    bar.start = 50;
    bar.out = 49;
    bar.title.kind = TitleKind::Progress;
    bar.title.color = QColor(255, 214, 0);
    bar.title.barRound = false;
    bar.title.barThickness = 10;
    bar.title.boxOn = true;
    bar.title.boxColor = QColor(0, 0, 0, 255);
    Timeline tl;
    tl.video.resize(2);
    tl.video[0].clips << under;
    tl.video[1].clips << bar;
    // Timeline span: frame f of 101 = f / 100 of the width, wherever the title sits
    CHECK_EQ(barWidth(render(tl, 50), 5), 160);
    CHECK_EQ(barWidth(render(tl, 75), 5), 240);
    CHECK_EQ(barWidth(render(tl, 99), 5), 317); // 316.8
    // The track: black over the rest of the bar, the picture below it
    const QImage at75 = render(tl, 75);
    CHECK(near(at75.pixel(300, 5), Qt::black));
    CHECK(near(at75.pixel(300, 20), kBlue));
    // Export of a range (frames 50..99): the bar fills over the exported video, from empty to full
    {
        Picture::Options o;
        o.format = kFormat;
        o.mode = Picture::Mode::Export;
        o.rangeStart = 50;
        o.rangeEnd = 99;
        Picture::Renderer r(tl, o);
        CHECK_EQ(barWidth(r.render(50).toImage(), 5), 0);
        const int mid = barWidth(r.render(75).toImage(), 5); // 25 / 49 = 163.3 px
        CHECK(mid >= 163 && mid <= 164);
        CHECK_EQ(barWidth(r.render(99).toImage(), 5), 320);
    }
    // Clip span: over the title's 50 frames
    tl.video[1].clips[0].title.span = ProgressSpan::Clip;
    CHECK_EQ(barWidth(render(tl, 50), 5), 0);
    CHECK_EQ(barWidth(render(tl, 99), 5), 320);
    const int w74 = barWidth(render(tl, 74), 5); // 24 / 49 = 156.7 px, the last one antialiased
    CHECK(w74 >= 156 && w74 <= 157);
    // Bottom edge with a margin
    tl.video[1].clips[0].title.barTop = false;
    tl.video[1].clips[0].title.barMargin = 20;
    const QImage bottom = render(tl, 99);
    CHECK_EQ(barWidth(bottom, 5), 0);
    CHECK_EQ(barWidth(bottom, 180 - 20 - 5), 320);
    // Its place follows the timeline's own resolution (a 9:16 timeline: bar 1080 wide)
    const ProjectFormat vertical{1080, 1920, {25, 1}};
    tl.video[1].clips[0].title.barTop = true;
    tl.video[1].clips[0].title.barMargin = 0;
    CHECK_EQ(barWidth(render(tl, 99, vertical), 5), 1080);
}

// A progress bar dropped onto a track spans the whole timeline: on a track with clips that would overwrite them all
void testProgressDrop()
{
    std::printf("progress bar drop\n");
    Project p;
    ProjectData d;
    d.timeline = emptyTimeline();
    p.load(d);
    p.edit("setup", [&](Timeline& tl) {
        Clip a = media(p.newClipId(), "/x/a.mp4", 0, 100);
        Clip b = media(p.newClipId(), "/x/b.mp4", 100, 50);
        tl.video[0].clips << a << b;
    });
    Selection sel;
    Editor e(&p, &sel);
    e.addTitle(60, 0, 0, TitleKind::Progress);
    const Timeline& tl = p.timeline();
    CHECK_EQ(tl.video[0].clips.size(), 2); // V1 untouched
    CHECK_EQ(tl.video[0].clips.value(0).length(), 100);
    CHECK_EQ(tl.video[0].clips.value(1).length(), 50);
    if (CHECK(tl.video.size() >= 2 && tl.video[1].clips.size() == 1)) {
        CHECK(tl.video[1].clips[0].title.isProgress());
        CHECK_EQ(tl.video[1].clips[0].start, 0);
        CHECK_EQ(tl.video[1].clips[0].length(), 150);
    }
    // Onto an empty track: there
    p.edit("track", [](Timeline& t) { TimelineOps::ensureTracks(t, TrackKind::Video, 4); });
    e.addTitle(10, 3, 0, TitleKind::Progress);
    CHECK_EQ(p.timeline().video[3].clips.size(), 1);
    // A counter keeps the language's separators (the test runs in English)
    e.addTitle(10, -1, 25, TitleKind::Counter);
    CHECK(TimelineOps::findClip(p.timeline(), *sel.ids().begin())->title.numbers == NumberStyle::CommaPoint);
}

// The Inspector of a counter: scrolling the Inspector with the mouse wheel over a slider must not change the count by
// millions (the From/To sliders spanned +-1e9: one wheel notch over them moved "To" from 1000 to about 6,001,000)
void testCounterInspector()
{
    std::printf("counter inspector\n");
    Project p;
    ProjectData d;
    d.timeline = emptyTimeline();
    p.load(d);
    Selection sel;
    Editor e(&p, &sel);
    Inspector insp(&e);
    e.addTitle(0, -1, 50, TitleKind::Counter);
    const int id = *sel.ids().begin();
    insp.resize(380, 1600);
    insp.show();
    QCoreApplication::processEvents();
    int sliders = 0;
    for (QSlider* s : insp.findChildren<QSlider*>()) {
        if (!s->isVisibleTo(&insp)) continue;
        ++sliders;
        const QPointF at(s->width() / 2.0, s->height() / 2.0);
        QWheelEvent wheel(at, s->mapToGlobal(at), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(s, &wheel);
    }
    CHECK(sliders > 0); // the title page is shown (size, outline …)
    const Clip* c = TimelineOps::findClip(p.timeline(), id);
    if (CHECK(c)) {
        CHECK(std::abs(c->title.countFrom - TitleStyle().countFrom) < 100);
        CHECK(std::abs(c->title.countTo - TitleStyle().countTo) < 100);
    }
}

// Scrolling the Inspector with the mouse wheel over a slider or a combo box (not clicked into) scrolls the page and
// leaves the value alone (window-system wheel events, so they propagate like the user's)
void testInspectorWheel()
{
    std::printf("inspector wheel\n");
    Project p;
    ProjectData d;
    d.timeline = emptyTimeline();
    p.load(d);
    Selection sel;
    Editor e(&p, &sel);
    Inspector insp(&e);
    e.addTitle(0, -1, 50, TitleKind::Text);
    insp.resize(380, 420);
    insp.show();
    QCoreApplication::processEvents();
    const Timeline before = p.timeline();
    auto wheelOver = [&](QWidget* w) -> QScrollBar* {
        QScrollArea* area = nullptr;
        for (QWidget* up = w; up && !area; up = up->parentWidget()) area = qobject_cast<QScrollArea*>(up);
        if (!CHECK(area)) return nullptr;
        area->verticalScrollBar()->setValue(0);
        QCoreApplication::processEvents();
        const QPoint at = w->mapTo(insp.window(), w->rect().center());
        QTest::wheelEvent(insp.window()->windowHandle(), at, QPoint(0, -120));
        return area->verticalScrollBar();
    };
    // shown and really under the mouse at its centre (not covered, inside the visible part of the page)
    auto underMouse = [&](QWidget* w) {
        if (!w->isVisibleTo(&insp) || !w->isEnabled()) return false;
        const QPoint at = w->mapTo(&insp, w->rect().center());
        if (!insp.rect().contains(at)) return false;
        for (QWidget* hit = insp.childAt(at); hit; hit = hit->parentWidget())
            if (hit == w) return true;
        return false;
    };
    QSlider* slider = nullptr;
    for (QSlider* s : insp.findChildren<QSlider*>())
        if (!slider && underMouse(s)) slider = s;
    QComboBox* combo = nullptr;
    for (QComboBox* c : insp.findChildren<QComboBox*>())
        if (!combo && underMouse(c) && c->count() > 1) combo = c;
    if (CHECK(slider)) {
        const int value = slider->value();
        QScrollBar* bar = wheelOver(slider);
        CHECK_EQ(slider->value(), value);
        if (bar) CHECK(bar->value() > 0); // the page scrolled
    }
    if (CHECK(combo)) {
        const int index = combo->currentIndex();
        QScrollBar* bar = wheelOver(combo);
        CHECK_EQ(combo->currentIndex(), index);
        if (bar) CHECK(bar->value() > 0);
    }
    CHECK(p.timeline() == before); // nothing changed in the project
}

// Counter decimal places in the Inspector: 0..6 like the project file and the CLI
void testCounterDecimals()
{
    std::printf("counter decimals\n");
    Project p;
    ProjectData d;
    d.timeline = emptyTimeline();
    p.load(d);
    Selection sel;
    Editor e(&p, &sel);
    Inspector insp(&e);
    e.addTitle(0, -1, 50, TitleKind::Counter);
    const int id = *sel.ids().begin();
    p.edit("decimals", [id](Timeline& tl) { TimelineOps::findClip(tl, id)->title.decimals = 5; });
    QComboBox* places = nullptr;
    for (QComboBox* c : insp.findChildren<QComboBox*>())
        if (c->count() > 0 && c->itemText(0) == "0" && c->itemData(0).toInt() == 0) places = c;
    if (!CHECK(places)) return;
    CHECK_EQ(places->count(), 7);
    CHECK_EQ(places->currentText(), QString("5"));
    places->setCurrentIndex(6);
    emit places->activated(6);
    const Clip* c = TimelineOps::findClip(p.timeline(), id);
    if (CHECK(c)) CHECK_EQ(c->title.decimals, 6);
}

void testProjectFile(const QString& dir)
{
    std::printf("project file\n");
    Project p;
    ProjectData d;
    d.timeline = emptyTimeline();
    p.load(d);
    p.edit("setup", [&](Timeline& tl) {
        Clip c;
        c.id = p.newClipId();
        c.kind = ClipKind::Title;
        c.out = 49;
        c.title.kind = TitleKind::Counter;
        c.title.countFrom = -5;
        c.title.countTo = 12.5;
        c.title.decimals = 2;
        c.title.prefix = "$";
        c.title.suffix = "%";
        c.title.grouping = false;
        c.title.countEase = MotionEase::InOut;
        c.title.numbers = NumberStyle::SpaceComma;
        c.transOut = 10;
        c.transOutStyle.type = TransitionType::WhipUp;
        c.transOutStyle.ease = MotionEase::In;
        c.transOutStyle.blur = 80;
        tl.video[0].clips << c;
        Clip b = c;
        b.id = p.newClipId();
        b.start = 50;
        b.transOut = 0;
        b.transOutStyle = {};
        b.title = TitleStyle();
        b.title.kind = TitleKind::Progress;
        b.title.span = ProgressSpan::Clip;
        b.title.barTop = false;
        b.title.barThickness = 33;
        b.title.barMargin = 7;
        b.title.barRound = false;
        tl.video[0].clips << b;
    });
    const QString path = dir + "/g.schneidi";
    QString error;
    CHECK(ProjectFile::save(p.data(), path, &error));
    ProjectData back;
    CHECK(ProjectFile::load(path, &back, &error));
    CHECK(back.timeline == p.timeline());
    // A plain text title keeps its old shape in the file (no counter/progress fields)
    Clip text;
    text.kind = ClipKind::Title;
    CHECK(!ProjectFile::clipJson(text).value("title").toObject().contains("kind"));
}

QString g_path;

QJsonObject run(const QString& name, QJsonObject raw)
{
    if (name == "edit") raw["full"] = true;
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    return cmd->run(Cli::normalize(*cmd, raw), ctx);
}

QString errorOf(const QString& name, const QJsonObject& raw)
{
    try {
        run(name, raw);
    } catch (const Cli::Error& e) {
        return e.code;
    }
    return "none";
}

QJsonObject edit(const QString& ops) { return run("edit", {{"project", g_path}, {"ops", ops}}); }

ProjectData saved()
{
    ProjectData d;
    QString error;
    ProjectFile::load(g_path, &d, &error);
    return d;
}

void testCli(const QString& dir)
{
    std::printf("cli\n");
    g_path = dir + "/cli.schneidi";
    {
        Project p;
        ProjectData d;
        d.timeline = emptyTimeline();
        d.format = ProjectFormat{1080, 1920, {25, 1}};
        p.load(d);
        p.addMedia({"/x/a.mp4", "a.mp4", 100, true, false, false});
        p.addMedia({"/x/b.mp4", "b.mp4", 100, true, false, false});
        p.edit("setup", [&](Timeline& tl) {
            Clip a;
            a.id = p.newClipId();
            a.mediaPath = "/x/a.mp4";
            a.in = 10;
            a.out = 59;
            Clip b = a;
            b.id = p.newClipId();
            b.mediaPath = "/x/b.mp4";
            b.start = 50;
            tl.video[0].clips << a << b;
        });
        QString error;
        CHECK(ProjectFile::save(p.data(), g_path, &error));
    }
    // `effects` lists the new transition types
    const QJsonArray types = run("effects", {}).value("transition_types").toArray();
    for (const char* t : {"push_right", "push_up", "whip_left", "whip_down"}) CHECK(types.contains(QString(t)));

    // Push and whip at the cut; a whip is eased in_out unless told otherwise
    edit(R"([{"op":"transition","at":50,"type":"push_left","length":8}])");
    ProjectData d = saved();
    CHECK(d.timeline.video[0].clips[0].transOutStyle.type == TransitionType::PushLeft);
    CHECK(d.timeline.video[0].clips[0].transOutStyle.ease == MotionEase::Linear);
    edit(R"([{"op":"transition","at":50,"type":"whip_up","blur":70}])");
    d = saved();
    CHECK(d.timeline.video[0].clips[1].transInStyle.type == TransitionType::WhipUp);
    CHECK(d.timeline.video[0].clips[1].transInStyle.ease == MotionEase::InOut);
    CHECK_EQ(d.timeline.video[0].clips[1].transInStyle.blur, 70.0);
    edit(R"([{"op":"transition","at":50,"ease":"out"}])");
    CHECK(saved().timeline.video[0].clips[0].transOutStyle.ease == MotionEase::Out);
    CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", R"([{"op":"transition","at":50,"ease":"fast"}])"}}),
             QString("BAD_ARGUMENT"));

    // Counter: a title of its own kind with its settings
    edit(R"([{"op":"title","kind":"counter","from":0,"to":1000,"suffix":" kills","at":"1s","duration":"2s","ease":"linear"}])");
    d = saved();
    const Clip* counter = nullptr;
    const Clip* bar = nullptr;
    for (const Track& t : d.timeline.video)
        for (const Clip& c : t.clips) {
            if (c.isTitle() && c.title.isCounter()) counter = &c;
            if (c.isTitle() && c.title.isProgress()) bar = &c;
        }
    if (CHECK(counter)) {
        CHECK_EQ(counter->start, 25);
        CHECK_EQ(counter->length(), 50);
        CHECK_EQ(counter->title.countTo, 1000.0);
        CHECK_EQ(counter->title.suffix, QString(" kills"));
        CHECK(counter->title.countEase == MotionEase::Linear);
        CHECK(counter->title.text.isEmpty());
        CHECK(counter->title.size > 150); // sized for the 1920 high timeline
    }
    const int counterId = counter ? counter->id : 0;
    // Its separators are stored (the test process runs in English: a new one writes 1,000)
    if (counter) {
        CHECK(counter->title.numbers == NumberStyle::CommaPoint);
        edit(QString(R"([{"op":"title","clip":%1,"separators":"point_comma"}])").arg(counterId));
        bool stored = false;
        for (const Track& t : saved().timeline.video)
            for (const Clip& c : t.clips)
                if (c.id == counterId) stored = c.title.numbers == NumberStyle::PointComma;
        CHECK(stored);
        CHECK_EQ(errorOf("edit", {{"project", g_path},
                                  {"ops", QString(R"([{"op":"title","clip":%1,"separators":"dot"}])").arg(counterId)}}),
                 QString("BAD_ARGUMENT"));
        edit(QString(R"([{"op":"title","clip":%1,"separators":"comma_point"}])").arg(counterId));
    }
    // Progress bar without a place: the whole timeline, at the top
    edit(R"([{"op":"title","kind":"progress"}])");
    d = saved();
    for (const Track& t : d.timeline.video)
        for (const Clip& c : t.clips)
            if (c.isTitle() && c.title.isProgress()) bar = &c;
    if (CHECK(bar)) {
        const int barId = bar->id;
        CHECK_EQ(bar->start, 0);
        CHECK_EQ(bar->end(), 100);
        CHECK(bar->title.barTop);
        CHECK(bar->title.span == ProgressSpan::Timeline);
        CHECK_EQ(bar->title.barThickness, 16.0); // 1920 / 120
        // `info` tells what it is
        const QJsonObject info = run("info", {{"project", g_path}});
        CHECK(QJsonDocument(info).toJson().contains("\"kind\": \"progress\""));
        // At the top: no finding; at the bottom of the 9:16 timeline: covered by the apps
        const ProjectFormat v{1080, 1920, {25, 1}};
        auto progressIssue = [&](const Timeline& tl) {
            for (const auto& i : ExportCheck::layout(tl, 0, 100, v))
                if (i.clip == barId) return true;
            return false;
        };
        CHECK(!progressIssue(d.timeline));
        edit(QString(R"([{"op":"title","clip":%1,"edge":"bottom","span":"clip","thickness":20}])").arg(barId));
        d = saved();
        CHECK(progressIssue(d.timeline));
        edit(QString(R"([{"op":"title","clip":%1,"margin":500}])").arg(barId));
        CHECK(!progressIssue(saved().timeline));
    }
    // A counter low in the vertical frame: found by its widest value and named by its range
    if (counterId) {
        const int id = counterId;
        edit(QString(R"([{"op":"title","clip":%1,"y":-850}])").arg(id));
        bool named = false;
        for (const auto& i : ExportCheck::layout(saved().timeline, 0, 100, ProjectFormat{1080, 1920, {25, 1}}))
            if (i.clip == id && i.check == "safe_area") named = i.message.contains("1000 kills");
        CHECK(named);
    }
    CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", R"([{"op":"title","kind":"chart"}])"}}),
             QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", R"([{"op":"title","kind":"progress","edge":"left"}])"}}),
             QString("BAD_ARGUMENT"));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("generators");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    g_a = still(tmp.filePath("a.png"), kRed, kWhite, true);
    g_b = still(tmp.filePath("b.png"), kBlue, kGreen, false);
    Check::initEngine();
    std::setlocale(LC_NUMERIC, "C");
    testPush();
    testWipes();
    testWhip();
    testCounterValues();
    testCounterPicture();
    testProgress();
    testProgressDrop();
    testCounterInspector();
    testInspectorWheel();
    testCounterDecimals();
    testProjectFile(tmp.path());
    testCli(tmp.path());
    return Check::result();
}
