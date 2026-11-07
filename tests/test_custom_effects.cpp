// SPDX-License-Identifier: AGPL-3.0-only
// Custom effects (0.19): FFmpeg filter chain ("ffmpeg_filter") and shader ("shader").
// Filter: pixels change as the chain says (negate inverts, hue=s=0 is gray, fade follows the clip time), forbidden
// filters/options and broken chains are refused at edit time with the reason, the comp node "fx" runs it too, the
// project file keeps chain, shader code, values and keyframes. Shader: a compile error comes back with its line
// (needs qsb; the pixels on the card are tested by test_custom_shader).
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/ProjectFile.h"
#include "core/TimelineOps.h"
#include "engine/picture/CompNodes.h"
#include "engine/picture/CustomEffects.h"

#include "check.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <clocale>
#include <vector>

namespace PC = Picture::Custom;

namespace {

QString g_project;
bool g_quiet = false;

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        if (!g_quiet) std::printf("       unexpected error %s: %s\n", qPrintable(e.code), qPrintable(e.message));
        throw;
    }
}

QString messageOf(const QString& name, const QJsonObject& raw)
{
    g_quiet = true;
    try {
        run(name, raw);
    } catch (const Cli::Error& e) {
        g_quiet = false;
        return e.code + ": " + e.message;
    }
    g_quiet = false;
    return "none";
}

QJsonObject edit(const QJsonArray& ops) { return run("edit", {{"project", g_project}, {"ops", ops}}); }
QString editError(const QJsonArray& ops) { return messageOf("edit", {{"project", g_project}, {"ops", ops}}); }

ProjectData load()
{
    ProjectData d;
    ProjectFile::load(g_project, &d, nullptr);
    return d;
}

// A gradient picture (every channel different, opaque)
std::vector<uint8_t> gradient(int w, int h)
{
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &px[(size_t(y) * w + x) * 4];
            p[0] = uint8_t(x * 255 / std::max(1, w - 1));
            p[1] = uint8_t(y * 255 / std::max(1, h - 1));
            p[2] = uint8_t((x + y) & 255);
            p[3] = 255;
        }
    return px;
}

void filterOnPixels()
{
    const int w = 64, h = 48;
    // negate inverts every colour channel and keeps the alpha
    {
        const std::vector<uint8_t> src = gradient(w, h);
        std::vector<uint8_t> px = src;
        QString why;
        CHECK(PC::applyFilter("negate", px.data(), w, h, 0.0, &why));
        CHECK_EQ(why, QString());
        int wrong = 0;
        for (size_t i = 0; i < px.size(); i += 4)
            for (int c = 0; c < 4; ++c)
                if (px[i + c] != (c == 3 ? src[i + c] : 255 - src[i + c])) ++wrong;
        CHECK_EQ(wrong, 0);
    }
    // hue=s=0 makes it gray (r = g = b within rounding)
    {
        std::vector<uint8_t> px = gradient(w, h);
        CHECK(PC::applyFilter("hue=s=0", px.data(), w, h, 0.0));
        int colored = 0;
        for (size_t i = 0; i < px.size(); i += 4)
            if (std::abs(px[i] - px[i + 1]) > 3 || std::abs(px[i + 1] - px[i + 2]) > 3) ++colored;
        CHECK_EQ(colored, 0);
    }
    // Time: a fade in over the first second is black at t = 0 and the picture itself after it (the same cached
    // graph, asked for different times)
    {
        const std::vector<uint8_t> src = gradient(w, h);
        std::vector<uint8_t> a = src, b = src;
        CHECK(PC::applyFilter("fade=t=in:st=0:d=1", a.data(), w, h, 0.0));
        CHECK(PC::applyFilter("fade=t=in:st=0:d=1", b.data(), w, h, 2.0));
        int lit = 0, changed = 0;
        for (size_t i = 0; i < a.size(); i += 4) {
            if (a[i] > 2 || a[i + 1] > 2 || a[i + 2] > 2) ++lit;
            if (std::abs(b[i] - src[i]) > 2 || std::abs(b[i + 1] - src[i + 1]) > 2) ++changed;
        }
        CHECK_EQ(lit, 0);
        CHECK_EQ(changed, 0);
    }
    // A chain that changes the size is scaled back: the picture keeps its size
    {
        std::vector<uint8_t> px = gradient(w, h);
        CHECK(PC::applyFilter("crop=iw/2:ih/2:0:0", px.data(), w, h, 0.0));
        CHECK(px[size_t(h - 1) * w * 4 + (w - 1) * 4 + 0] < 140); // the top-left quarter fills the picture
    }
    // An empty chain changes nothing
    {
        const std::vector<uint8_t> src = gradient(w, h);
        std::vector<uint8_t> px = src;
        CHECK(PC::applyFilter("  ", px.data(), w, h, 0.0));
        CHECK(px == src);
    }
}

