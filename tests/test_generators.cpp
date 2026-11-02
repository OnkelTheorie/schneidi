// SPDX-License-Identifier: AGPL-3.0-only
// Shorts building blocks: push/whip transitions and eased wipes (pixels at 0/50/100 % per direction), the animated
// counter (value and text at given frames, easing, decimals, German vs English separators, drawn like a text title
// with that text), the progress bar (filled width at given frames in both spans), project file round trip, the CLI
// ops and `check` on a progress bar at the bottom of a vertical timeline.
// SCHNEIDI_PICTURE_DUMP=<folder>: saves the rendered pictures there to look at.
#include "check.h"

#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/ExportCheck.h"
#include "core/Generators.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/ProjectFormat.h"
#include "core/TimelineOps.h"

#include <QApplication>
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
    I18n::install("en");
    CHECK_EQ(Generators::counterText(s, 100, 101), QString("$2,500.0 k"));
    CHECK_EQ(Generators::counterText(s, 50, 101), QString("$1,250.0 k"));
    I18n::install("de");
    CHECK_EQ(Generators::counterText(s, 100, 101), QString("$2.500,0 k"));
    CHECK_EQ(Generators::counterText(s, 0, 101), QString("$0,0 k"));
    s.grouping = false;
    CHECK_EQ(Generators::counterText(s, 100, 101), QString("$2500,0 k"));
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
    // Progress bar without a place: the whole timeline, at the top
    edit(R"([{"op":"title","kind":"progress"}])");
    d = saved();
    for (const Track& t : d.timeline.video)
        for (const Clip& c : t.clips)
            if (c.isTitle() && c.title.isProgress()) bar = &c;
    if (CHECK(bar)) {
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
                if (i.clip == bar->id) return true;
            return false;
        };
        CHECK(!progressIssue(d.timeline));
        edit(QString(R"([{"op":"title","clip":%1,"edge":"bottom","span":"clip","thickness":20}])").arg(bar->id));
        d = saved();
        CHECK(progressIssue(d.timeline));
        edit(QString(R"([{"op":"title","clip":%1,"margin":500}])").arg(bar->id));
        CHECK(!progressIssue(saved().timeline));
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
    testProjectFile(tmp.path());
    testCli(tmp.path());
    return Check::result();
}
