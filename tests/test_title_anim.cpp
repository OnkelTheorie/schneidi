// SPDX-License-Identifier: AGPL-3.0-only
// Title entry/exit animation (0.19, TitleStyle::enter/exit): pop (60 % -> 108 % -> 100 % of the size over 0.2 s),
// fade, slide up; the exit backwards at the clip's end; frames outside the animation bit-identical to a title without
// one; a transformed title (drawn as its own part) the same; project file round trip (unset = not written, unknown
// values ignored) and the CLI op `title` options enter/exit.
// SCHNEIDI_PICTURE_DUMP=<folder>: saves the rendered pictures there to look at.
#include "render_check.h"

#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/Generators.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/ProjectFormat.h"

#include <QApplication>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <clocale>
#include <cmath>
#include <functional>

#define CHECK_NEAR(a, b, tol) CHECK(std::abs(double(a) - double(b)) <= (tol))

namespace {

const ProjectFormat kFormat{640, 360, {25, 1}}; // 0.2 s = 5 frames

Clip titleClip(const TitleStyle& st)
{
    Clip c;
    c.id = 1;
    c.kind = ClipKind::Title;
    c.out = 24; // 25 frames (out inclusive)
    c.title = st;
    return c;
}

QImage render(const Clip& c, int frame, const QString& name)
{
    Timeline tl;
    tl.video.resize(1);
    tl.video[0].clips << c;
    const QImage img = Check::renderFrame(tl, kFormat, frame).convertToFormat(QImage::Format_RGB32);
    if (const QString dump = qEnvironmentVariable("SCHNEIDI_PICTURE_DUMP"); !dump.isEmpty() && !img.isNull())
        img.save(QString("%1/title-anim-%2-%3.png").arg(dump, name).arg(frame));
    return img;
}

TitleStyle base()
{
    TitleStyle st;
    st.text = "HH";
    st.size = 140;
    st.bold = true;
    st.color = Qt::white;
    return st;
}

int brightest(const QImage& img)
{
    int m = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) m = std::max(m, qGreen(img.pixel(x, y)));
    return m;
}

QRect boxOf(const QImage& img, int threshold = 40)
{
    QRect r;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (qGreen(img.pixel(x, y)) > threshold) r |= QRect(x, y, 1, 1);
    return r;
}

void testMotionCurve()
{
    std::printf("motion curve\n");
    TitleStyle st = base();
    CHECK(Generators::titleMotion(st, 0, 25, 25).still());
    st.enter = TitleAnim::Pop;
    CHECK_NEAR(Generators::titleMotion(st, 0, 25, 25).scale, 0.6, 1e-9);
    CHECK_NEAR(Generators::titleMotion(st, 3, 25, 25).scale, 1.08, 1e-9); // 60 % of 5 frames
    CHECK(Generators::titleMotion(st, 5, 25, 25).still());
    st.enter = TitleAnim::None;
    st.exit = TitleAnim::Pop;
    CHECK_NEAR(Generators::titleMotion(st, 24, 25, 25).scale, 0.6, 1e-9); // the last frame
    CHECK(Generators::titleMotion(st, 19, 25, 25).still());
    st.exit = TitleAnim::Fade;
    st.exitLen = 0.4; // 10 frames
    CHECK_NEAR(Generators::titleMotion(st, 19, 25, 25).opacity, 0.5, 1e-9);
}

void testPop()
{
    std::printf("pop\n");
    TitleStyle st = base();
    const QImage still = render(titleClip(st), 0, "still");
    const QRect full = boxOf(still);
    if (!CHECK(full.width() > 100)) return;
    st.enter = TitleAnim::Pop;
    const Clip c = titleClip(st);
    const QRect start = boxOf(render(c, 0, "pop")), peak = boxOf(render(c, 3, "pop")), end = boxOf(render(c, 5, "pop"));
    std::printf("       widths: still %d, t=0 %d, t=3 %d, t=5 %d\n", full.width(), start.width(), peak.width(), end.width());
    CHECK_NEAR(start.width() / double(full.width()), 0.6, 0.06);
    CHECK_NEAR(peak.width() / double(full.width()), 1.08, 0.05);
    CHECK(peak.width() > full.width());
    CHECK_NEAR(start.center().x(), full.center().x(), 3); // around the title's centre
    CHECK_NEAR(start.center().y(), full.center().y(), 3);
    // After the animation: exactly the title without one
    CHECK(render(c, 5, "pop") == still);
    CHECK(render(c, 20, "pop") == render(titleClip(base()), 20, "still"));
    // Exit pop: small on the last frame
    TitleStyle out = base();
    out.exit = TitleAnim::Pop;
    const int popOut = boxOf(render(titleClip(out), 24, "popout")).width();
    std::printf("       exit pop, last frame: %d\n", popOut);
    CHECK_NEAR(popOut / double(full.width()), 0.6, 0.06);
}

