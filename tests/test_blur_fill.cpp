// Blurred fill around the fitted picture (ClipTransform::fill, op 'transform' fit:'blur', engine/BlurFill): the bars
// of a 16:9 clip in a 9:16 timeline show a blurred, enlarged copy of the same picture instead of the track below, on
// one track. Checks the pixel functions, renders through the TimelineBuilder in the preview (RGBA) and export (YUV)
// formats and at half size (crop limits the copy, zoom < 1 and rotation fill too, opacity makes the whole clip
// see-through, the track below is covered), live value changes, the project file (old files unchanged) and the CLI.

#include "check.h"

#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/ProjectFormat.h"
#include "engine/BlurFill.h"
#include "engine/LiveUpdate.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <clocale>
#include <cmath>
#include <functional>

namespace {

const ProjectFormat kTall{180, 320, {25, 1}}; // 9:16
const ProjectFormat kWide{320, 180, {25, 1}};
QString g_red, g_banded, g_green, g_greenWide; // banded: red with blue bands at the top and bottom (30 of 180 rows)

struct Rgb {
    int r = 0, g = 0, b = 0;
};

struct Picture {
    int w = 0, h = 0;
    mlt_image_format format = mlt_image_rgba;
    QVector<uint8_t> data;
    // RGBA: the pixel; YUV: luma only (in r, g, b)
    Rgb at(int x, int y) const
    {
        if (format == mlt_image_rgba) {
            const uint8_t* p = data.constData() + (size_t(y) * w + x) * 4;
            return {p[0], p[1], p[2]};
        }
        const int l = data[y * w + x];
        return {l, l, l};
    }
};

Picture render(const Timeline& tl, const ProjectFormat& fmt, mlt_image_format format, int frame, double scale = 1.0)
{
    auto profile = makeProfile(fmt);
    TimelineBuilder b(*profile);
    auto tractor = b.build(tl);
    tractor->seek(frame);
    std::unique_ptr<Mlt::Frame> f(tractor->get_frame());
    Picture pic;
    pic.w = int(fmt.width * scale);
    pic.h = int(fmt.height * scale);
    pic.format = format;
    mlt_image_format got = format;
    int w = pic.w, h = pic.h;
    f->set("consumer.rescale", "bilinear");
    const uint8_t* img = f->get_image(got, w, h);
    if (!CHECK(img && w == pic.w && h == pic.h && got == format)) return {};
    const int bytes = format == mlt_image_rgba ? w * h * 4 : w * h;
    pic.data = QVector<uint8_t>(img, img + bytes);
    return pic;
}

bool reddish(Rgb c) { return c.r > 120 && c.g < 90 && c.b < 90; }
bool greenish(Rgb c) { return c.g > 150 && c.r < 100; }
bool bluish(Rgb c) { return c.b > 120 && c.r < 120; }

int countGreen(const Picture& p)
{
    int n = 0;
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x) n += greenish(p.at(x, y));
    return n;
}

// The clip `media` on V2 over a green clip on V1
Timeline overGreen(const QString& media, const std::function<void(Clip&)>& setup, const QString& background = g_green)
{
    Timeline tl;
    tl.video.resize(2);
    Clip low;
    low.id = 1;
    low.mediaPath = background;
    low.out = 49;
    tl.video[0].clips << low;
    Clip up;
    up.id = 2;
    up.mediaPath = media;
    up.out = 49;
    setup(up);
    tl.video[1].clips << up;
    return tl;
}

void blurFill(Clip& c, double dim = 0)
{
    c.transform.fill = FitFill::Blur;
    c.transform.fillDim = dim;
}

