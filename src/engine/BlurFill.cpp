#include "engine/BlurFill.h"

#include <Mlt.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>

namespace BlurFill {

namespace {

// Target sigma in pixels of the small background: big enough that scaling it up stays smooth, small enough that the
// background has few pixels (blur 40 in a 9:16 frame: 70 x 125)
constexpr double kSmallSigma = 2.5;
constexpr int kMinHeight = 16, kMaxHeight = 360, kMaxWidth = 640;

// Radii of three box blurs that together approximate a Gaussian of `sigma` (W. Jarosz / I. Kutskir)
std::array<int, 3> boxRadii(double sigma)
{
    constexpr int n = 3;
    if (sigma < 0.5) return {0, 0, 0};
    const double ideal = std::sqrt(12 * sigma * sigma / n + 1);
    int wl = int(std::floor(ideal));
    if (wl % 2 == 0) --wl;
    const int wu = wl + 2;
    const int m = int(std::lround((12 * sigma * sigma - n * wl * wl - 4.0 * n * wl - 3.0 * n) / (-4.0 * wl - 4)));
    std::array<int, 3> r{};
    for (int i = 0; i < n; ++i) r[size_t(i)] = ((i < m ? wl : wu) - 1) / 2;
    return r;
}

// One box blur from a line of n packed RGBA pixels into n pixels `stride` bytes apart; the edge pixels repeat outward
void boxLine(const uint8_t* src, uint8_t* dst, int n, size_t stride, int r)
{
    const int w = 2 * r + 1;
    int sum[4] = {};
    for (int k = -r; k <= r; ++k) {
        const uint8_t* p = src + size_t(std::clamp(k, 0, n - 1)) * 4;
        for (int c = 0; c < 4; ++c) sum[c] += p[c];
    }
    for (int i = 0; i < n; ++i) {
        uint8_t* d = dst + size_t(i) * stride;
        for (int c = 0; c < 4; ++c) d[c] = uint8_t((sum[c] + w / 2) / w);
        const uint8_t* add = src + size_t(std::min(i + r + 1, n - 1)) * 4;
        const uint8_t* sub = src + size_t(std::max(i - r, 0)) * 4;
        for (int c = 0; c < 4; ++c) sum[c] += add[c] - sub[c];
    }
}

void blur(Background& bg, double sigma)
{
    const int w = bg.width, h = bg.height;
    std::vector<uint8_t> line(size_t(std::max(w, h)) * 4);
    for (int r : boxRadii(sigma)) {
        if (r <= 0) continue;
        for (int y = 0; y < h; ++y) {
            uint8_t* row = bg.rgba.data() + size_t(y) * w * 4;
            std::memcpy(line.data(), row, size_t(w) * 4);
            boxLine(line.data(), row, w, 4, r);
        }
        for (int x = 0; x < w; ++x) {
            uint8_t* col = bg.rgba.data() + size_t(x) * 4;
            for (int y = 0; y < h; ++y) std::memcpy(&line[size_t(y) * 4], col + size_t(y) * w * 4, 4);
            boxLine(line.data(), col, h, size_t(w) * 4, r);
        }
    }
}

// x / 255 rounded, exact for 0..65535 (a shift instead of a division, so blend loops vectorize)
inline uint32_t div255(uint32_t x)
{
    x += 128;
    return (x + (x >> 8)) >> 8;
}

// out = a * (1 - f) + b * f, f in 1/256 (separate pointers: the compiler may vectorize)
void mixRows(const uint8_t* __restrict a, const uint8_t* __restrict b, uint8_t* __restrict out, size_t n, uint32_t f)
{
    const uint32_t g = 256 - f;
    for (size_t i = 0; i < n; ++i) out[i] = uint8_t((a[i] * g + b[i] * f + 128) >> 8);
}

// Straight-alpha picture over an opaque background row, in place (an opaque pixel stays exactly as it is)
void blendOver(uint8_t* __restrict p, const uint8_t* __restrict s, int w)
{
    for (int i = 0; i < w; ++i) {
        const uint32_t a = p[i * 4 + 3], ia = 255 - a;
        p[i * 4] = uint8_t(div255(p[i * 4] * a + s[i * 4] * ia));
        p[i * 4 + 1] = uint8_t(div255(p[i * 4 + 1] * a + s[i * 4 + 1] * ia));
        p[i * 4 + 2] = uint8_t(div255(p[i * 4 + 2] * a + s[i * 4 + 2] * ia));
        p[i * 4 + 3] = 255;
    }
}

// First pixel of a row of w RGBA pixels whose alpha is not `alpha` (w = none); two pixels per step
int firstOther(const uint8_t* p, int w, uint8_t alpha)
{
    // The alpha bytes (offsets 3 and 7) of two pixels read as one 64-bit number
    constexpr uint64_t mask = std::endian::native == std::endian::little ? 0xFF000000FF000000ull : 0x000000FF000000FFull;
    const uint64_t want = mask / 0xFF * alpha;
    int x = 0;
    for (; x + 2 <= w; x += 2) {
        uint64_t v;
        std::memcpy(&v, p + size_t(x) * 4, 8);
        if ((v & mask) != want) break;
    }
    while (x < w && p[size_t(x) * 4 + 3] == alpha) ++x;
    return x;
}

// Source position (left edge) of small pixel i of n over [from, from + length)
int edge(double from, double length, int i, int n, int limit)
{
    return std::clamp(int(std::floor(from + length * i / n)), 0, limit);
}

} // namespace

Background makeBackground(const uint8_t* rgba, int w, int h, double pictureAspect, const double crop[4],
                          double frameAspect, double blurStrength, double dim)
{
    Background bg;
    if (!rgba || w <= 0 || h <= 0 || frameAspect <= 0) return bg;
    // Kept part of the picture (shares), its shape as displayed
    const double l = std::clamp(crop[0], 0.0, 0.99), t = std::clamp(crop[1], 0.0, 0.99);
    const double cw = std::max(0.01, 1 - l - std::max(0.0, crop[2])), ch = std::max(0.01, 1 - t - std::max(0.0, crop[3]));
    const double shape = (pictureAspect > 0 ? pictureAspect : double(w) / h) * cw / ch;
    // The part of it that covers the frame: full height of a wider picture, full width of a narrower one
    const double aw = shape > frameAspect ? cw * frameAspect / shape : cw;
    const double ah = shape > frameAspect ? ch : ch * shape / frameAspect;
    const double ax = (l + (cw - aw) / 2) * w, ay = (t + (ch - ah) / 2) * h;
    const double areaW = aw * w, areaH = ah * h;

    // Size: sigma of kSmallSigma pixels there (strength 100 = 1/20 of the frame height, like the blur effect)
    const double sigmaShare = std::clamp(blurStrength, 0.0, 100.0) / 2000.0;
    int sh = sigmaShare > 0 ? int(std::lround(kSmallSigma / sigmaShare)) : kMaxHeight;
    sh = std::clamp(sh, kMinHeight, std::max(kMinHeight, std::min(kMaxHeight, int(std::ceil(areaH)))));
    int sw = std::max(1, int(std::lround(sh * frameAspect)));
    if (sw > kMaxWidth) {
        sh = std::max(1, int(std::lround(sh * double(kMaxWidth) / sw)));
        sw = kMaxWidth;
    }
    bg.width = sw;
    bg.height = sh;
    bg.rgba.resize(size_t(sw) * sh * 4);

    // Average down (premultiplied); in big footprints every few pixels are enough, the blur follows
    std::vector<int> xs(size_t(sw) + 1), ys(size_t(sh) + 1);
    for (int i = 0; i <= sw; ++i) xs[size_t(i)] = edge(ax, areaW, i, sw, w);
    for (int j = 0; j <= sh; ++j) ys[size_t(j)] = edge(ay, areaH, j, sh, h);
    const int stepX = std::max(1, int(areaW / sw / 4)), stepY = std::max(1, int(areaH / sh / 4));
    bool opaque = true;
    for (int j = 0; j < sh; ++j) {
        const int y0 = std::min(ys[size_t(j)], h - 1), y1 = std::max(y0 + 1, ys[size_t(j) + 1]);
        for (int i = 0; i < sw; ++i) {
            const int x0 = std::min(xs[size_t(i)], w - 1), x1 = std::max(x0 + 1, xs[size_t(i) + 1]);
            uint64_t sum[3] = {}, alpha = 0, n = 0;
            for (int y = y0; y < y1 && y < h; y += stepY) {
                const uint8_t* p = rgba + (size_t(y) * w + x0) * 4;
                for (int x = x0; x < x1 && x < w; x += stepX, p += size_t(stepX) * 4) {
                    const unsigned a = p[3];
                    sum[0] += p[0] * a;
                    sum[1] += p[1] * a;
                    sum[2] += p[2] * a;
                    alpha += a;
                    ++n;
                }
            }
            uint8_t* d = &bg.rgba[(size_t(j) * sw + i) * 4];
            if (!n) {
                std::memset(d, 0, 4);
                opaque = false;
                continue;
            }
            for (int c = 0; c < 3; ++c) d[c] = uint8_t((sum[c] + n * 255 / 2) / (n * 255));
            d[3] = uint8_t((alpha + n / 2) / n);
            opaque = opaque && d[3] == 255;
        }
    }
    bg.opaque = opaque;
    blur(bg, sigmaShare * sh);
    // Darker: premultiplied color times (1 - dim)
    const int keep = int(std::lround(256 * (1 - std::clamp(dim, 0.0, 100.0) / 100.0)));
    if (keep < 256)
        for (size_t i = 0; i < bg.rgba.size(); i += 4)
            for (size_t c = 0; c < 3; ++c) bg.rgba[i + c] = uint8_t((bg.rgba[i + c] * keep + 128) >> 8);
    return bg;
}

void composite(uint8_t* rgba, int w, int h, const Background& bg)
{
    const int bw = bg.width, bh = bg.height;
    if (!rgba || w <= 0 || h <= 0 || bw <= 0 || bh <= 0 || bg.rgba.size() < size_t(bw) * bh * 4) return;
    // Bilinear: for each output column/row the two background pixels and the weight of the second (0..256)
    struct Tap {
        int a, b;
        uint32_t f;
    };
    auto taps = [](int out, int in) {
        std::vector<Tap> v(static_cast<size_t>(out));
        for (int i = 0; i < out; ++i) {
            const double u = (i + 0.5) * in / out - 0.5;
            const int a = std::clamp(int(std::floor(u)), 0, in - 1);
            const double f = u < 0 ? 0.0 : std::clamp(u - a, 0.0, 1.0);
            v[size_t(i)] = {a, std::min(a + 1, in - 1), uint32_t(std::lround(f * 256))};
        }
        return v;
    };
    const std::vector<Tap> cols = taps(w, bw), rows = taps(h, bh);
    // The background rows stretched to the output width, each once when a row first needs it; then every output
    // row is one vertical mix of two of them and one blend, both plain loops the compiler vectorizes
    // (per-pixel taps took ~19 ms for a 1080x1920 frame, this ~2)
    std::vector<uint8_t> wide(size_t(bh) * w * 4);
    std::vector<char> ready(size_t(bh), 0);
    auto wideRow = [&](int j) -> const uint8_t* {
        uint8_t* out = &wide[size_t(j) * w * 4];
        if (ready[size_t(j)]) return out;
        ready[size_t(j)] = 1;
        const uint8_t* src = &bg.rgba[size_t(j) * bw * 4];
        for (int x = 0; x < w; ++x) {
            const Tap& c = cols[size_t(x)];
            const uint8_t* a = src + size_t(c.a) * 4;
            const uint8_t* b = src + size_t(c.b) * 4;
            for (int k = 0; k < 4; ++k) out[size_t(x) * 4 + k] = uint8_t((a[k] * (256 - c.f) + b[k] * c.f + 128) >> 8);
        }
        return out;
    };
    std::vector<uint8_t> back(size_t(w) * 4);
    for (int y = 0; y < h; ++y) {
        uint8_t* p = rgba + size_t(y) * w * 4;
        int x = firstOther(p, w, 255);
        if (x == w) continue; // the picture covers this row
        const Tap& r = rows[size_t(y)];
        const uint8_t* top = wideRow(r.a);
        const uint8_t* bottom = wideRow(r.b);
        if (x == 0 && bg.opaque && firstOther(p, w, 0) == w) { // a bar: none of the picture, the background straight in
            mixRows(top, bottom, p, size_t(w) * 4, r.f);
            continue;
        }
        mixRows(top, bottom, back.data(), back.size(), r.f);
        if (bg.opaque) {
            blendOver(p, back.data(), w);
            continue;
        }
        for (; x < w; ++x) {
            // Picture over a (partly) transparent background: premultiplied "over", then straight again
            uint8_t* q = p + size_t(x) * 4;
            const uint8_t* s = &back[size_t(x) * 4];
            const uint32_t fa = q[3];
            if (fa == 255) continue;
            const uint32_t outA = fa + div255(s[3] * (255 - fa));
            if (!outA) {
                std::memset(q, 0, 4);
                continue;
            }
            for (int k = 0; k < 3; ++k) {
                const uint32_t premul = div255(q[k] * fa + s[k] * (255 - fa));
                q[k] = uint8_t(std::min<uint32_t>(255, (premul * 255 + outA / 2) / outA));
            }
            q[3] = uint8_t(outA);
        }
    }
}

namespace {

constexpr const char* kFrameData = "_schneidi_blurfill";

void deleteBackground(void* p) { delete static_cast<Background*>(p); }

int captureGetImage(mlt_frame frame, uint8_t** image, mlt_image_format* format, int* width, int* height, int writable)
{
    auto filter = static_cast<mlt_filter>(mlt_frame_pop_service(frame));
    const int pos = mlt_frame_pop_service_int(frame);
    const int len = mlt_frame_pop_service_int(frame);
    // qtblend asks for RGBA anyway; asking for it here too keeps the copy exact in its pass-through cases
    *format = mlt_image_rgba;
    const int err = mlt_frame_get_image(frame, image, format, width, height, writable);
    if (err || !*image || *format != mlt_image_rgba) return err;
    mlt_properties fp = MLT_FILTER_PROPERTIES(filter);
    const mlt_rect r = mlt_properties_anim_get_rect(fp, "crop", pos, len);
    const double crop[4] = {r.x, r.y, r.w, r.h};
    auto* bg = new Background(makeBackground(*image, *width, *height, mlt_properties_get_double(fp, "picture_aspect"), crop,
                                             mlt_properties_get_double(fp, "frame_aspect"),
                                             mlt_properties_get_double(fp, "blur"), mlt_properties_get_double(fp, "dim")));
    mlt_properties_set_data(MLT_FRAME_PROPERTIES(frame), kFrameData, bg, 0, deleteBackground, nullptr);
    return 0;
}

mlt_frame captureProcess(mlt_filter filter, mlt_frame frame)
{
    mlt_frame_push_service_int(frame, int(mlt_filter_get_length2(filter, frame)));
    mlt_frame_push_service_int(frame, int(mlt_filter_get_position(filter, frame)));
    mlt_frame_push_service(frame, filter);
    mlt_frame_push_get_image(frame, captureGetImage);
    return frame;
}

int compositeGetImage(mlt_frame frame, uint8_t** image, mlt_image_format* format, int* width, int* height, int)
{
    mlt_frame_pop_service(frame);
    *format = mlt_image_rgba;
    const int err = mlt_frame_get_image(frame, image, format, width, height, 1);
    if (err || !*image || *format != mlt_image_rgba) return err;
    if (auto* bg = static_cast<Background*>(mlt_properties_get_data(MLT_FRAME_PROPERTIES(frame), kFrameData, nullptr)))
        composite(*image, *width, *height, *bg);
    return 0;
}

mlt_frame compositeProcess(mlt_filter filter, mlt_frame frame)
{
    mlt_frame_push_service(frame, filter);
    mlt_frame_push_get_image(frame, compositeGetImage);
    return frame;
}

Mlt::Filter ownFilter(mlt_frame (*process)(mlt_filter, mlt_frame))
{
    mlt_filter f = mlt_filter_new();
    if (!f) return Mlt::Filter();
    f->process = process;
    Mlt::Filter filter(f); // holds its own reference
    mlt_filter_close(f);
    return filter;
}

} // namespace

Mlt::Filter captureFilter(Mlt::Profile& profile, double pictureAspect, double blurStrength, double dim)
{
    Mlt::Filter f = ownFilter(captureProcess);
    if (!f.is_valid()) return f;
    // Numbers as doubles (never text: MLT would read them with the system's number format)
    f.set("picture_aspect", pictureAspect);
    f.set("frame_aspect", double(profile.width()) / std::max(1, profile.height()));
    f.set("blur", blurStrength);
    f.set("dim", dim);
    return f;
}

Mlt::Filter compositeFilter() { return ownFilter(compositeProcess); }

} // namespace BlurFill