// Rough cost at 1920x1080 (printed, not checked: depends on the machine)
void filterTiming()
{
    const int w = 1920, h = 1080;
    std::vector<uint8_t> px = gradient(w, h);
    for (const char* chain : {"negate", "hue=s=0,edgedetect", "gblur=sigma=8"}) {
        PC::applyFilter(chain, px.data(), w, h, 0.0); // builds the graph
        QElapsedTimer t;
        t.start();
        for (int i = 0; i < 5; ++i) PC::applyFilter(chain, px.data(), w, h, i / 25.0);
        std::printf("filter %s 1920x1080: %.1f ms per frame\n", chain, t.nsecsElapsed() / 5e6);
    }
}

void filterChecks()
{
    auto refused = [](const char* chain, const char* expect) {
        const QString why = PC::checkFilter(QString::fromUtf8(chain));
        if (!CHECK(why.contains(QString::fromUtf8(expect), Qt::CaseInsensitive)))
            std::printf("       chain %s: %s\n", chain, qPrintable(why));
    };
    CHECK_EQ(PC::checkFilter("negate"), QString());
    CHECK_EQ(PC::checkFilter("hue=s=0,edgedetect"), QString());
    CHECK_EQ(PC::checkFilter("gblur=sigma=4,vignette"), QString());
    // Files, commands, other inputs: refused before any filter is made
    refused("movie=/etc/passwd", "not allowed");
    refused("negate,sendcmd=f=cmds.txt", "not allowed");
    refused("zmq", "not allowed");
    refused("subtitles=a.srt", "not allowed");
    refused("lut3d=file=a.cube", "not allowed");
    refused("curves=psfile=a.acv", "would use a file");
    refused("selectivecolor=psfile=a.asv", "would use a file");
    // Options by type and reviewed name: curves' plot writes a gnuplot file (a name without "file"), also when it
    // is reached by position; text options not reviewed and the generic ones other than enable are refused
    refused("curves=preset=vintage:plot=plot.txt", "would use a file");
    refused("curves=none:0/0:0/0:0/0:0/0:0/0:a.acv", "would use a file");
    refused("hue=s=0:threads=4", "no option 'threads'");
    refused("drawbox=box_source=x:enable=1,hue=s=0:extra_hw_frames=2", "no option 'extra_hw_frames'");
    // ... while the normal uses of curves and the other text options still work
    CHECK_EQ(PC::checkFilter("curves=preset=vintage"), QString());
    CHECK_EQ(PC::checkFilter("curves=vintage"), QString());
    CHECK_EQ(PC::checkFilter("curves=r='0/0 0.5/0.6 1/1':all='0/0.1 1/0.9'"), QString());
    CHECK_EQ(PC::checkFilter("hue=s=0:enable='between(t,0,1)'"), QString());
    CHECK_EQ(PC::checkFilter("drawbox=x=4:y=4:w=10:h=10:c=red@0.5:t=fill,rotate=a=PI/8:c=black"), QString());
    CHECK_EQ(PC::checkFilter("lutrgb=r=negval:g=val*0.5,convolution=0m='0 -1 0 -1 5 -1 0 -1 0'"), QString());
    CHECK_EQ(PC::checkFilter("scale=w=iw/2:h=-2:flags=bicubic,format=pix_fmts=gray,setsar=r=1"), QString());
    // Expressions that loop (an endless while() would hang the edit or a render thread for good) and pictures much
    // larger than the input are refused; a chain slower than its time budget too
    refused("geq=lum='while(1\\,1)'", "uses while()");
    refused("geq=lum='WHILE (1,1)':cb=128:cr=128", "uses while()");
    refused("geq=lum='st(0\\,X)+ld(0)':cb=128:cr=128", "uses st()");
    refused("hue=s=0:enable='while(1,1)'", "uses while()");
    refused("pad=w=16000:h=16000", "at most");
    refused("scale=w=iw*3:h=ih*3", "at most");
    CHECK_EQ(PC::checkFilter("transpose,pad=w=iw+20:h=ih+20"), QString());
    {
        QString slow = "negate";
        for (int i = 0; i < 40; ++i) slow += ",gblur=sigma=30:steps=6,geq=lum='taylor(sin(X),Y,9)':cb=128:cr=128";
        QElapsedTimer t;
        t.start();
        const QString why = PC::checkFilter(slow, {}, 1);
        if (!CHECK(why.contains("takes too long"))) std::printf("       %s\n", qPrintable(why));
        CHECK(t.elapsed() < 500); // the caller does not wait for the slow chain
        CHECK_EQ(PC::checkFilter(slow), QString()); // fine with the normal budget
    }
    // Tried at the picture's size: a fixed crop fits a 1080p clip but not the small fallback picture
    CHECK_EQ(PC::checkFilter("crop=1280:720", QSize(1920, 1080)), QString());
    CHECK(!PC::checkFilter("crop=1280:720").isEmpty());
    // Not one linear chain
    refused("negate;hue=s=0", "one linear chain");
    refused("[in]negate[out]", "labels");
    // Unknown filter or option, bad value
    refused("nosuchfilter", "no filter 'nosuchfilter'");
    refused("hue=bogus=1", "no option 'bogus'");
    CHECK(!PC::checkFilter("gblur=sigma=abc").isEmpty());
    // GPL-only filters are not on the list (the bundled FFmpeg has none)
    refused("boxblur=2", "not allowed");
    // A bad chain at render time: nothing drawn, no crash, the picture stays
    {
        std::vector<uint8_t> px = gradient(16, 16);
        const std::vector<uint8_t> src = px;
        QString why;
        CHECK(!PC::applyFilter("movie=x", px.data(), 16, 16, 0.0, &why));
        CHECK(!why.isEmpty());
        CHECK(px == src);
    }
}