void testPixels()
{
    // A uniform picture: the background has the frame's shape and the picture's color, darker by `dim`
    const int w = 320, h = 180;
    QVector<uint8_t> img(w * h * 4);
    for (int i = 0; i < w * h; ++i) {
        img[i * 4] = 200;
        img[i * 4 + 1] = 100;
        img[i * 4 + 2] = 50;
        img[i * 4 + 3] = 255;
    }
    const double none[4] = {0, 0, 0, 0};
    BlurFill::Background bg = BlurFill::makeBackground(img.constData(), w, h, 0, none, 9.0 / 16, 40, 0);
    CHECK_EQ(bg.height, 125); // sigma 2.5 px at blur 40 (2 % of the frame height)
    CHECK_EQ(bg.width, 70);
    CHECK(bg.opaque);
    bool uniform = true;
    for (int i = 0; i < bg.width * bg.height; ++i)
        uniform = uniform && bg.rgba[i * 4] == 200 && bg.rgba[i * 4 + 1] == 100 && bg.rgba[i * 4 + 2] == 50;
    if (!CHECK(uniform))
        for (int i = 0; i < 6; ++i)
            std::printf("       %d: %d %d %d %d\n", i, bg.rgba[i * 4], bg.rgba[i * 4 + 1], bg.rgba[i * 4 + 2], bg.rgba[i * 4 + 3]);
    const BlurFill::Background dark = BlurFill::makeBackground(img.constData(), w, h, 0, none, 9.0 / 16, 40, 50);
    CHECK(std::abs(dark.rgba[0] - 100) <= 1 && std::abs(dark.rgba[1] - 50) <= 1);
    // Stronger blur = fewer pixels; a wide frame keeps its shape
    CHECK(BlurFill::makeBackground(img.constData(), w, h, 0, none, 9.0 / 16, 100, 0).height < bg.height);
    const BlurFill::Background wide = BlurFill::makeBackground(img.constData(), w, h, 0, none, 16.0 / 9, 40, 0);
    CHECK(std::abs(double(wide.width) / wide.height - 16.0 / 9) < 0.05);

    // Composite: opaque picture pixels stay, transparent ones show the background, half ones mix
    QVector<uint8_t> fg(4 * 4 * 4, 0);
    for (int x = 0; x < 4; ++x) { // row 0 opaque white, row 1 half white, rows 2/3 transparent
        uint8_t* p = &fg[x * 4];
        p[0] = p[1] = p[2] = 255;
        p[3] = 255;
        uint8_t* q = &fg[(4 + x) * 4];
        q[0] = q[1] = q[2] = 255;
        q[3] = 128;
    }
    BlurFill::composite(fg.data(), 4, 4, bg);
    CHECK(fg[0] == 255 && fg[1] == 255 && fg[3] == 255);
    CHECK(std::abs(fg[16] - (255 + 200) / 2) <= 2 && std::abs(fg[17] - (255 + 100) / 2) <= 2 && fg[19] == 255);
    CHECK(fg[32] == 200 && fg[33] == 100 && fg[34] == 50 && fg[35] == 255);

    // A transparent source (keyed out) gives a transparent background: the track below still shows there
    QVector<uint8_t> clear(w * h * 4, 0);
    const BlurFill::Background see = BlurFill::makeBackground(clear.constData(), w, h, 0, none, 9.0 / 16, 40, 20);
    CHECK(!see.opaque);
    QVector<uint8_t> px(4, 0);
    BlurFill::composite(px.data(), 1, 1, see);
    CHECK_EQ(int(px[3]), 0);

    // The crop limits the copy: the blue band at the top is not in it
    QVector<uint8_t> banded(w * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &banded[(y * w + x) * 4];
            const bool band = y < 30 || y >= 150;
            p[0] = band ? 0 : 255;
            p[1] = 0;
            p[2] = band ? 255 : 0;
            p[3] = 255;
        }
    const BlurFill::Background top = BlurFill::makeBackground(banded.constData(), w, h, 0, none, 9.0 / 16, 10, 0);
    CHECK(top.rgba[2] > 200 && top.rgba[0] < 50); // first pixel: blue band
    const double crop[4] = {0, 30.0 / 180, 0, 30.0 / 180};
    const BlurFill::Background cut = BlurFill::makeBackground(banded.constData(), w, h, 0, crop, 9.0 / 16, 10, 0);
    CHECK(cut.rgba[0] > 200 && cut.rgba[2] < 50);
}