void testFadeSlide()
{
    std::printf("fade, slide up\n");
    TitleStyle st = base();
    const QImage still = render(titleClip(st), 0, "still");
    const QRect full = boxOf(still);
    st.enter = TitleAnim::Fade;
    const Clip c = titleClip(st);
    CHECK_EQ(brightest(render(c, 0, "fade")), 0); // nothing yet
    const int mid = brightest(render(c, 2, "fade"));    // 40 %
    std::printf("       fade at 40 %%: %d\n", mid);
    CHECK_NEAR(mid, 102, 12);
    CHECK(render(c, 5, "fade") == still);
    // Slide up: lower and fainter at first, then in place
    st.enter = TitleAnim::SlideUp;
    const QImage s2 = render(titleClip(st), 2, "slide");
    const QRect box = boxOf(s2, 20);
    // rise = 0.5 * 140 * (1 - ease_out(0.4)) = 15.1 pixels below its place
    std::printf("       slide at 40 %%: %d px lower, brightest %d\n", box.center().y() - full.center().y(), brightest(s2));
    CHECK_NEAR(box.center().y() - full.center().y(), 15, 3);
    CHECK_NEAR(brightest(s2), 102, 12);
    CHECK(render(titleClip(st), 5, "slide") == still);
    // Leaving: rises on
    TitleStyle out = base();
    out.exit = TitleAnim::SlideUp;
    out.exitLen = 0.4;
    const QRect up = boxOf(render(titleClip(out), 22, "slideout"), 20);
    CHECK(up.center().y() < full.center().y() - 10);
}

void testTransformedAndOutlined()
{
    std::printf("transformed, outlined\n");
    // A title with a transform is drawn only as large as its text (its own part): the animation the same way
    TitleStyle st = base();
    st.outlineOn = true;
    st.outlineWidth = 6;
    st.outlineColor = Qt::red;
    st.boxOn = true;
    Clip c = titleClip(st);
    c.transform.posX = 40;
    const QRect full = boxOf(render(c, 10, "moved"));
    c.title.enter = TitleAnim::Pop;
    c.title.exit = TitleAnim::Fade;
    const QRect start = boxOf(render(c, 0, "moved"));
    CHECK_NEAR(start.width() / double(full.width()), 0.6, 0.08);
    CHECK_NEAR(start.center().x(), full.center().x(), 3);
    const QImage fading = render(c, 22, "moved"); // 2 frames after it of 5: 40 %
    std::printf("       fading out, 2 of 5 frames left: brightest %d\n", brightest(fading));
    CHECK_NEAR(brightest(fading), 102, 14);
    CHECK(boxOf(fading, 5).width() >= full.width() - 2);
}

void testProjectFile(const QString& dir)
{
    std::printf("project file\n");
    Project p;
    ProjectData d;
    d.format = kFormat;
    d.timeline.video.resize(1);
    p.load(d);
    p.edit("setup", [&](Timeline& tl) {
        Clip c = titleClip(base());
        c.id = p.newClipId();
        c.title.enter = TitleAnim::Pop;
        c.title.enterLen = 0.35;
        c.title.exit = TitleAnim::SlideUp;
        tl.video[0].clips << c;
    });
    const QString path = dir + "/t.schneidi";
    QString error;
    CHECK(ProjectFile::save(p.data(), path, &error));
    ProjectData back;
    CHECK(ProjectFile::load(path, &back, &error));
    CHECK(back.timeline == p.timeline());
    const QJsonObject title = ProjectFile::clipJson(p.timeline().video[0].clips[0]).value("title").toObject();
    CHECK_EQ(title.value("enter").toString(), QString("pop"));
    CHECK_EQ(title.value("exit").toString(), QString("slide_up"));
    CHECK(!title.contains("exitLen")); // the default
    // Without an animation: no new keys
    const QJsonObject plain = ProjectFile::clipJson(titleClip(base())).value("title").toObject();
    for (const char* key : {"enter", "exit", "enterLen", "exitLen"}) CHECK(!plain.contains(key));
    // An animation a newer version knows: none
    QFile f(path);
    CHECK(f.open(QIODevice::ReadOnly));
    QJsonValue doc = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    std::function<void(QJsonValue&)> spoil = [&](QJsonValue& v) {
        if (v.isArray()) {
            QJsonArray a = v.toArray();
            for (auto it = a.begin(); it != a.end(); ++it) {
                QJsonValue x = *it;
                spoil(x);
                *it = x;
            }
            v = a;
        } else if (v.isObject()) {
            QJsonObject o = v.toObject();
            if (o.contains("enter")) o["enter"] = "spin";
            for (auto it = o.begin(); it != o.end(); ++it) {
                QJsonValue x = *it;
                spoil(x);
                *it = x;
            }
            v = o;
        }
    };
    spoil(doc);
    ProjectData spoiled;
    if (CHECK(ProjectFile::fromJson(QJsonDocument(doc.toObject()).toJson(), path, &spoiled, &error))
        && CHECK(!spoiled.timeline.video.isEmpty() && !spoiled.timeline.video[0].clips.isEmpty())) {
        CHECK(spoiled.timeline.video[0].clips[0].title.enter == TitleAnim::None);
        CHECK(spoiled.timeline.video[0].clips[0].title.exit == TitleAnim::SlideUp);
    }
}