void fxNode()
{
    Picture::Comp::Buf src(Picture::Comp::Buf::Rgba, 32, 18);
    const std::vector<uint8_t> g = gradient(32, 18);
    src.px = g;
    Picture::Comp::RunOptions opt;
    opt.size = QSize(32, 18);
    Picture::Comp::Buf out;
    CHECK(Picture::Comp::runNode("fx", {{"effect", "ffmpeg_filter"}, {"e_chain", "negate"}}, {{"src", src}}, opt, &out));
    if (CHECK_EQ(out.px.size(), g.size())) CHECK_EQ(int(out.px[4 * 5]), 255 - g[4 * 5]);
    // The fx node with a custom effect is never cached as static (the chain may use the time)
    const Picture::Comp::NodeType* k = Picture::Comp::find("fx");
    if (CHECK(k)) CHECK(Picture::Comp::varies(*k, {{"effect", "ffmpeg_filter"}}));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    std::setlocale(LC_NUMERIC, "C"); // as schneidi and schneidi-cli do: FFmpeg reads numbers with a dot
    Check::initApp("custom_effects");
    Check::initEngine();
    QTemporaryDir dir;

    CHECK(EffectRegistry::find("ffmpeg_filter") && EffectRegistry::find("shader"));
    const bool filters = PC::filterAvailable();
    if (filters) {
        filterOnPixels();
        filterChecks();
        filterTiming();
        fxNode();
    } else {
        std::printf("no libavfilter in this build: only the 'not available' answer is tested\n");
        CHECK(PC::checkFilter("negate").contains("not available"));
    }

    // Through the CLI: the effect on a green clip, the picture in `frames`, refusals with the reason
    const QString still = dir.filePath("green.png");
    {
        QImage img(320, 180, QImage::Format_RGB32);
        img.fill(QColor(0, 255, 0));
        CHECK(img.save(still));
    }
    g_project = dir.filePath("custom.schneidi");
    run("new", {{"project", g_project}, {"width", 320}, {"height", 180}, {"fps", 25}, {"media", QJsonArray{still}}});
    edit({QJsonObject{{"op", "append"}, {"media", still}, {"duration", "2s"}}});
    int clip = 0;
    {
        const ProjectData d = load();
        if (!CHECK(!d.timeline.video.isEmpty() && !d.timeline.video[0].clips.isEmpty())) return Check::result();
        clip = d.timeline.video[0].clips.first().id;
    }
    if (filters) {
        edit({QJsonObject{{"op", "effect"}, {"clips", QJsonArray{clip}}, {"effect", "ffmpeg_filter"},
                          {"params", QJsonObject{{"chain", "negate"}}}}});
        const QJsonObject r = run("frames", {{"source", g_project}, {"out", dir.filePath("f")}, {"width", 320}, {"at", QJsonArray{10}}});
        const QImage img(r["frames"].toArray().first().toObject()["file"].toString());
        if (CHECK(!img.isNull())) {
            const QColor c = img.pixelColor(160, 90);
            CHECK(c.red() > 250 && c.green() < 5 && c.blue() > 250); // green inverted = magenta
        }
        const QString bad = editError({QJsonObject{{"op", "effect"}, {"clips", QJsonArray{clip}}, {"effect", "ffmpeg_filter"},
                                                   {"params", QJsonObject{{"chain", "movie=/etc/passwd"}}}}});
        CHECK(bad.startsWith("BAD_ARGUMENT") && bad.contains("not allowed"));
        const QString badOpt = editError({QJsonObject{{"op", "effect"}, {"clips", QJsonArray{clip}},
                                                      {"effect", "ffmpeg_filter"},
                                                      {"params", QJsonObject{{"chain", "hue=bogus=2"}}}}});
        CHECK(badOpt.contains("no option 'bogus'"));
        // tried at the timeline's size (320x180): a fixed crop larger than the small fallback picture is fine
        CHECK_EQ(editError({QJsonObject{{"op", "effect"}, {"clips", QJsonArray{clip}}, {"effect", "ffmpeg_filter"},
                                        {"params", QJsonObject{{"chain", "crop=200:100"}}}}}),
                 QString("none"));
        edit({QJsonObject{{"op", "effect"}, {"clips", QJsonArray{clip}}, {"effect", "ffmpeg_filter"},
                          {"params", QJsonObject{{"chain", "negate"}}}}});
        // the refused chain did not replace the stored one
        const ProjectData d = load();
        const Clip* c = TimelineOps::findClip(d.timeline, clip);
        CHECK(c && EffectRegistry::value(*c, "ffmpeg_filter", "chain").toString() == "negate");
        // effects lists the allowed filters
        const QJsonObject e = run("effects", {{"effect", "ffmpeg_filter"}});
        CHECK(e["effects"].toArray().first().toObject()["allowed_filters"].toArray().contains(QJsonValue("negate")));
    }

    // Shader: the compile error comes back with the user's line; values and keyframes are stored
    QString noCompiler;
    const bool compiler = PC::shaderCompilerAvailable(&noCompiler);
    if (compiler) {
        CHECK_EQ(PC::checkShader("vec4 effect(vec2 uv) { return pixel(uv) * p1; }"), QString());
        const QString err = PC::checkShader("vec4 effect(vec2 uv)\n{\n    return pixel(uv) * undefinedThing;\n}");
        if (!CHECK(err.contains("undefinedThing") && err.contains("3"))) std::printf("       %s\n", qPrintable(err));
        CHECK(PC::checkShader("void main2() {}").contains("vec4 effect"));
        const QString bad = editError({QJsonObject{{"op", "effect"}, {"clips", QJsonArray{clip}}, {"effect", "shader"},
                                                   {"params", QJsonObject{{"code", "vec4 effect(vec2 uv) { return x; }"}}}}});
        CHECK(bad.startsWith("BAD_ARGUMENT") && bad.contains("shader.code"));
    } else {
        std::printf("no shader compiler (%s): compile checks skipped\n", qPrintable(noCompiler));
        CHECK(!PC::checkShader("vec4 effect(vec2 uv) { return pixel(uv); }").isEmpty());
    }
    const QString code = "vec4 effect(vec2 uv) { return mix(pixel(uv), color1, p1); }";
    QJsonObject params{{"p1", 0.25}, {"color1", "#ff0000"}};
    if (compiler) params["code"] = code;
    edit({QJsonObject{{"op", "effect"}, {"clips", QJsonArray{clip}}, {"effect", "shader"}, {"params", params}},
          QJsonObject{{"op", "keyframe"}, {"clips", QJsonArray{clip}}, {"param", "shader.p2"}, {"at", 0}, {"value", 1}},
          QJsonObject{{"op", "keyframe"}, {"clips", QJsonArray{clip}}, {"param", "shader.p2"}, {"at", 25}, {"value", 3}}});
    {
        // Round trip through the project file (edit saved it; load it again)
        const ProjectData d = load();
        const Clip* c = TimelineOps::findClip(d.timeline, clip);
        if (CHECK(c)) {
            if (filters) CHECK_EQ(EffectRegistry::value(*c, "ffmpeg_filter", "chain").toString(), QString("negate"));
            if (compiler) CHECK_EQ(EffectRegistry::value(*c, "shader", "code").toString(), code);
            CHECK_EQ(EffectRegistry::value(*c, "shader", "p1").toDouble(), 0.25);
            CHECK_EQ(EffectRegistry::value(*c, "shader", "color1").value<QColor>(), QColor(255, 0, 0));
            const AnimParam p2 = Keys::effectParam("shader", "p2");
            CHECK(Keys::animated(*c, p2));
            CHECK(std::abs(Keys::valueAt(*c, p2, 12.5) - 2.0) < 1e-6);
            // the shader comes after the filter (the clip's order)
            const QStringList order = EffectRegistry::order(*c);
            if (filters) CHECK(order.indexOf("ffmpeg_filter") >= 0 && order.indexOf("ffmpeg_filter") < order.indexOf("shader"));
        }
    }
    return Check::result();
}
