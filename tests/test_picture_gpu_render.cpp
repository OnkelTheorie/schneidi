// SPDX-License-Identifier: AGPL-3.0-only
// The picture engine's GPU path (Picture::Renderer with Options::gpu) against its CPU path, whole frames of timelines:
// plain clips, transforms/crop/opacity, colour grade (with LUT), cross dissolve, luma and plain wipes, dip to colour,
// stills, and what the card leaves to the CPU per layer (titles, blur, blur fill, wipe border, subtitles). Tolerance
// instead of bit equality. Also the encoder planes written by the card (RenderHints::yuv) against
// OutputColor::fromRgba of the CPU frame. Skips (77) without a hardware GPU device (headless, software rasterizer).
// PICTURE_GPU_RENDER_BENCH=n: time n export frames of 3 tracks 1080p + grade, CPU vs GPU.

#include "check.h"

#include "core/CompGraph.h"
#include "core/EffectRegistry.h"
#include "engine/OutputColor.h"
#include "engine/picture/Renderer.h"
#include "engine/picture/gpu/Gpu.h"

#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QTemporaryDir>

#include <clocale>
#include <cmath>
#include <ctime>
#include <functional>
#include <vector>

namespace {

const ProjectFormat kFmt{1280, 720, {30, 1}};
QString g_a, g_b, g_c, g_still;

struct Diff {
    double mean = 0, psnr = 99;
    int max = 0;
    double bad = 0; // percent of values off by more than 24 (sharp chroma edges: the CPU's unscaled conversion takes the
                    // nearest chroma sample, the card interpolates; a two-pixel seam at every colour edge of test patterns)
};

Diff compare(const uint8_t* a, const uint8_t* b, size_t n)
{
    Diff d;
    double sum = 0, sq = 0;
    size_t bad = 0;
    for (size_t i = 0; i < n; ++i) {
        const int e = std::abs(int(a[i]) - int(b[i]));
        sum += e;
        sq += double(e) * e;
        d.max = std::max(d.max, e);
        bad += e > 24;
    }
    d.bad = n ? 100.0 * bad / n : 0;
    d.mean = n ? sum / n : 0;
    const double mse = n ? sq / n : 0;
    d.psnr = mse > 0 ? 10 * std::log10(255.0 * 255.0 / mse) : 99;
    return d;
}

Clip media(int id, const QString& path, int start, int length, int in = 0)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.in = in;
    c.out = in + length - 1;
    return c;
}

Picture::Options options(Picture::Mode mode, bool gpu, QSize out = {})
{
    Picture::Options o;
    o.format = kFmt;
    o.mode = mode;
    o.gpu = gpu;
    o.output = out;
    return o;
}

// GPU vs CPU of frames `at`; limits on mean and PSNR (RGBA)
void same(const char* name, const Timeline& tl, std::initializer_list<int> at, double maxMean, double minPsnr,
          Picture::Mode mode = Picture::Mode::Export, QSize out = {})
{
    Picture::Renderer cpu(tl, options(mode, false, out)), gpu(tl, options(mode, true, out));
    for (int f : at) {
        const Picture::Frame a = cpu.render(f), b = gpu.render(f);
        if (!CHECK(!a.isNull() && !b.isNull())) continue;
        CHECK(b.gpu());
        CHECK(!a.gpu());
        if (!CHECK(a.width() == b.width() && a.height() == b.height())) continue;
        const Diff d = compare(a.bits(), b.bits(), size_t(a.width()) * a.height() * 4);
        std::printf("  %-28s f%-4d mean %.3f max %3d PSNR %.1f dB, %.2f %% > 24\n", name, f, d.mean, d.max, d.psnr, d.bad);
        if (!CHECK(d.mean <= maxMean && d.psnr >= minPsnr && d.bad <= 1.5)) {
            // For a look at the difference: both pictures in the temp folder
            a.toImage().save(QDir::temp().filePath(QString("picture-gpu-%1-%2-cpu.png").arg(name).arg(f)));
            b.toImage().save(QDir::temp().filePath(QString("picture-gpu-%1-%2-gpu.png").arg(name).arg(f)));
        }
    }
}