void testRender()
{
    for (mlt_image_format f : {mlt_image_rgba, mlt_image_yuv420p}) {
        const bool rgba = f == mlt_image_rgba;
        // Without: the bars are transparent, the green track shows (as before)
        const Picture plain = render(overGreen(g_red, [](Clip&) {}), kTall, f, 5);
        if (rgba) CHECK(greenish(plain.at(90, 20)));
        // With: red bars from the clip itself, the picture unchanged, nothing of the green track left
        const Picture filled = render(overGreen(g_red, [](Clip& c) { blurFill(c); }), kTall, f, 5);
        if (rgba) {
            CHECK(reddish(filled.at(90, 20)));
            CHECK(reddish(filled.at(90, 300)));
            CHECK(reddish(filled.at(90, 160)));
        } else { // luma of red (BT.709 limited) ~63, green ~173
            CHECK(std::abs(filled.at(90, 20).r - filled.at(90, 160).r) < 12);
        }
        CHECK_EQ(countGreen(filled), 0);
        // Darker by half
        const Picture dim = render(overGreen(g_red, [](Clip& c) { blurFill(c, 50); }), kTall, f, 5);
        if (rgba) {
            const Rgb bar = dim.at(90, 20), pic = dim.at(90, 160);
            CHECK(std::abs(bar.r - pic.r / 2) < 20);
        } else {
            CHECK(dim.at(90, 20).r < dim.at(90, 160).r - 10);
        }
    }
    // The section switch turns it off with the transform
    CHECK(greenish(render(overGreen(g_red, [](Clip& c) {
                              blurFill(c);
                              c.transform.transformOn = false;
                          }),
                          kTall, mlt_image_rgba, 5)
                       .at(90, 20)));
    // Opacity acts on the whole clip: the green shows through the bars and the picture alike
    {
        const Picture half = render(overGreen(g_red, [](Clip& c) {
                                        blurFill(c);
                                        c.transform.opacity = 50;
                                    }),
                                    kTall, mlt_image_rgba, 5);
        const Rgb bar = half.at(90, 20), pic = half.at(90, 160);
        CHECK(bar.g > 60 && bar.r > 60);
        CHECK(std::abs(bar.g - pic.g) < 25 && std::abs(bar.r - pic.r) < 25);
    }
    // Crop: the copy leaves out what is cropped (blue bands of the source), at every size
    for (double scale : {1.0, 0.5}) {
        const Picture banded = render(overGreen(g_banded, [](Clip& c) { blurFill(c); }), kTall, mlt_image_rgba, 5, scale);
        CHECK(bluish(banded.at(int(90 * scale), int(4 * scale))));
        const double band = 30 * 180.0 / 320; // 30 source rows in timeline pixels on the fitted picture
        const Picture cropped = render(overGreen(g_banded, [&](Clip& c) {
                                           blurFill(c);
                                           c.transform.cropTop = c.transform.cropBottom = band;
                                       }),
                                       kTall, mlt_image_rgba, 5, scale);
        CHECK(reddish(cropped.at(int(90 * scale), int(4 * scale))));
        CHECK_EQ(countGreen(cropped), 0);
    }
    // Preview size = export size scaled: the same colors at the same places
    {
        const Picture full = render(overGreen(g_banded, [](Clip& c) { blurFill(c, 20); }), kTall, mlt_image_rgba, 5);
        const Picture half = render(overGreen(g_banded, [](Clip& c) { blurFill(c, 20); }), kTall, mlt_image_rgba, 5, 0.5);
        int worst = 0;
        for (int y : {6, 40, 80, 240, 280, 314})
            for (int x : {10, 90, 170}) {
                const Rgb a = full.at(x, y), b = half.at(x / 2, y / 2);
                worst = std::max({worst, std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
            }
        if (!CHECK(worst < 24)) std::printf("       preview differs by %d\n", worst);
    }
    // Room left by zoom < 1 and rotation fills too (a 16:9 clip in a 16:9 timeline)
    {
        const Picture small = render(overGreen(g_red, [](Clip& c) {
                                         blurFill(c);
                                         c.transform.zoomX = c.transform.zoomY = 0.5;
                                     }, g_greenWide),
                                     kWide, mlt_image_rgba, 5);
        CHECK(reddish(small.at(5, 5)));
        CHECK_EQ(countGreen(small), 0);
        const Picture turned = render(overGreen(g_red, [](Clip& c) {
                                          blurFill(c);
                                          c.transform.rotation = 20;
                                      }, g_greenWide),
                                      kWide, mlt_image_rgba, 5);
        CHECK(reddish(turned.at(2, 2)));
        CHECK_EQ(countGreen(turned), 0);
    }
}

void testLiveUpdate()
{
    // Blur and darkening are values: the running preview takes them without a rebuild; switching the fill on is not
    auto profile = makeProfile(kTall);
    const Timeline base = overGreen(g_banded, [](Clip& c) { blurFill(c, 0); });
    Timeline changed = base;
    changed.video[1].clips[0].transform.fillDim = 80;
    changed.video[1].clips[0].transform.fillBlur = 90;
    CHECK(LiveUpdate::valuesOnly(base, changed));
    TimelineBuilder builder(*profile);
    auto live = builder.build(base);
    auto fresh = builder.build(changed);
    CHECK(LiveUpdate::transfer(*live, *fresh));
    live->seek(5);
    std::unique_ptr<Mlt::Frame> f(live->get_frame());
    mlt_image_format fmt = mlt_image_rgba;
    int w = kTall.width, h = kTall.height;
    const uint8_t* img = f->get_image(fmt, w, h);
    if (CHECK(img)) CHECK(img[(10 * w + 90) * 4] + img[(10 * w + 90) * 4 + 2] < 80); // 80 % darker
    const Timeline off = overGreen(g_banded, [](Clip&) {});
    auto plain = builder.build(off);
    auto with = builder.build(base);
    CHECK(!LiveUpdate::transfer(*plain, *with));
}

void testProjectFile(const QString& dir)
{
    Clip c;
    c.id = 1;
    c.mediaPath = "/x/a.mp4";
    c.out = 9;
    CHECK(!ProjectFile::clipJson(c).contains("transform")); // untouched clips stay without
    c.transform.zoomX = 1.5;
    const QJsonObject zoomOnly = ProjectFile::clipJson(c).value("transform").toObject();
    CHECK(!zoomOnly.contains("fill") && !zoomOnly.contains("fillBlur") && !zoomOnly.contains("fillDim"));
    c.transform.zoomX = 1;
    c.transform.fill = FitFill::Blur;
    c.transform.fillBlur = 55;
    c.transform.fillDim = 10;
    const QJsonObject t = ProjectFile::clipJson(c).value("transform").toObject();
    CHECK_EQ(t.value("fill").toString(), QString("blur"));
    CHECK_EQ(t.value("fillBlur").toDouble(), 55.0);

    ProjectData d;
    d.timeline.video.resize(1);
    d.timeline.audio.resize(1);
    d.timeline.video[0].clips << c;
    QString error;
    const QString path = dir + "/fill.schneidi";
    CHECK(ProjectFile::save(d, path, &error));
    ProjectData back;
    CHECK(ProjectFile::load(path, &back, &error));
    if (CHECK(back.timeline.video.size() == 1 && back.timeline.video[0].clips.size() == 1))
        CHECK(back.timeline.video[0].clips[0].transform == c.transform);

    // Undo: one step, back to transparent
    Project p;
    p.load(d);
    Selection sel;
    Editor ed(&p, &sel);
    ed.modifyClips({1}, "fill", [](Clip& x) { x.transform.fill = FitFill::None; });
    CHECK(p.timeline().video[0].clips[0].transform.fill == FitFill::None);
    p.undoStack()->undo();
    CHECK(p.timeline().video[0].clips[0].transform.fill == FitFill::Blur);
}

// op 'transform' fit/fit_blur/fit_dim, `info`, errors
void testCli(const QString& dir)
{
    const QString path = dir + "/cli.schneidi";
    Project p;
    ProjectData d;
    d.timeline = emptyTimeline();
    p.load(d);
    p.addMedia({"/x/a.mp4", "a.mp4", 100, true, false, false});
    p.edit("setup", [&](Timeline& tl) {
        Clip v;
        v.id = p.newClipId();
        v.mediaPath = "/x/a.mp4";
        v.out = 49;
        tl.video[0].clips << v;
        Clip t;
        t.id = p.newClipId();
        t.kind = ClipKind::Title;
        t.start = 50;
        t.out = 24;
        tl.video[0].clips << t;
    });
    QString error;
    CHECK(ProjectFile::save(p.data(), path, &error));
    const int clip = p.timeline().video[0].clips[0].id, title = p.timeline().video[0].clips[1].id;
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
    run("edit", {{"project", path},
                 {"ops", QString(R"([{"op":"transform","clips":[%1,%2],"fit":"blur","fit_dim":35,"zoom":1.3}])")
                             .arg(clip)
                             .arg(title)}});
    const QJsonObject info = run("info", {{"project", path}});
    const QJsonArray clips = info["timeline"]["tracks"][0]["clips"].toArray();
    CHECK_EQ(clips[0]["transform"]["fit"].toString(), QString("blur"));
    CHECK_EQ(clips[0]["transform"]["fit_dim"].toDouble(), 35.0);
    CHECK_EQ(clips[0]["transform"]["fit_blur"].toDouble(), 70.0);
    CHECK(!clips[1]["transform"].toObject().contains("fit")); // titles fill the frame
    CHECK_EQ(errorOf(QString(R"([{"op":"transform","clips":[%1],"fit":"smear"}])").arg(clip)), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf(QString(R"([{"op":"transform","clips":[%1],"fit_blur":300}])").arg(clip)), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf(QString(R"([{"op":"transform","clips":[%1],"fit":"blur"}])").arg(title)), QString("BAD_ARGUMENT"));
    run("edit", {{"project", path}, {"ops", QString(R"([{"op":"transform","clips":[%1],"fit":"none"}])").arg(clip)}});
    const QJsonObject t = run("info", {{"project", path}})["timeline"]["tracks"][0]["clips"][0]["transform"].toObject();
    CHECK(!t.contains("fit"));
    CHECK_EQ(t.value("fit_dim").toDouble(), 35.0); // kept for the next time it is switched on
    // The op help names it, so a model finds it
    bool helped = false;
    for (const QJsonValue& v : Cli::detail::editOpsHelp())
        if (v["op"].toString() == "transform") helped = v["details"].toString().contains("fit:'blur'");
    CHECK(helped);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("blur_fill");
    testPixels();
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    testProjectFile(tmp.path());
    testCli(tmp.path());
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    const QStringList h264{"-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"};
    g_red = Check::makeMedia(tmp.filePath("red.mp4"), QStringList{"-f", "lavfi", "-i", "color=c=red:s=320x180:r=25:d=2"} + h264);
    g_banded = Check::makeMedia(tmp.filePath("banded.mp4"),
                                QStringList{"-f", "lavfi", "-i",
                                            "color=c=red:s=320x180:r=25:d=2,drawbox=x=0:y=0:w=320:h=30:color=blue:t=fill,"
                                            "drawbox=x=0:y=150:w=320:h=30:color=blue:t=fill"} + h264);
    g_green = Check::makeMedia(tmp.filePath("green.mp4"), QStringList{"-f", "lavfi", "-i", "color=c=0x00ff00:s=180x320:r=25:d=2"} + h264);
    g_greenWide = Check::makeMedia(tmp.filePath("greenwide.mp4"),
                                   QStringList{"-f", "lavfi", "-i", "color=c=0x00ff00:s=320x180:r=25:d=2"} + h264);
    if (!CHECK(!g_red.isEmpty() && !g_banded.isEmpty() && !g_green.isEmpty() && !g_greenWide.isEmpty()))
        return Check::result();
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    testRender();
    testLiveUpdate();
    return Check::result();
}
