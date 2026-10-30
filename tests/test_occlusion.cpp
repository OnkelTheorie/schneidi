// SPDX-License-Identifier: AGPL-3.0-only
// Covered videos are not decoded (tip from the user, 2026-10-24): the renderer takes the upper picture alone when it
// is opaque and fills the frame, and never decodes the lower track. This test counts decoded layers (export and
// preview) so it stays that way, and checks the cases where the lower track must show (smaller, transparent, other
// aspect ratio).

#include "check.h"

#include "engine/ClipFormat.h"
#include "engine/picture/Renderer.h"

#include <QApplication>
#include <QTemporaryDir>
#include <atomic>
#include <clocale>
#include <functional>

namespace {

const ProjectFormat kFmt{640, 360, {25, 1}};
QString g_wide, g_narrow;

// Layers decoded beyond the upper one for frames 0-9 (0 = the lower track was never decoded)
int lowerDecodes(const Timeline& tl, Picture::Mode mode)
{
    Picture::Options o;
    o.format = kFmt;
    o.mode = mode;
    Picture::Renderer r(tl, o);
    for (int pos = 0; pos < 10; ++pos) CHECK(!r.render(pos).isNull());
    return r.stats().layersDecoded - 10;
}

Timeline stacked(const std::function<void(Clip&)>& top, const QString& topMedia = g_wide)
{
    Timeline tl;
    tl.video.resize(2);
    Clip low;
    low.id = 1;
    low.mediaPath = g_wide;
    low.out = 49;
    tl.video[0].clips << low;
    Clip up;
    up.id = 2;
    up.mediaPath = topMedia;
    up.out = 49;
    top(up);
    tl.video[1].clips << up;
    return tl;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("occlusion");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    g_wide = Check::makeMedia(tmp.filePath("wide.mp4"), {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=2",
                                                         "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    g_narrow = Check::makeMedia(tmp.filePath("narrow.mp4"), {"-f", "lavfi", "-i", "testsrc2=size=480x360:rate=25:duration=2",
                                                             "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!g_wide.isEmpty() && !g_narrow.isEmpty())) return Check::result();
    Check::initEngine();
    std::setlocale(LC_NUMERIC, "C");

    struct Case {
        const char* name;
        Timeline tl;
        bool lowerVisible;
    };
    const QVector<Case> cases = {
        {"covered", stacked([](Clip&) {}), false},
        {"covered, zoomed in", stacked([](Clip& c) { c.transform.zoomX = c.transform.zoomY = 1.3; }), false},
        {"smaller", stacked([](Clip& c) { c.transform.zoomX = c.transform.zoomY = 0.5; }), true},
        {"moved", stacked([](Clip& c) { c.transform.posX = 100; }), true},
        {"transparent", stacked([](Clip& c) { c.transform.opacity = 70; }), true},
        {"other aspect ratio", stacked([](Clip&) {}, g_narrow), true},
    };
    for (const Case& c : cases)
        for (Picture::Mode mode : {Picture::Mode::Export, Picture::Mode::Preview}) {
            const int n = lowerDecodes(c.tl, mode);
            if (!CHECK(c.lowerVisible ? n > 0 : n == 0))
                std::printf("       %s (%s): %d decodes of the lower track\n", c.name,
                            mode == Picture::Mode::Export ? "export" : "preview", n);
        }
    return Check::result();
}