QString g_path;

QJsonObject run(const QString& name, QJsonObject raw)
{
    if (name == "edit") raw["full"] = true;
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    return cmd->run(Cli::normalize(*cmd, raw), ctx);
}

QString errorOf(const QString& ops)
{
    try {
        run("edit", {{"project", g_path}, {"ops", ops}});
    } catch (const Cli::Error& e) {
        return e.code;
    }
    return "none";
}

TitleStyle savedTitle()
{
    ProjectData d;
    QString error;
    ProjectFile::load(g_path, &d, &error);
    for (const Track& t : d.timeline.video)
        for (const Clip& c : t.clips)
            if (c.isTitle()) return c.title;
    return {};
}

int savedTitleId()
{
    ProjectData d;
    QString error;
    ProjectFile::load(g_path, &d, &error);
    for (const Track& t : d.timeline.video)
        for (const Clip& c : t.clips)
            if (c.isTitle()) return c.id;
    return 0;
}

void testCli(const QString& dir)
{
    std::printf("cli\n");
    g_path = dir + "/cli.schneidi";
    {
        Project p;
        ProjectData d;
        d.format = kFormat;
        d.timeline.video.resize(1);
        p.load(d);
        QString error;
        CHECK(ProjectFile::save(p.data(), g_path, &error));
    }
    run("edit", {{"project", g_path}, {"ops", R"([{"op":"title","text":"GG","enter":"pop"}])"}});
    TitleStyle t = savedTitle();
    CHECK(t.enter == TitleAnim::Pop);
    CHECK_NEAR(t.enterLen, 0.2, 1e-9);
    CHECK(t.exit == TitleAnim::None);
    const int id = savedTitleId();
    run("edit", {{"project", g_path},
                 {"ops", QString(R"([{"op":"title","clip":%1,"enter":{"duration":"0.4s"},"exit":{"type":"slide_up","duration":5}}])").arg(id)}});
    t = savedTitle();
    CHECK(t.enter == TitleAnim::Pop); // a duration alone keeps the type
    CHECK_NEAR(t.enterLen, 0.4, 1e-9);
    CHECK(t.exit == TitleAnim::SlideUp);
    CHECK_NEAR(t.exitLen, 0.2, 1e-9); // 5 frames at 25 fps
    // `info` shows it in the op's form
    const QByteArray info = QJsonDocument(run("info", {{"project", g_path}})).toJson(QJsonDocument::Compact);
    CHECK(info.contains(R"("enter":{"duration":"0.4s","type":"pop"})"));
    run("edit", {{"project", g_path}, {"ops", QString(R"([{"op":"title","clip":%1,"enter":"none"}])").arg(id)}});
    CHECK(savedTitle().enter == TitleAnim::None);
    CHECK_EQ(errorOf(QString(R"([{"op":"title","clip":%1,"enter":"spin"}])").arg(id)), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf(QString(R"([{"op":"title","clip":%1,"enter":{"duration":"1s"}}])").arg(id)), QString("BAD_ARGUMENT"));
    const QString help = QJsonDocument(run("help", {{"command", "edit"}, {"op", "title"}})).toJson();
    CHECK(help.contains("slide_up"));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("title_anim");
    I18n::setLanguage("en");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    Check::initEngine();
    std::setlocale(LC_NUMERIC, "C");
    testMotionCurve();
    testPop();
    testFadeSlide();
    testTransformedAndOutlined();
    testProjectFile(tmp.path());
    testCli(tmp.path());
    return Check::result();
}
