// SPDX-License-Identifier: AGPL-3.0-only
// Custom effect "ffmpeg_filter" (CustomEffects.h): a libavfilter chain on the clip's picture.
//
// Safety: libavfilter has filters that read files (movie, amovie, lut3d/haldclut/curves/selectivecolor with a file,
// subtitles/ass, drawtext's textfile/fontfile), write files (curves' plot), run commands (sendcmd, zmq/azmq) or take
// other inputs. A denylist would have to follow every FFmpeg release, so the chain may only use filters of an
// allowlist: colour, tone, blur, edge, distortion and drawing filters that work on one picture from its own pixels.
// Options are allowlisted too: by their type (numbers, switches, colours, sizes, durations, flag names cannot name a
// file) and every text option by name, reviewed per filter (expressions, point lists, colour names; never psfile or
// plot). The chain must be one linear chain (no labels, no ';'), and all of it is checked on the parsed chain BEFORE
// any filter is made (a filter's init may already open its file). The bundled LGPL FFmpeg
// (packaging/ffmpeg-components.txt) builds exactly these filters.
#include "engine/picture/CustomEffects.h"

#include "engine/FfmpegLog.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifdef SCHNEIDI_HAVE_AVFILTER
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}
#endif

namespace Picture::Custom {

namespace {

// Filters a chain may use. Rules for adding one: it works on one video picture from its own pixels (no file, no
// network, no second input, no commands), it is LGPL (no --enable-gpl filter: the bundled FFmpeg has none), it does
// not keep pictures of other frames (frames render out of order and in parallel), and it is added to
// packaging/ffmpeg-components.txt too.
const QSet<QString>& allowed()
{
    static const QSet<QString> s = {
        // colour and tone
        "negate", "hue", "huesaturation", "colorchannelmixer", "colorbalance", "colorlevels", "colorcontrast",
        "colorcorrect", "colorize", "colortemperature", "vibrance", "exposure", "curves", "lut", "lutrgb", "lutyuv",
        "geq", "monochrome", "grayworld", "selectivecolor", "colorhold", "chromahold", "pseudocolor", "limiter",
        "shuffleplanes",
        // blur, sharpen, edges
        "gblur", "avgblur", "unsharp", "cas", "median", "bilateral", "edgedetect", "sobel", "prewitt", "roberts",
        "scharr", "kirsch", "convolution", "dilation", "erosion", "deflate", "inflate", "deband", "gradfun",
        // shape and pattern
        "vignette", "noise", "pixelize", "chromashift", "rgbashift", "hflip", "vflip", "transpose", "rotate", "shear",
        "lenscorrection", "swaprect", "crop", "pad", "scale", "fillborders", "drawbox", "drawgrid",
        // keys (alpha)
        "colorkey", "lumakey", "chromakey",
        // time-based fade (by t), format helpers
        "fade", "format", "null", "setsar"};
    return s;
}

// The text options a filter of the list may take, reviewed one by one: expressions (hue, lut*, geq, pseudocolor,
// vignette, rotate, swaprect, crop, pad, scale, drawbox/drawgrid, setsar), curves' point lists, selectivecolor's
// adjustments, convolution's matrices, colour names, scale's flags and size, format's names. A text option that is
// not here is refused (curves/selectivecolor psfile reads a file, curves plot writes one). A new filter or FFmpeg
// version with new text options needs a review before they are added
const QHash<QString, QSet<QString>>& textOptions()
{
    static const QHash<QString, QSet<QString>> s = {
        {"hue", {"h", "s", "H", "b"}},
        {"curves", {"master", "m", "red", "r", "green", "g", "blue", "b", "all"}},
        {"lut", {"c0", "c1", "c2", "c3", "y", "u", "v", "r", "g", "b", "a"}},
        {"lutrgb", {"c0", "c1", "c2", "c3", "y", "u", "v", "r", "g", "b", "a"}},
        {"lutyuv", {"c0", "c1", "c2", "c3", "y", "u", "v", "r", "g", "b", "a"}},
        {"geq", {"lum_expr", "lum", "cb_expr", "cb", "cr_expr", "cr", "alpha_expr", "a", "red_expr", "r", "green_expr",
                 "g", "blue_expr", "b"}},
        {"selectivecolor", {"reds", "yellows", "greens", "cyans", "blues", "magentas", "whites", "neutrals", "blacks"}},
        {"pseudocolor", {"c0", "c1", "c2", "c3"}},
        {"convolution", {"0m", "1m", "2m", "3m"}},
        {"vignette", {"angle", "a", "x0", "y0"}},
        {"rotate", {"angle", "a", "out_w", "ow", "out_h", "oh", "fillcolor", "c"}},
        {"shear", {"fillcolor", "c"}},
        {"swaprect", {"w", "h", "x1", "y1", "x2", "y2"}},
        {"crop", {"out_w", "w", "out_h", "h", "x", "y"}},
        {"pad", {"width", "w", "height", "h", "x", "y"}},
        {"scale", {"w", "width", "h", "height", "flags", "size", "s"}},
        {"drawbox", {"x", "y", "width", "w", "height", "h", "color", "c", "thickness", "t", "box_source"}},
        {"drawgrid", {"x", "y", "width", "w", "height", "h", "color", "c", "thickness", "t"}},
        {"format", {"pix_fmts", "color_spaces", "color_ranges"}},
        {"setsar", {"sar", "ratio", "r"}}};
    return s;
}

} // namespace

QStringList allowedFilters()
{
    QStringList l(allowed().begin(), allowed().end());
    l.sort();
    return l;
}

bool isCustom(const QString& effectId)
{
    return effectId == QLatin1String(kFilterId) || effectId == QLatin1String(kShaderId);
}

bool canRender(const QString& effectId, QString* why)
{
    if (effectId == QLatin1String(kFilterId)) {
        if (filterAvailable()) return true;
        if (why) *why = QStringLiteral("this build has no libavfilter (FFmpeg filter chains are not available)");
        return false;
    }
    if (effectId == QLatin1String(kShaderId)) return shaderRenderable(why);
    return false;
}

#ifdef SCHNEIDI_HAVE_AVFILTER

namespace {

QString avError(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return QString::fromUtf8(buf);
}

struct GraphFree {
    void operator()(AVFilterGraph* g) const { avfilter_graph_free(&g); }
};
struct SegmentFree {
    void operator()(AVFilterGraphSegment* s) const { avfilter_graph_segment_free(&s); }
};
struct InOutFree {
    void operator()(AVFilterInOut* io) const { avfilter_inout_free(&io); }
};
struct FrameFree {
    void operator()(AVFrame* f) const { av_frame_free(&f); }
};

// Is one option of a filter of the list allowed (by its type, text options by name)? Empty = fine
QString checkOption(const AVFilter* f, const QString& name, const AVDictionaryEntry* e)
{
    const QString key = QString::fromUtf8(e->key);
    const AVOption* o = nullptr;
    if (f->priv_class)
        o = av_opt_find(const_cast<const AVClass**>(&f->priv_class), e->key, nullptr, 0, AV_OPT_SEARCH_FAKE_OBJ);
    if (!o) {
        // The options every filter has: only `enable` (an expression when the filter works)
        const AVClass* generic = avfilter_get_class();
        if (key == QLatin1String("enable") && (f->flags & AVFILTER_FLAG_SUPPORT_TIMELINE)
            && av_opt_find(&generic, e->key, nullptr, 0, AV_OPT_SEARCH_FAKE_OBJ))
            return {};
        return QStringLiteral("filter '%1' has no option '%2' (see `ffmpeg -h filter=%1`)").arg(name, key);
    }
    switch (o->type) {
    case AV_OPT_TYPE_FLAGS:
    case AV_OPT_TYPE_INT:
    case AV_OPT_TYPE_INT64:
    case AV_OPT_TYPE_UINT64:
    case AV_OPT_TYPE_DOUBLE:
    case AV_OPT_TYPE_FLOAT:
    case AV_OPT_TYPE_BOOL:
    case AV_OPT_TYPE_RATIONAL:
    case AV_OPT_TYPE_COLOR:
    case AV_OPT_TYPE_IMAGE_SIZE:
    case AV_OPT_TYPE_VIDEO_RATE:
    case AV_OPT_TYPE_DURATION:
    case AV_OPT_TYPE_PIXEL_FMT:
        return {}; // parsed as a number, a name of a list or a colour: cannot name a file
    case AV_OPT_TYPE_STRING:
        if (textOptions().value(name).contains(key)) return {};
        break;
    default:
        break;
    }
    if (key.contains(QLatin1String("file"), Qt::CaseInsensitive) || key == QLatin1String("plot"))
        return QStringLiteral("filter '%1': option '%2' would use a file (not allowed)").arg(name, key);
    return QStringLiteral("filter '%1': option '%2' is not allowed (only numbers, switches, colours and reviewed "
                          "expressions)")
        .arg(name, key);
}

// FFmpeg's expressions can loop (while(cond, body); ld/st keep the loop's state): one such option hangs the thread
// that builds or runs the chain, and a running filter cannot be stopped. So those functions are refused in every
// value. The expression reader drops all white space and compares names case by case; this check ignores case
QString checkExpression(const QString& name, const AVDictionaryEntry* e)
{
    static const QRegularExpression loop(QStringLiteral(R"((^|[^A-Za-z0-9_])(while|ld|st)\()"),
                                         QRegularExpression::CaseInsensitiveOption);
    QString v = QString::fromUtf8(e->value);
    v.remove(QRegularExpression(QStringLiteral("\\s")));
    const QRegularExpressionMatch m = loop.match(v);
    if (!m.hasMatch()) return {};
    return QStringLiteral("filter '%1': option '%2' uses %3() (loops and stored variables are not allowed in "
                          "expressions)")
        .arg(name, QString::fromUtf8(e->key), m.captured(2));
}

// Checks the parsed chain before anything of it is made: one linear chain of allowed filters, no labels, no file
// options. Empty = fine
QString checkSegment(const AVFilterGraphSegment* seg)
{
    if (seg->nb_chains != 1) return QStringLiteral("only one linear chain (filters separated by ',', no ';')");
    const AVFilterChain* ch = seg->chains[0];
    if (ch->nb_filters == 0) return QStringLiteral("the chain has no filter");
    for (size_t i = 0; i < ch->nb_filters; ++i) {
        const AVFilterParams* p = ch->filters[i];
        const QString name = QString::fromUtf8(p->filter_name ? p->filter_name : "");
        if (p->nb_inputs || p->nb_outputs)
            return QStringLiteral("no link labels like [in] in the chain (filter '%1')").arg(name);
        if (!allowed().contains(name)) {
            if (avfilter_get_by_name(p->filter_name))
                return QStringLiteral("filter '%1' is not allowed (files, commands or several inputs; allowed: %2)")
                    .arg(name, allowedFilters().join(", "));
            return QStringLiteral("no filter '%1' (allowed: %2)").arg(name, allowedFilters().join(", "));
        }
        if (!avfilter_get_by_name(p->filter_name))
            return QStringLiteral("filter '%1' is not in this build's FFmpeg").arg(name);
        const AVFilter* f = avfilter_get_by_name(p->filter_name);
        const AVDictionaryEntry* e = nullptr;
        while ((e = av_dict_iterate(p->opts, e))) {
            if (const QString why = checkOption(f, name, e); !why.isEmpty()) return why;
            if (const QString why = checkExpression(name, e); !why.isEmpty()) return why;
        }
    }
    return {};
}

// One built graph: buffer -> chain -> scale back to the size -> rgba -> buffersink
struct Graph {
    QString chain;
    int w = 0, h = 0;
    std::unique_ptr<AVFilterGraph, GraphFree> graph;
    AVFilterContext* src = nullptr;
    AVFilterContext* sink = nullptr;
};

// Builds the graph for w x h; nullptr = error in *error
std::unique_ptr<Graph> build(const QString& chain, int w, int h, QString* error)
{
    FfmpegLog::Capture log;
    auto fail = [&](const QString& what) -> std::unique_ptr<Graph> {
        if (error) {
            *error = what;
            const QString detail = log.text();
            if (!detail.isEmpty()) *error += QStringLiteral(" (FFmpeg: %1)").arg(detail.simplified());
        }
        return nullptr;
    };
    auto g = std::make_unique<Graph>();
    g->chain = chain;
    g->w = w;
    g->h = h;
    g->graph.reset(avfilter_graph_alloc());
    if (!g->graph) return fail(QStringLiteral("out of memory"));
    g->graph->nb_threads = 1; // frames render in parallel already
    const QByteArray text = chain.toUtf8();
    AVFilterGraphSegment* rawSeg = nullptr;
    int ret = avfilter_graph_segment_parse(g->graph.get(), text.constData(), 0, &rawSeg);
    std::unique_ptr<AVFilterGraphSegment, SegmentFree> seg(rawSeg);
    if (ret < 0) return fail(QStringLiteral("the chain cannot be read: %1").arg(avError(ret)));
    if (const QString why = checkSegment(seg.get()); !why.isEmpty()) return fail(why);
    // The filter names are freed by create_filters: keep them for messages
    QStringList names;
    for (size_t i = 0; i < seg->chains[0]->nb_filters; ++i)
        names << QString::fromUtf8(seg->chains[0]->filters[i]->filter_name);
    if ((ret = avfilter_graph_segment_create_filters(seg.get(), 0)) < 0)
        return fail(QStringLiteral("cannot make the filters: %1").arg(avError(ret)));
    if ((ret = avfilter_graph_segment_apply_opts(seg.get(), 0)) < 0) {
        if (ret == AVERROR_OPTION_NOT_FOUND)
            for (size_t i = 0; i < seg->chains[0]->nb_filters; ++i) {
                const AVDictionaryEntry* e = av_dict_iterate(seg->chains[0]->filters[i]->opts, nullptr);
                if (e)
                    return fail(QStringLiteral("filter '%1' has no option '%2' (see `ffmpeg -h filter=%1`)")
                                    .arg(names.value(int(i)), QString::fromUtf8(e->key)));
            }
        return fail(QStringLiteral("bad option value: %1").arg(avError(ret)));
    }
    if ((ret = avfilter_graph_segment_init(seg.get(), 0)) < 0)
        return fail(QStringLiteral("a filter refused its options: %1").arg(avError(ret)));
    AVFilterInOut *rawIn = nullptr, *rawOut = nullptr;
    ret = avfilter_graph_segment_link(seg.get(), 0, &rawIn, &rawOut);
    std::unique_ptr<AVFilterInOut, InOutFree> ins(rawIn), outs(rawOut);
    if (ret < 0) return fail(QStringLiteral("cannot link the chain: %1").arg(avError(ret)));
    if (!ins || ins->next || !outs || outs->next)
        return fail(QStringLiteral("the chain must take one picture and give one picture"));

    const QByteArray srcArgs =
        QStringLiteral("video_size=%1x%2:pix_fmt=rgba:time_base=1/1000000:pixel_aspect=1/1").arg(w).arg(h).toUtf8();
    AVFilterContext *scale = nullptr, *format = nullptr;
    if ((ret = avfilter_graph_create_filter(&g->src, avfilter_get_by_name("buffer"), "in", srcArgs.constData(), nullptr,
                                            g->graph.get())) < 0
        || (ret = avfilter_graph_create_filter(&scale, avfilter_get_by_name("scale"), "size",
                                               QStringLiteral("w=%1:h=%2").arg(w).arg(h).toUtf8().constData(), nullptr,
                                               g->graph.get())) < 0
        || (ret = avfilter_graph_create_filter(&format, avfilter_get_by_name("format"), "rgba", "pix_fmts=rgba", nullptr,
                                               g->graph.get())) < 0
        || (ret = avfilter_graph_create_filter(&g->sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr,
                                               g->graph.get())) < 0)
        return fail(QStringLiteral("cannot set up the filter graph: %1").arg(avError(ret)));
    if ((ret = avfilter_link(g->src, 0, ins->filter_ctx, unsigned(ins->pad_idx))) < 0
        || (ret = avfilter_link(outs->filter_ctx, unsigned(outs->pad_idx), scale, 0)) < 0
        || (ret = avfilter_link(scale, 0, format, 0)) < 0 || (ret = avfilter_link(format, 0, g->sink, 0)) < 0)
        return fail(QStringLiteral("cannot link the chain: %1").arg(avError(ret)));
    if ((ret = avfilter_graph_config(g->graph.get(), nullptr)) < 0)
        return fail(QStringLiteral("the chain does not work on a picture: %1").arg(avError(ret)));
    // No picture inside the chain may be much larger than the clip's (pad=w=16000:h=16000 would take a gigabyte per
    // graph, and every render thread keeps a few): each side at most 2x the larger side (a turn still fits) and
    // 8192, the area at most 4x
    const int side = std::min(8192, 2 * std::max(w, h));
    const qint64 area = 4 * qint64(w) * h;
    for (unsigned i = 0; i < g->graph->nb_filters; ++i) {
        const AVFilterContext* f = g->graph->filters[i];
        for (unsigned j = 0; j < f->nb_outputs; ++j) {
            const AVFilterLink* l = f->outputs[j];
            if (l && (l->w > side || l->h > side || qint64(l->w) * l->h > area))
                return fail(QStringLiteral("the chain makes a picture of %1x%2 from %3x%4 (at most %5 per side and 4 "
                                           "times the area)")
                                .arg(l->w).arg(l->h).arg(w).arg(h).arg(side));
        }
    }
    return g;
}

bool run(Graph& g, uint8_t* rgba, double seconds, QString* error)
{
    FfmpegLog::Capture log;
    auto fail = [&](const QString& what, int code) {
        if (error) {
            *error = QStringLiteral("%1: %2").arg(what, avError(code));
            if (!log.text().isEmpty()) *error += QStringLiteral(" (FFmpeg: %1)").arg(log.text().simplified());
        }
        return false;
    };
    std::unique_ptr<AVFrame, FrameFree> in(av_frame_alloc()), out(av_frame_alloc());
    if (!in || !out) return false;
    in->format = AV_PIX_FMT_RGBA;
    in->width = g.w;
    in->height = g.h;
    in->pts = std::llround(seconds * 1e6);
    in->sample_aspect_ratio = {1, 1};
    int ret = av_frame_get_buffer(in.get(), 0);
    if (ret < 0) return fail(QStringLiteral("no memory for the picture"), ret);
    const size_t row = size_t(g.w) * 4;
    for (int y = 0; y < g.h; ++y) std::memcpy(in->data[0] + size_t(y) * in->linesize[0], rgba + y * row, row);
    if ((ret = av_buffersrc_add_frame_flags(g.src, in.get(), 0)) < 0) return fail(QStringLiteral("filter input"), ret);
    if ((ret = av_buffersink_get_frame(g.sink, out.get())) < 0) return fail(QStringLiteral("filter output"), ret);
    if (out->width != g.w || out->height != g.h || out->format != AV_PIX_FMT_RGBA)
        return fail(QStringLiteral("filter output has another size"), AVERROR(EINVAL));
    for (int y = 0; y < g.h; ++y) std::memcpy(rgba + y * row, out->data[0] + size_t(y) * out->linesize[0], row);
    return true;
}

// The graphs of this thread (export workers, preview pump): the last few chains/sizes
struct ThreadCache {
    std::vector<std::unique_ptr<Graph>> graphs;
    QHash<QString, QString> failed; // chain + size -> error (not rebuilt every frame)
};
thread_local ThreadCache t_cache;

} // namespace

bool filterAvailable() { return true; }

QString checkFilter(const QString& chain, QSize size, int budgetMs)
{
    if (chain.trimmed().isEmpty()) return {};
    // A test picture with a gradient: the chain must build and turn it into a picture of the same size. On a thread
    // of its own with a time budget: a chain that takes too long here would stall every frame (and the edit that
    // checks it). FFmpeg cannot be interrupted, so a late check runs on detached; the loops that never end are
    // refused before (checkExpression)
    struct State {
        std::mutex mutex;
        std::condition_variable done;
        bool finished = false;
        QString error;
    };
    auto st = std::make_shared<State>();
    // The size the effect will run at (a crop=1280:720 fits a 1080p clip, not a small picture); unknown: a small one
    const int w = size.width() > 0 && size.height() > 0 ? std::min(size.width(), 8192) : 64;
    const int h = size.width() > 0 && size.height() > 0 ? std::min(size.height(), 8192) : 36;
    std::thread([st, chain, w, h] {
        std::vector<uint8_t> px(size_t(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t* p = &px[(size_t(y) * w + x) * 4];
                p[0] = uint8_t(x * 255 / w), p[1] = uint8_t(y * 255 / h), p[2] = 128, p[3] = 255;
            }
        QString error;
        if (auto g = build(chain, w, h, &error)) run(*g, px.data(), 0.0, &error);
        std::lock_guard lock(st->mutex);
        st->error = error;
        st->finished = true;
        st->done.notify_all();
    }).detach();
    std::unique_lock lock(st->mutex);
    if (!st->done.wait_for(lock, std::chrono::milliseconds(std::max(1, budgetMs)), [&] { return st->finished; }))
        return QStringLiteral("the chain takes too long (more than %1 ms for one picture)").arg(budgetMs);
    return st->error;
}

bool applyFilter(const QString& chain, uint8_t* rgba, int width, int height, double seconds, QString* error)
{
    if (chain.trimmed().isEmpty() || width <= 0 || height <= 0) return true;
    ThreadCache& c = t_cache;
    const QString failKey = QStringLiteral("%1|%2x%3").arg(chain).arg(width).arg(height);
    if (auto f = c.failed.constFind(failKey); f != c.failed.cend()) {
        if (error) *error = *f;
        return false;
    }
    auto it = std::find_if(c.graphs.begin(), c.graphs.end(),
                           [&](const auto& g) { return g->chain == chain && g->w == width && g->h == height; });
    if (it == c.graphs.end()) {
        QString why;
        auto g = build(chain, width, height, &why);
        if (!g) {
            if (c.failed.size() > 32) c.failed.clear();
            c.failed.insert(failKey, why);
            if (error) *error = why;
            return false;
        }
        if (c.graphs.size() >= 4) c.graphs.erase(c.graphs.begin());
        c.graphs.push_back(std::move(g));
        it = c.graphs.end() - 1;
    }
    QString why;
    if (run(**it, rgba, seconds, &why)) return true;
    // A chain that fails while running is not built again every frame: kept as failed like one that does not build
    c.graphs.erase(it);
    if (c.failed.size() > 32) c.failed.clear();
    c.failed.insert(failKey, why);
    if (error) *error = why;
    return false;
}

#else // built without libavfilter

namespace {
QString unavailable()
{
    return QStringLiteral("FFmpeg filter chains are not available in this build (schneidi was built without "
                          "libavfilter)");
}
} // namespace

bool filterAvailable() { return false; }
QString checkFilter(const QString& chain, QSize, int) { return chain.trimmed().isEmpty() ? QString() : unavailable(); }
bool applyFilter(const QString& chain, uint8_t*, int, int, double, QString* error)
{
    if (chain.trimmed().isEmpty()) return true;
    if (error) *error = unavailable();
    return false;
}

#endif

} // namespace Picture::Custom
