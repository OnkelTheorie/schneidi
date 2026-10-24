// Covered videos are not decoded (tip from the user, 2026-10-24): MLT's qtblend between the tracks takes the upper
// frame alone when it is opaque and fills the picture, and never asks the lower track for its image. This test counts
// image requests on the lower track (export format yuv420p and preview format rgba) so it stays that way, and checks
// the cases where the lower track must show (smaller, transparent, other aspect ratio).

#include "check.h"

#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QApplication>
#include <QTemporaryDir>
#include <atomic>
#include <clocale>
#include <functional>

namespace {

const ProjectFormat kFmt{640, 360, {25, 1}};
QString g_wide, g_narrow;

int countImage(mlt_frame frame, uint8_t** image, mlt_image_format* format, int* width, int* height, int writable)
{
    ++*static_cast<std::atomic<int>*>(mlt_frame_pop_service(frame));
    return mlt_frame_get_image(frame, image, format, width, height, writable);
}

mlt_frame countProcess(mlt_filter filter, mlt_frame frame)
{
    mlt_frame_push_service(frame, mlt_properties_get_data(MLT_FILTER_PROPERTIES(filter), "counter", nullptr));
    mlt_frame_push_get_image(frame, countImage);
    return frame;
}

// Image requests on the lower video track (MLT track 1; 0 is the black background) for frames 0-9
int lowerRequests(const Timeline& tl, mlt_image_format format)
{
    auto profile = makeProfile(kFmt);
    TimelineBuilder b(*profile);
    auto tractor = b.build(tl);
    std::unique_ptr<Mlt::Producer> lower(tractor->track(1));
    std::atomic<int> count{0};
    mlt_filter f = mlt_filter_new();
    f->process = countProcess;
    mlt_properties_set_data(MLT_FILTER_PROPERTIES(f), "counter", &count, 0, nullptr, nullptr);
    Mlt::Filter filter(f);
    mlt_filter_close(f);
    lower->attach(filter);
    for (int pos = 0; pos < 10; ++pos) {
        tractor->seek(pos);
        std::unique_ptr<Mlt::Frame> frame(tractor->get_frame());
        mlt_image_format fmt = format;
        int w = kFmt.width, h = kFmt.height;
        CHECK(frame->get_image(fmt, w, h) != nullptr);
    }
    lower->detach(filter);
    return count;
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
    Check::initMlt();
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
        for (mlt_image_format format : {mlt_image_yuv420p, mlt_image_rgba}) {
            const int n = lowerRequests(c.tl, format);
            if (!CHECK(c.lowerVisible ? n > 0 : n == 0))
                std::printf("       %s (%s): %d requests on the lower track\n", c.name, mlt_image_format_name(format), n);
        }
    return Check::result();
}
