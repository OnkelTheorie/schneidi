// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/video/Deinterlace.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/version.h>
#ifdef SCHNEIDI_HAVE_AVFILTER
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#endif
}

#include <QByteArray>

#include <atomic>
#include <memory>

namespace Video {

#ifdef SCHNEIDI_HAVE_AVFILTER

namespace {

const AVFilter* filter()
{
    static const AVFilter* f = [] {
        const AVFilter* b = avfilter_get_by_name("bwdif");
        return b ? b : avfilter_get_by_name("yadif");
    }();
    return f;
}

std::atomic<int> g_neighboursFiltered{0};

// FFmpeg 6.1-7.1 (FF_API_INTERLACED_FRAME): the decoders still fill AVFrame's deprecated interlaced_frame and
// top_field_first, and buffersrc ORs them into the flags again - the picture before, decoded interlaced, was flagged
// interlaced once more and deinterlaced too (twice the filter's cost per picture). The old fields follow the flags.
void matchOldFields(AVFrame* f)
{
#if FF_API_INTERLACED_FRAME
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    f->interlaced_frame = (f->flags & AV_FRAME_FLAG_INTERLACED) ? 1 : 0;
    f->top_field_first = (f->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) ? 1 : 0;
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#else
    (void)f;
#endif
}

struct GraphFree {
    void operator()(AVFilterGraph* g) const { avfilter_graph_free(&g); }
};

} // namespace

bool canDeinterlace()
{
    return filter() != nullptr;
}

int neighboursFiltered()
{
    return g_neighboursFiltered.load(std::memory_order_relaxed);
}

AVFrame* deinterlace(const AVFrame* prev, const AVFrame* cur, const AVFrame* next, bool topFieldFirst)
{
    const AVFilter* deint = filter();
    if (!deint || !cur || cur->width <= 0 || cur->height <= 0) return nullptr;
    auto alike = [cur](const AVFrame* f) {
        return f && f->width == cur->width && f->height == cur->height && f->format == cur->format ? f : cur;
    };
    prev = alike(prev);
    next = alike(next);

    std::unique_ptr<AVFilterGraph, GraphFree> graph(avfilter_graph_alloc());
    if (!graph) return nullptr;
    graph->nb_threads = 1; // pictures are deinterlaced in parallel by the render workers
    const AVRational sar = cur->sample_aspect_ratio.num > 0 ? cur->sample_aspect_ratio : AVRational{1, 1};
    const QByteArray srcArgs = QByteArray("video_size=") + QByteArray::number(cur->width) + 'x'
                               + QByteArray::number(cur->height) + ":pix_fmt=" + QByteArray::number(cur->format)
                               + ":time_base=1/25:pixel_aspect=" + QByteArray::number(sar.num) + '/'
                               + QByteArray::number(sar.den);
    // deint=interlaced: the picture before passes through untouched (flagged progressive below), so only `cur` costs
    const QByteArray deintArgs = QByteArray("mode=send_frame:deint=interlaced:parity=") + (topFieldFirst ? "tff" : "bff");
    AVFilterContext *src = nullptr, *mid = nullptr, *sink = nullptr;
    if (avfilter_graph_create_filter(&src, avfilter_get_by_name("buffer"), "in", srcArgs.constData(), nullptr,
                                     graph.get())
            < 0
        || avfilter_graph_create_filter(&mid, deint, "deint", deintArgs.constData(), nullptr, graph.get()) < 0
        || avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, graph.get())
               < 0
        || avfilter_link(src, 0, mid, 0) < 0 || avfilter_link(mid, 0, sink, 0) < 0
        || avfilter_graph_config(graph.get(), nullptr) < 0)
        return nullptr; // e.g. a pixel format the filter does not take

    // The three pictures in time order. The filter keeps its own references (the frames are only read).
    const AVFrame* in[3] = {prev, cur, next};
    for (int i = 0; i < 3; ++i) {
        AVFrame* f = av_frame_clone(in[i]);
        if (!f) return nullptr;
        f->pts = i;
        f->repeat_pict = 0; // a repeated progressive neighbour would make the filter pass `cur` through
        if (i == 1) {
            f->flags |= AV_FRAME_FLAG_INTERLACED;
            if (topFieldFirst) f->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
            else f->flags &= ~AV_FRAME_FLAG_TOP_FIELD_FIRST;
        } else {
            f->flags &= ~AV_FRAME_FLAG_INTERLACED;
        }
        matchOldFields(f);
        const int r = av_buffersrc_add_frame_flags(src, f, 0);
        av_frame_free(&f);
        if (r < 0) return nullptr;
    }
    // Out: the picture before (passed through), then `cur` deinterlaced (it needed the picture after it)
    AVFrame* out = nullptr;
    const uint8_t* prevPlane = prev->data[0]; // passed through, the picture before keeps its buffer
    for (int n = 0;; ++n) {
        AVFrame* f = av_frame_alloc();
        if (!f) break;
        if (av_buffersink_get_frame(sink, f) < 0) {
            av_frame_free(&f);
            break;
        }
        if (n == 0 && f->data[0] != prevPlane) g_neighboursFiltered.fetch_add(1, std::memory_order_relaxed);
        if (n == 1) {
            out = f;
            break;
        }
        av_frame_free(&f);
    }
    if (!out) return nullptr;
    av_frame_copy_props(out, cur);
    out->flags &= ~(AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST);
    matchOldFields(out); // progressive for whatever reads the old fields (a filter graph it is fed to)
    out->repeat_pict = 0;
    return out;
}

#else

bool canDeinterlace()
{
    return false;
}

AVFrame* deinterlace(const AVFrame*, const AVFrame*, const AVFrame*, bool)
{
    return nullptr;
}

int neighboursFiltered()
{
    return 0;
}

#endif

} // namespace Video
