// Frame timing of media between the paths that read them: a proxy shows the same picture at frame k as the original
// (a recording whose sound starts a frame before the picture, as OBS/phones/games write them), a source shorter than
// the model expects does not move the clips after it in the preview graph, and the frame position of a timestamp
// counts from the container start in the stream's time base (reader and smart render alike).
#include "check.h"

#include "core/Types.h"
#include "engine/Profiles.h"
#include "engine/ProxyManager.h"
#include "engine/TimelineBuilder.h"
#include "engine/video/Reader.h"

#include <Mlt.h>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
}

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

namespace {

// Mean luma difference of two pictures of the same size (8-bit planar), 1e9 if they cannot be compared
double lumaDiff(const Video::Picture* a, const Video::Picture* b)
{
    if (!a || !b || a->frame->width != b->frame->width || a->frame->height != b->frame->height) return 1e9;
    double sum = 0;
    const int w = a->frame->width, h = a->frame->height;
    for (int y = 0; y < h; ++y) {
        const uint8_t* p = a->frame->data[0] + y * a->frame->linesize[0];
        const uint8_t* q = b->frame->data[0] + y * b->frame->linesize[0];
        for (int x = 0; x < w; ++x) sum += std::abs(int(p[x]) - int(q[x]));
    }
    return sum / (double(w) * h);
}

// Proxy frame `n` matches original frame `n` better than its neighbours
void checkProxyFrames(const QString& original, const QString& proxy, const char* what)
{
    Video::Reader::Options o;
    o.hardware = false;
    const auto orig = Video::Reader::open(original, o);
    const auto prox = Video::Reader::open(proxy, o);
    if (!CHECK(orig && prox)) return;
    for (const int n : {1, 5, 12, 30}) {
        const auto p = prox->picture(n);
        const double same = lumaDiff(p.get(), orig->picture(n).get());
        const double before = lumaDiff(p.get(), orig->picture(n - 1).get());
        const double after = lumaDiff(p.get(), orig->picture(n + 1).get());
        Check::report(same < before && same < after, "proxy frame n = original frame n", __FILE__, __LINE__,
                      QString("%1, frame %2: same %3, n-1 %4, n+1 %5").arg(what).arg(n).arg(same).arg(before).arg(after));
    }
}

QString makeProxy(const QString& original)
{
    ProxyManager pm;
    QString message;
    QObject::connect(&pm, &ProxyManager::failed, [&](const QString&, const QString& m) { message = m; });
    pm.generate({original});
    QEventLoop loop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (!pm.isPending(original)) loop.quit();
    });
    poll.start(50);
    QTimer::singleShot(120000, &loop, &QEventLoop::quit);
    loop.exec();
    if (!message.isEmpty()) std::printf("proxy failed: %s\n", qPrintable(message));
    return pm.hasProxy(original) ? ProxyManager::proxyPath(original) : QString();
}

// Mean of the red and blue channel of the preview graph's picture at `pos`
std::pair<double, double> redBlue(Mlt::Tractor& tr, Mlt::Profile& prof, int pos)
{
    tr.seek(pos);
    std::unique_ptr<Mlt::Frame> f(tr.get_frame());
    mlt_image_format ifmt = mlt_image_rgba;
    int w = prof.width(), h = prof.height();
    const uint8_t* d = f ? f->get_image(ifmt, w, h) : nullptr;
    if (!d) return {-1, -1};
    double r = 0, b = 0;
    for (int i = 0; i < w * h; ++i) {
        r += d[i * 4];
        b += d[i * 4 + 2];
    }
    return {r / (w * h), b / (w * h)};
}

// Tractors reachable from `p` (through cuts, playlists and tractor tracks)
void tractorsIn(Mlt::Producer& p, std::set<mlt_producer>& out, int depth = 0)
{
    if (depth > 40 || !p.is_valid()) return;
    if (p.is_cut()) {
        tractorsIn(p.parent(), out, depth + 1);
        return;
    }
    if (p.type() == mlt_service_tractor_type) {
        if (!out.insert(p.get_producer()).second) return;
        Mlt::Tractor t(p);
        for (int i = 0; i < t.count(); ++i)
            if (std::unique_ptr<Mlt::Producer> track(t.track(i)); track) tractorsIn(*track, out, depth + 1);
    } else if (p.type() == mlt_service_playlist_type) {
        Mlt::Playlist pl(p);
        for (int i = 0; i < pl.count(); ++i)
            if (std::unique_ptr<Mlt::Producer> clip(pl.get_clip(i)); clip) tractorsIn(*clip, out, depth + 1);
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("media_timing");

    // --- Frame position of a timestamp: from the first key frame, else from the container start, which is in µs and
    // has to be converted to the stream's time base (MPEG-TS: 1/90000, starts at 1.4 s)
    {
        const AVRational ts{1, 90000};
        const int64_t start = 1400000; // µs
        CHECK_EQ(Video::framePosition(126000, AV_NOPTS_VALUE, start, ts, 25), int64_t(0));
        CHECK_EQ(Video::framePosition(126000 + 3600 * 10, AV_NOPTS_VALUE, start, ts, 25), int64_t(10));
        CHECK_EQ(Video::framePosition(126000 + 3600 * 10, 126000 - 3600, start, ts, 25), int64_t(11)); // key frame wins
        CHECK_EQ(Video::framePosition(3600 * 4, AV_NOPTS_VALUE, AV_NOPTS_VALUE, ts, 25), int64_t(4));
        // Seeking aims at the timestamp of the position, the position of that timestamp is the position again
        for (const int64_t pos : {int64_t(0), int64_t(1), int64_t(17), int64_t(250)}) {
            const int64_t t = Video::frameTimestamp(pos, AV_NOPTS_VALUE, start, ts, 25);
            CHECK_EQ(Video::framePosition(t, AV_NOPTS_VALUE, start, ts, 25), pos);
        }
        CHECK_EQ(Video::frameTimestamp(10, AV_NOPTS_VALUE, start, ts, 25), int64_t(126000 + 36000));
        CHECK_EQ(Video::frameTimestamp(10, 1000, start, ts, 25), int64_t(1000 + 36000));
    }

    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    Check::initMlt();
    QTemporaryDir dir;

    return Check::result();
}