Timeline oneTrack(std::initializer_list<Clip> clips)
{
    Timeline tl;
    tl.video.resize(1);
    for (const Clip& c : clips) tl.video[0].clips << c;
    return tl;
}

void transition(Timeline& tl, TransitionType type, std::function<void(TransitionStyle&)> more = {})
{
    Clip& a = tl.video[0].clips[0];
    Clip& b = tl.video[0].clips[1];
    a.transOut = b.transIn = 10;
    a.transOutStyle.type = b.transInStyle.type = type;
    if (more) {
        more(a.transOutStyle);
        more(b.transInStyle);
    }
}

void grade(Clip& c, bool lut)
{
    EffectRegistry::add(c, "grade");
    EffectRegistry::instance(c, "grade")->params["saturation"] = 75.0;
    EffectRegistry::instance(c, "grade")->params["contrast"] = 1.2;
    if (lut) EffectRegistry::instance(c, "grade")->params["lut"] = QString(":/luts/kino.cube");
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("picture_gpu_render");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    if (!Picture::Gpu::Service::instance()) {
        std::printf("SKIP: no GPU (%s)\n", qPrintable(Picture::Gpu::probe().error));
        return Check::kSkip;
    }
    std::printf("GPU: %s on %s\n", qPrintable(Picture::Gpu::Service::instance()->info().api),
                qPrintable(Picture::Gpu::Service::instance()->info().device));
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    const QStringList x264 = {"-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"};
    g_a = Check::makeMedia(tmp.filePath("a.mp4"), QStringList{"-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30:duration=3"} + x264);
    g_b = Check::makeMedia(tmp.filePath("b.mp4"), QStringList{"-f", "lavfi", "-i", "mandelbrot=size=1280x720:rate=30", "-t", "3"} + x264);
    g_c = Check::makeMedia(tmp.filePath("c.mp4"), QStringList{"-f", "lavfi", "-i", "testsrc=size=1920x1080:rate=30:duration=3"} + x264);
    g_still = Check::makeMedia(tmp.filePath("s.png"), {"-f", "lavfi", "-i", "smptehdbars=size=800x600", "-frames:v", "1"});
    if (!CHECK(!g_a.isEmpty() && !g_b.isEmpty() && !g_c.isEmpty() && !g_still.isEmpty())) return Check::result();
    Check::initEngine();
    std::setlocale(LC_NUMERIC, "C");

    // Plain clips (fitted, larger source shrunk with mips) and a still
    same("plain", oneTrack({media(1, g_a, 0, 30)}), {0, 17}, 1.0, 30);
    same("plain 1080p source", oneTrack({media(1, g_c, 0, 30)}), {3}, 1.5, 30);
    same("still", oneTrack({media(1, g_still, 0, 30)}), {0}, 1.0, 30);
    same("preview half size", oneTrack({media(1, g_c, 0, 30)}), {5}, 2.0, 30, Picture::Mode::Preview, QSize(640, 360));

    // Three tracks: transform, crop, opacity, rotation, grade (+ LUT)
    {
        Timeline tl;
        tl.video.resize(3);
        Clip bottom = media(1, g_c, 0, 60);
        grade(bottom, true);
        tl.video[0].clips << bottom;
        Clip mid = media(2, g_a, 0, 60);
        mid.transform.zoomX = mid.transform.zoomY = 0.5;
        mid.transform.posX = -250;
        mid.transform.rotation = 12;
        mid.transform.cropLeft = 40;
        mid.transform.cropBottom = 30;
        grade(mid, false);
        tl.video[1].clips << mid;
        Clip top = media(3, g_b, 0, 60);
        top.transform.zoomX = top.transform.zoomY = 0.4;
        top.transform.posX = 300;
        top.transform.posY = 150;
        top.transform.opacity = 70;
        tl.video[2].clips << top;
        same("3 tracks transform+grade", tl, {0, 29}, 1.5, 30);
        same("3 tracks preview", tl, {10}, 2.0, 30, Picture::Mode::Preview, QSize(960, 540));
    }

    // A clip tilted in depth over a plain one: the card leaves it to the CPU (homography), the frame stays the same
    {
        Timeline tl;
        tl.video.resize(2);
        tl.video[0].clips << media(1, g_c, 0, 30);
        Clip tilted = media(2, g_a, 0, 30);
        tilted.transform.zoomX = tilted.transform.zoomY = 0.7;
        tilted.transform.tiltX = 20;
        tilted.transform.tiltY = -35;
        tilted.transform.rotation = 5;
        tl.video[1].clips << tilted;
        same("tilted clip", tl, {4}, 1.5, 30);
        same("tilted clip preview", tl, {9}, 2.0, 30, Picture::Mode::Preview, QSize(640, 360));
    }

    // A kept region (Reframe::keepCopy, the kill feed above a short) on its thin dark frame, faded to 80 %
    {
        Timeline tl;
        tl.video.resize(2);
        tl.video[0].clips << media(1, g_c, 0, 30);
        Clip keep = media(2, g_a, 0, 30);
        keep.keepRegion = "killfeed";
        keep.transform.cropLeft = 700;
        keep.transform.cropTop = 40;
        keep.transform.cropBottom = 520;
        keep.transform.zoomX = keep.transform.zoomY = 1.6;
        keep.transform.posY = 200;
        keep.transform.opacity = 80;
        tl.video[1].clips << keep;
        same("kept region frame", tl, {5}, 1.5, 30);
    }

    // Transitions
    {
        Timeline tl = oneTrack({media(1, g_a, 0, 30), media(2, g_b, 30, 30)});
        transition(tl, TransitionType::CrossDissolve);
        same("dissolve", tl, {26, 30, 34}, 1.5, 30);
        Timeline wipe = tl;
        transition(wipe, TransitionType::WipeRight, [](TransitionStyle& s) { s.softness = 20; });
        same("wipe", wipe, {27, 31}, 1.5, 30);
        Timeline luma = tl;
        transition(luma, TransitionType::Luma, [](TransitionStyle& s) { s.luma = ":/lumas/kreis"; });
        same("luma", luma, {28, 32}, 1.5, 30);
        Timeline dip = tl;
        transition(dip, TransitionType::DipToColor, [](TransitionStyle& s) { s.color = QColor(200, 40, 40); });
        same("dip", dip, {26, 29, 33}, 1.5, 30);
        Timeline border = tl;
        transition(border, TransitionType::WipeDown, [](TransitionStyle& s) { s.border = 8; });
        same("wipe border (CPU layer)", border, {30}, 1.5, 30);
        Timeline eased = tl;
        transition(eased, TransitionType::WipeLeft, [](TransitionStyle& s) { s.ease = MotionEase::InOut; });
        same("wipe eased", eased, {28, 31}, 1.5, 30);
        Timeline push = tl;
        transition(push, TransitionType::PushUp, [](TransitionStyle& s) { s.ease = MotionEase::Out; });
        same("push (CPU layer)", push, {27, 31}, 1.5, 30);
        Timeline whip = tl;
        transition(whip, TransitionType::WhipRight, [](TransitionStyle& s) { s.ease = MotionEase::InOut; });
        same("whip (CPU layer)", whip, {29}, 1.5, 30);
        Timeline fades = oneTrack({media(1, g_a, 0, 30)});
        fades.video[0].clips[0].transIn = 8;
        fades.video[0].clips[0].transInAlone = true;
        same("fade in", fades, {3}, 1.5, 30);
    }

    // What the card leaves to the CPU: a title over a clip, blur, blur fill, subtitles
    {
        Timeline tl;
        tl.video.resize(3);
        tl.video[0].clips << media(1, g_a, 0, 30);
        Clip blurred = media(2, g_b, 0, 30);
        blurred.transform.zoomX = blurred.transform.zoomY = 0.5;
        EffectRegistry::add(blurred, "blur");
        tl.video[1].clips << blurred;
        Clip title;
        title.id = 3;
        title.kind = ClipKind::Title;
        title.out = 29;
        title.title.text = "GPU";
        title.title.size = 120;
        tl.video[2].clips << title;
        SubtitleTrack st;
        SubtitleCue cue;
        cue.start = 0;
        cue.end = 30;
        cue.text = "Untertitel";
        st.cues << cue;
        tl.subtitles << st;
        same("title+blur+subtitle", tl, {5}, 1.5, 30);
        // Counter and progress bar titles: drawn by the CPU, the same picture
        Timeline gen = oneTrack({media(1, g_a, 0, 30)});
        gen.video.resize(3);
        Clip counter;
        counter.id = 5;
        counter.kind = ClipKind::Title;
        counter.out = 29;
        counter.title.kind = TitleKind::Counter;
        counter.title.countTo = 5000;
        counter.title.size = 120;
        gen.video[1].clips << counter;
        Clip bar = counter;
        bar.id = 6;
        bar.title.kind = TitleKind::Progress;
        bar.title.boxOn = true;
        gen.video[2].clips << bar;
        same("counter + progress bar", gen, {0, 13, 29}, 1.5, 30);
        // Entry/exit animation (pop in, fade out): the card shows the same sizes and opacities
        Timeline anim = oneTrack({media(1, g_a, 0, 30)});
        anim.video.resize(2);
        Clip pop = counter;
        pop.id = 7;
        pop.title.kind = TitleKind::Text;
        pop.title.text = "POP";
        pop.title.enter = TitleAnim::Pop;
        pop.title.exit = TitleAnim::Fade;
        anim.video[1].clips << pop;
        same("title pop in + fade out", anim, {0, 3, 27}, 1.5, 30);
        Timeline fill = oneTrack({media(1, g_still, 0, 30)});
        fill.video[0].clips[0].transform.fill = FitFill::Blur;
        same("blur fill still", fill, {0}, 1.5, 30);
        Timeline vfill = oneTrack({media(1, g_c, 0, 30)});
        vfill.video[0].clips[0].transform.fill = FitFill::Blur;
        vfill.video[0].clips[0].transform.zoomX = vfill.video[0].clips[0].transform.zoomY = 0.7;
        vfill.video[0].clips[0].transform.opacity = 80;
        grade(vfill.video[0].clips[0], false);
        same("blur fill video+grade", vfill, {4}, 1.5, 30);
        Picture::Options o = options(Picture::Mode::Export, true);
        Picture::Renderer r(vfill, o);
        r.render(4);
        CHECK_EQ(r.stats().layersCpu, 0); // all on the card
    }

    // Compositing graphs (0.19): evaluated on the card (only the clip's picture made on the CPU), the same frames as
    // CompEval on the CPU; with the clip's transform/crop/opacity, and as one side of a dissolve
    {
        auto withComp = [](Clip c) {
            EffectInstance e;
            e.effectId = ::Comp::kEffectId;
            CompGraph g;
            const char* types[] = {"clip", "key", "gradient", "merge", "mask", "blur"};
            for (int i = 0; i < 6; ++i) g.nodes << CompNode{i + 1, types[i], QPointF()};
            // clip -> luma key -> merge over a gradient -> blur in a soft ellipse -> out
            g.links = {{1, 2, "src"}, {3, 4, "bg"}, {2, 4, "fg"}, {4, 6, "src"}, {5, 6, "mask"}};
            g.out = 6;
            ::Comp::setGraph(e, g);
            e.params.insert("n2.mode", "luma");
            e.params.insert("n2.low", 0.25);
            e.params.insert("n2.soft", 0.1);
            e.params.insert("n3.c0", "#ff302060");
            e.params.insert("n3.c1", "#ff60c0a0");
            e.params.insert("n5.shape", "ellipse");
            e.params.insert("n5.soft", 20.0);
            e.params.insert("n6.radius", 5.0);
            c.effects << e;
            return c;
        };
        Timeline plain = oneTrack({withComp(media(1, g_a, 0, 30))});
        same("comp graph", plain, {0, 17}, 1.5, 30);
        Timeline placed = plain;
        Clip& c = placed.video[0].clips[0];
        c.transform.zoomX = c.transform.zoomY = 0.7;
        c.transform.posX = 60;
        c.transform.cropLeft = 40;
        c.transform.opacity = 70;
        same("comp graph transform+crop", placed, {5}, 1.5, 30);
        for (const Timeline* tl : {&plain, &placed}) {
            Picture::Renderer r(*tl, options(Picture::Mode::Export, true));
            r.render(5);
            CHECK_EQ(r.stats().layersCompGpu, 1);
            CHECK_EQ(r.stats().layersCpu, 0);
        }
        Timeline dissolve = oneTrack({withComp(media(1, g_a, 0, 30)), media(2, g_b, 30, 30)});
        transition(dissolve, TransitionType::CrossDissolve);
        same("comp graph in a dissolve", dissolve, {27, 31}, 1.5, 30);
    }

    // Encoder planes from the card (4:2:0 and NV12) against OutputColor::fromRgba of the CPU frame
    {
        Timeline tl = oneTrack({media(1, g_a, 0, 30)});
        grade(tl.video[0].clips[0], false);
        Picture::Renderer cpu(tl, options(Picture::Mode::Export, false)), gpu(tl, options(Picture::Mode::Export, true));
        const int W = kFmt.width, H = kFmt.height;
        for (OutputColor::Layout layout : {OutputColor::Layout::Yuv420, OutputColor::Layout::Nv12}) {
            std::vector<uint8_t> ref(size_t(W) * H * 3 / 2), got(ref.size(), 7);
            auto targetOf = [&](std::vector<uint8_t>& v) {
                OutputColor::Target t;
                t.layout = layout;
                t.colorspace = 709;
                t.planes[0] = v.data();
                t.strides[0] = W;
                t.planes[1] = v.data() + size_t(W) * H;
                if (layout == OutputColor::Layout::Nv12) {
                    t.strides[1] = W;
                } else {
                    t.strides[1] = t.strides[2] = W / 2;
                    t.planes[2] = t.planes[1] + size_t(W / 2) * (H / 2);
                }
                return t;
            };
            const Picture::Frame a = cpu.render(11);
            if (!CHECK(!a.isNull())) continue;
            OutputColor::fromRgba(a.bits(), W, H, a.stride(), targetOf(ref));
            const OutputColor::Target t = targetOf(got);
            Picture::RenderHints hints;
            hints.yuv = &t;
            const Picture::Frame b = gpu.render(11, hints);
            CHECK(!b.isNull());
            CHECK(b.yuvWritten());
            CHECK(b.bits() == nullptr);
            const Diff d = compare(ref.data(), got.data(), ref.size());
            std::printf("  yuv readback %-14s mean %.3f max %3d PSNR %.1f dB\n",
                        layout == OutputColor::Layout::Nv12 ? "NV12" : "4:2:0", d.mean, d.max, d.psnr);
            CHECK(d.mean <= 1.0 && d.psnr >= 40);
        }
        // A layout the card does not write: RGBA as usual
        OutputColor::Target t10;
        t10.bits = 10;
        Picture::RenderHints hints;
        hints.yuv = &t10;
        const Picture::Frame c = gpu.render(12, hints);
        CHECK(!c.isNull() && !c.yuvWritten() && c.bits());
        CHECK(gpu.stats().framesGpu >= 3);
    }

    // Before/After in the preview: the grade bypass reaches the GPU path
    {
        Timeline tl = oneTrack({media(1, g_a, 0, 30)});
        grade(tl.video[0].clips[0], true);
        auto bypass = std::make_shared<std::atomic<bool>>(true);
        Picture::Options o = options(Picture::Mode::Preview, true);
        o.gradeBypass = bypass;
        Picture::Options p = o;
        p.gpu = false;
        Picture::Renderer gpu(tl, o), plain(oneTrack({media(1, g_a, 0, 30)}), p);
        const Picture::Frame a = gpu.render(4), b = plain.render(4);
        const Diff d = compare(a.bits(), b.bits(), size_t(a.width()) * a.height() * 4);
        CHECK(d.mean <= 1.5);
    }

    // A still wider than the card's largest texture (a panorama: D3D11 takes 16384 px): the card cannot hold it, the
    // layer is drawn on the CPU. Before 0.21 the layer was left out and the frame still counted as made on the card
    {
        const int maxSize = Picture::Gpu::Service::instance()->info().maxTextureSize;
        std::printf("  largest texture %d px\n", maxSize);
        const int w = std::max(18000, maxSize + 600), h = 3'600'000 / w; // under the still loader's 4x project pixels
        const QString pano = Check::makeMedia(tmp.filePath("pano.png"),
                                              {"-f", "lavfi", "-i", QString("testsrc2=size=%1x%2").arg(w).arg(h), "-frames:v", "1"});
        if (CHECK(!pano.isEmpty())) {
            Clip c = media(1, pano, 0, 30);
            c.transform.zoomX = c.transform.zoomY = 12; // a part of it fills the frame
            same("still wider than a texture", oneTrack({c}), {0}, 1.5, 30);
        }
    }

    // Interlaced 4:2:0: the CPU converts field by field (chroma of one field never bleeds into the other); the card
    // sampled the chroma plane as progressive. Red lines in the top field, blue in the bottom one
    {
        const QString il = Check::makeMedia(
            tmp.filePath("interlaced.mkv"),
            {"-f", "lavfi", "-i", "color=c=black:size=1280x720:rate=25:duration=1", "-vf",
             "format=gbrp,geq=r='if(mod(Y,2),0,255)':g='0':b='if(mod(Y,2),255,0)',scale=interl=1,format=yuv420p,setfield=tff",
             "-c:v", "libx264", "-preset", "ultrafast", "-qp", "0", "-flags", "+ildct+ilme", "-x264-params", "tff=1",
             "-field_order", "tt"});
        if (CHECK(!il.isEmpty())) same("interlaced 4:2:0", oneTrack({media(1, il, 0, 20)}), {3}, 1.5, 30);
    }

    // The card lost (driver reset, GPU removed): the frame whose end reported it goes the CPU way, not the stale pixels
    // of a pooled buffer as a GPU frame; the context is made again and the next frame is on the card once more.
    // Before 0.21 endOffscreenFrame's result was not looked at and every later frame fell to the CPU for good
    {
        Timeline tl = oneTrack({media(1, g_a, 0, 30)});
        Picture::Renderer cpu(tl, options(Picture::Mode::Export, false)), gpu(tl, options(Picture::Mode::Export, true));
        const Picture::Frame a = cpu.render(5);
        CHECK(gpu.render(20).gpu()); // a pooled buffer with another picture in it
        Picture::Gpu::Service::instance()->simulateDeviceLoss();
        const Picture::Frame b = gpu.render(5);
        CHECK(!b.isNull());
        CHECK(!b.gpu());
        if (!b.isNull()) {
            const Diff d = compare(a.bits(), b.bits(), size_t(a.width()) * a.height() * 4);
            std::printf("  frame of the lost device       mean %.3f max %3d\n", d.mean, d.max);
            CHECK(d.mean <= 0.01);
        }
        const Picture::Frame c = gpu.render(5);
        CHECK(c.gpu()); // a new context
        if (!c.isNull()) CHECK(compare(a.bits(), c.bits(), size_t(a.width()) * a.height() * 4).mean <= 1.0);
    }

    // A readback that comes back short although the frame "succeeded" (Qt's D3D11 backend after a device removal: the
    // staging Map fails with a warning only, endOffscreenFrame still reports success): not a GPU frame with the stale
    // pixels of a pooled buffer - the CPU makes it
    {
        Timeline tl = oneTrack({media(1, g_a, 0, 30)});
        Picture::Renderer cpu(tl, options(Picture::Mode::Export, false)), gpu(tl, options(Picture::Mode::Export, true));
        const Picture::Frame a = cpu.render(8);
        CHECK(gpu.render(21).gpu());
        Picture::Gpu::Service::instance()->simulateEmptyReadbacks();
        const Picture::Frame b = gpu.render(8);
        CHECK(!b.isNull());
        CHECK(!b.gpu());
        if (!b.isNull()) {
            const Diff d = compare(a.bits(), b.bits(), size_t(a.width()) * a.height() * 4);
            std::printf("  frame of an empty readback     mean %.3f max %3d\n", d.mean, d.max);
            CHECK(d.mean <= 0.01);
        }
        CHECK(gpu.render(8).gpu());
    }

    // The device lost again and again: after the last remake the engine is off - run() refuses at once, and a
    // renderer no longer builds the card's draws first (a layer was decoded twice per frame: GPU try, then CPU)
    // (last: the GPU stays off in this process)
    {
        Timeline tl = oneTrack({media(1, g_a, 0, 30)});
        Picture::Renderer cpu(tl, options(Picture::Mode::Export, false)), gpu(tl, options(Picture::Mode::Export, true));
        Picture::Gpu::Service* svc = Picture::Gpu::Service::instance();
        for (int i = 0; i < 4 && svc->available(); ++i) {
            svc->simulateDeviceLoss();
            gpu.render(3 + i);
        }
        CHECK(!svc->available());
        bool called = false;
        CHECK(!svc->run([&](Picture::Gpu::Context&) { called = true; }));
        CHECK(!called);
        gpu.render(10);
        const int before = gpu.stats().layersDecoded;
        const Picture::Frame b = gpu.render(11), a = cpu.render(11);
        std::printf("  layers decoded for a frame with the GPU off: %d\n", gpu.stats().layersDecoded - before);
        CHECK_EQ(gpu.stats().layersDecoded - before, 1);
        CHECK(!b.gpu());
        if (!b.isNull() && !a.isNull()) CHECK(compare(a.bits(), b.bits(), size_t(a.width()) * a.height() * 4).mean <= 0.01);
    }

    if (const int n = qEnvironmentVariableIntValue("PICTURE_GPU_RENDER_BENCH"); n > 0) {
        Timeline tl;
        tl.video.resize(3);
        for (int i = 0; i < 3; ++i) {
            Clip c = media(i + 1, i == 1 ? g_c : g_a, 0, 90);
            if (i > 0) {
                c.transform.zoomX = c.transform.zoomY = 0.6;
                c.transform.posX = i == 1 ? -300 : 300;
            }
            grade(c, false);
            tl.video[i].clips << c;
        }
        ProjectFormat hd{1920, 1080, {30, 1}};
        for (bool useGpu : {false, true}) {
            Picture::Options o = options(Picture::Mode::Export, useGpu);
            o.format = hd;
            Picture::Renderer r(tl, o);
            r.render(0);
            QElapsedTimer wall;
            wall.start();
            const std::clock_t c0 = std::clock();
            for (int f = 1; f <= n; ++f) r.render(f % 90);
            std::printf("  bench %s: %.1f ms wall, %.1f ms CPU per frame\n", useGpu ? "GPU" : "CPU",
                        double(wall.nsecsElapsed()) / 1e6 / n, double(std::clock() - c0) * 1000.0 / CLOCKS_PER_SEC / n);
        }
    }
    return Check::result();
}
