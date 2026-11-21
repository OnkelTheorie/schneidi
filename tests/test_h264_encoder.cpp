// SPDX-License-Identifier: AGPL-3.0-only
// schneidi's own H.264 encoder (prototype): every stream must decode with FFmpeg's decoder to exactly the
// pictures the encoder reconstructed (CAVLC and CABAC; I and P frames, all intra/inter modes, slices, adaptive QP,
// rate control, odd sizes). With B pictures the access units come in coding order: the decoder must hand the
// pictures out in display order with the right timestamps. The psycho-visual mode decision (psyRd): off = the same
// stream as without the option, on = decodable, the same with any thread count, and closer to the source's texture.

#include "check.h"

#include "engine/encoder/h264/Cavlc.h"
#include "engine/encoder/h264/Deblock.h"
#include "engine/encoder/h264/Satd.h"
#include "engine/encoder/h264/Transform.h"
#include "engine/encoder/h264/H264Encoder.h"
#include "engine/encoder/h264/ThreadErrors.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace schneidi::h264;

namespace {

struct Yuv {
    int w = 0, h = 0;
    std::vector<uint8_t> y, u, v;
};

// > 0: the synthetic clip fades in from black over this many pictures (weighted prediction).
int gFadeFrames = 0;
// Picture kind of synthFrame: 0 = the moving pictures below, 1 = still (only the noise changes), 2 = a slow zoom.
int gSynthKind = 0;
// > 1: every picture of the synthetic clip is shown this many times (a recording at a higher frame rate than the game's).
int gSynthHold = 0;
// > 0: the synthetic clip stands for this many pictures (only a faint noise changes) before it starts to move.
int gSynthStart = 0;

// Moving gradient, a moving textured square and a little noise: exercises intra modes, motion and skip.
// A texture under a slow zoom with a little noise (gSynthKind 2): the material B pictures pay on.
int zoomSample(int x, int y, int w, int h, int t)
{
    auto lattice = [](int ix, int iy) {
        uint32_t s = uint32_t(ix) * 73856093u ^ uint32_t(iy) * 19349663u;
        s = (s ^ (s >> 13)) * 1274126177u;
        return int((s >> 16) & 255);
    };
    // Positions in 1/256 pixel: the picture's centre stays, the scale grows by 0.15 % per picture. Lattice cells
    // of 6 pixels, bilinear between their corners.
    const int scale = 65536 + t * 100;
    const int64_t u = (int64_t(x * 2 - w) * 128 * 65536 / scale + int64_t(w) * 128) / 6;
    const int64_t v = (int64_t(y * 2 - h) * 128 * 65536 / scale + int64_t(h) * 128) / 6;
    const int ix = int(u >> 8), iy = int(v >> 8), fx = int(u & 255), fy = int(v & 255);
    const int top = lattice(ix, iy) * (256 - fx) + lattice(ix + 1, iy) * fx;
    const int bottom = lattice(ix, iy + 1) * (256 - fx) + lattice(ix + 1, iy + 1) * fx;
    return (top * (256 - fy) + bottom * fy) >> 16;
}

Yuv synthFrame(int w, int h, int t)
{
    if (gSynthHold > 1)
        t -= t % gSynthHold;
    const int moved = std::max(0, t - gSynthStart);
    const int tm = gSynthKind != 0 ? 0 : moved;
    Yuv f;
    f.w = w;
    f.h = h;
    f.y.resize(size_t(w) * h);
    f.u.resize(size_t(w / 2) * (h / 2));
    f.v.resize(f.u.size());
    uint32_t seed = 12345u + uint32_t(t) * 7919u;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            seed = seed * 1664525u + 1013904223u;
            int v = (x * 255) / w / 2 + (y * 255) / h / 3 + int(seed >> 29);
            const int sx = (tm * 3) % w, sy = (tm * 2) % h;
            if (gSynthKind == 2)
                v = 16 + zoomSample(x, y, w, h, moved) * 7 / 8 + int(seed >> (t < gSynthStart ? 31 : 30)); // (standing: the faintest noise)
            else if (x >= sx && x < sx + 48 && y >= sy && y < sy + 40)
                v = ((x / 4 + y / 4) & 1) ? 220 : 40;
            f.y[size_t(y) * w + x] = uint8_t(std::clamp(v, 0, 255));
        }
    for (int y = 0; y < h / 2; ++y)
        for (int x = 0; x < w / 2; ++x) {
            f.u[size_t(y) * (w / 2) + x] = uint8_t(128 + (x - w / 4) / 3 + tm);
            f.v[size_t(y) * (w / 2) + x] = uint8_t(128 + (y - h / 4) / 2);
        }
    if (gFadeFrames > 0 && t < gFadeFrames) {
        const int num = t, den = gFadeFrames;
        for (uint8_t& p : f.y)
            p = uint8_t(16 + (int(p) - 16) * num / den);
        for (uint8_t& p : f.u)
            p = uint8_t(128 + (int(p) - 128) * num / den);
        for (uint8_t& p : f.v)
            p = uint8_t(128 + (int(p) - 128) * num / den);
    }
    return f;
}

// Encodes `frames` pictures, decodes them with libavcodec and compares with the reconstruction.
// With a chunk plan the parts go through their own encoders and the streams are concatenated.
// B pictures (cfg.bframes): the reconstructions are kept by display index, the packets carry the display index as
// pts, and the decoder (with its reordering delay) must return picture i with pts i. `info` reports what came out.
struct StreamInfo {
    std::string types;      // picture types in coding order
    std::vector<int> order; // display index of each access unit, in coding order
    std::vector<size_t> sizes; // bytes of each access unit, in coding order
    std::vector<int> qps;      // QP of each access unit's picture, in coding order
    double minPsnrY = 99;   // lowest luma PSNR of a reconstruction against its source picture
    uint64_t hash = 1469598103934665603ull; // FNV-1a of the whole stream
    // Psy distance (hadamardMagsDistance) between the source's and the reconstruction's whole 8x8 luma blocks,
    // summed over all pictures, and the same against flat blocks (all of the source's texture lost)
    double psyDistance = 0, psyFlat = 0;
    // Fine detail of the B pictures' reconstructions: the 4x4 Hadamard AC magnitudes of their 8x8 luma blocks, summed
    double detailB = 0;
};

// EncodeHints for picture t of the next roundTrip calls (nullptr = none), and what the encoder counted in the last one.
std::function<const EncodeHints*(int)> gHints;
Encoder::HintStats gHintStats;

bool roundTrip(const EncoderConfig& cfg, int frames, size_t* bytesOut = nullptr, int keyframeAt = -1,
               const ChunkPlan* plan = nullptr, StreamInfo* info = nullptr)
{
    auto encPtr = std::make_unique<Encoder>(plan ? plan->configFor(cfg, 0, cfg.threads) : cfg);
    Encoder& enc0 = *encPtr;
    (void)enc0;
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec)
        return false;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (cfg.bframes <= 0)
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->thread_count = 1;
    if (avcodec_open2(ctx, codec, nullptr) < 0)
        return false;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    std::map<int, Yuv> recon; // by display index
    size_t decoded = 0, bytes = 0;
    int chunkBase = 0, packets = 0;
    bool ok = true;
    auto receive = [&] {
        while (avcodec_receive_frame(ctx, frame) == 0) {
            if (!recon.count(int(decoded)) || frame->pts != int64_t(decoded)) {
                std::fprintf(stderr, "picture %zu: not coded yet or wrong pts %lld\n", decoded, static_cast<long long>(frame->pts));
                ok = false;
                return;
            }
            const Yuv& r = recon[int(decoded++)];
            for (int p = 0; p < 3; ++p) {
                const int pw = p ? r.w / 2 : r.w, ph = p ? r.h / 2 : r.h;
                const std::vector<uint8_t>& ref = p == 0 ? r.y : p == 1 ? r.u : r.v;
                for (int y = 0; y < ph; ++y)
                    if (std::memcmp(frame->data[p] + size_t(y) * frame->linesize[p], ref.data() + size_t(y) * pw,
                                    size_t(pw)) != 0) {
                        std::fprintf(stderr, "frame %zu plane %d row %d differs\n", decoded - 1, p, y);
                        ok = false;
                        return;
                    }
            }
        }
    };

    // The encoder may hold pictures back (lookahead): feed all, then flush.
    auto take = [&](std::vector<uint8_t> au) {
        if (au.empty())
            return;
        bytes += au.size();
        if (info)
            for (const uint8_t b : au)
                info->hash = (info->hash ^ b) * 1099511628211ull;
        Yuv r;
        r.w = cfg.width;
        r.h = cfg.height;
        const int pw = encPtr->paddedWidth();
        for (int y = 0; y < r.h; ++y)
            r.y.insert(r.y.end(), encPtr->reconY().begin() + y * pw, encPtr->reconY().begin() + y * pw + r.w);
        for (int y = 0; y < r.h / 2; ++y) {
            r.u.insert(r.u.end(), encPtr->reconU().begin() + y * (pw / 2), encPtr->reconU().begin() + y * (pw / 2) + r.w / 2);
            r.v.insert(r.v.end(), encPtr->reconV().begin() + y * (pw / 2), encPtr->reconV().begin() + y * (pw / 2) + r.w / 2);
        }
        const int display = chunkBase + encPtr->lastDisplayIndex();
        if (recon.count(display)) {
            std::fprintf(stderr, "picture %d came out twice\n", display);
            ok = false;
        }
        if (info) {
            info->types += encPtr->lastType();
            info->order.push_back(display);
            info->sizes.push_back(au.size());
            info->qps.push_back(encPtr->lastQp());
            const Yuv src = synthFrame(cfg.width, cfg.height, display);
            double sse = 0;
            for (size_t i = 0; i < src.y.size(); ++i)
                sse += double(int(src.y[i]) - int(r.y[i])) * double(int(src.y[i]) - int(r.y[i]));
            info->minPsnrY = std::min(info->minPsnrY, sse > 0 ? 10.0 * std::log10(255.0 * 255.0 * double(src.y.size()) / sse) : 99.0);
            for (int y = 0; y + 8 <= r.h; y += 8)
                for (int x = 0; x + 8 <= r.w; x += 8) {
                    alignas(16) int16_t ms[128], mr[128];
                    alignas(16) static const int16_t kFlat[128] = {};
                    hadamardMags8x8U8(&src.y[size_t(y) * r.w + x], r.w, ms);
                    hadamardMags8x8U8(&r.y[size_t(y) * r.w + x], r.w, mr);
                    info->psyDistance += hadamardMagsDistance(ms, mr);
                    info->psyFlat += hadamardMagsDistance(ms, kFlat);
                    if (info->types.back() == 'B')
                        for (int i = 0; i < 64; ++i)
                            info->detailB += mr[i];
                }
        }
        recon[display] = std::move(r);

        av_new_packet(pkt, int(au.size()));
        std::memcpy(pkt->data, au.data(), au.size());
        pkt->pts = display;
        pkt->dts = packets++ - encPtr->reorderDelay();
        if (avcodec_send_packet(ctx, pkt) < 0)
            ok = false;
        av_packet_unref(pkt);
        receive();
    };
    std::vector<uint8_t> first = encPtr->headers();
    size_t chunk = 0;
    for (int t = 0; t < frames && ok; ++t) {
        if (plan && chunk + 1 < plan->start.size() && t == plan->start[chunk + 1]) {
            while (ok) { // the rest of the chunk before, then the next encoder
                std::vector<uint8_t> au = encPtr->flush();
                if (au.empty())
                    break;
                au.insert(au.begin(), first.begin(), first.end());
                first.clear();
                take(std::move(au));
            }
            ++chunk;
            chunkBase = t;
            encPtr = std::make_unique<Encoder>(plan->configFor(cfg, int(chunk), cfg.threads));
        }
        const Yuv src = synthFrame(cfg.width, cfg.height, t);
        Picture pic{src.y.data(), src.u.data(), src.v.data(), src.w, src.w / 2};
        std::vector<uint8_t> au = encPtr->encode(pic, t == keyframeAt, gHints ? gHints(t) : nullptr);
        if (au.empty())
            continue;
        au.insert(au.begin(), first.begin(), first.end()); // SPS/PPS in front of the first access unit
        first.clear();
        take(std::move(au));
    }
    while (ok) {
        std::vector<uint8_t> au = encPtr->flush();
        if (au.empty())
            break;
        au.insert(au.begin(), first.begin(), first.end());
        first.clear();
        take(std::move(au));
    }
    avcodec_send_packet(ctx, nullptr);
    receive();
    gHintStats = encPtr->hintStats();
    ok = ok && decoded == size_t(frames);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    if (bytesOut)
        *bytesOut = bytes;
    return ok;
}

bool gCabac = true; // every case runs with both entropy coders

// The coded picture buffer as a decoder has it (Annex C, VBR, one picture per 1/fps in coding order): access units
// that arrive late (the buffer ran empty), the lowest level after a removal in bits. The first access unit's size
// includes the SPS/PPS.
int cpbUnderflows(const std::vector<size_t>& sizes, double fps, int maxrateKbps, int bufsizeKbits, double init, double* minLevel = nullptr)
{
    const double rate = std::ceil(maxrateKbps * 1000.0 / 64.0) * 64.0, size = std::floor(bufsizeKbits * 1000.0 / 16.0) * 16.0;
    double fill = std::floor(90000.0 * init * size / rate) * rate / 90000.0, low = size;
    int under = 0;
    for (size_t bytes : sizes) {
        fill -= double(bytes) * 8.0;
        under += fill < 0;
        low = std::min(low, fill);
        fill = std::min(fill + rate / fps, size);
    }
    if (minLevel)
        *minLevel = low;
    return under;
}

EncoderConfig config(int w, int h, IntraMode mode, int gop, int threads = 1)
{
    EncoderConfig c;
    c.cabac = gCabac;
    c.width = w;
    c.height = h;
    c.mode = mode;
    c.gop = gop;
    c.threads = threads;
    c.qp = 28;
    // The cases set what they test: no B pictures, the plain AQ strength and no psy unless asked for (the
    // defaults since 14.11.2026 are 8 B pictures with the probe, AQ 1.5, psy 1.0 by QP; a case below runs them).
    c.bframes = 0;
    c.aqStrength = 1.0;
    c.psyRd = 0.0;
    c.bSharp = 0.0; // B pictures average both lists wherever that is cheapest (1.5 since 16.11.2026; a case below)
    c.startBoostQp = 0.0; // two-pass: no finer start of the clip (2 QP since 16.11.2026; a case below)
    return c;
}

// The edge kernels (SIMD where available) against the per-line reference filter, and the
// four-predictions SATD against single calls, on random edges and samples.
bool kernelsMatchReference()
{
    using namespace deblock_detail;
    uint32_t seed = 12345;
    auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    constexpr int S = 40;
    for (int it = 0; it < 20000; ++it) {
        std::vector<uint8_t> a(S * S), c(S * S);
        const int base = int(rnd() % 256), amp = 1 + int(rnd() % (it % 3 == 0 ? 255 : 20));
        for (auto* v : {&a, &c})
            for (auto& x : *v)
                x = uint8_t(std::clamp(base + int(rnd() % unsigned(2 * amp + 1)) - amp, 0, 255));
        DeblockEdge e;
        e.alpha = int(rnd() % (it % 4 == 0 ? 40 : 256));
        e.beta = int(rnd() % 19);
        e.bs4 = rnd() % 3 == 0;
        for (auto& t : e.tc0)
            t = e.bs4 ? int8_t(0) : rnd() % 4 == 0 ? int8_t(-1) : int8_t(rnd() % 26);
        std::vector<uint8_t> ra = a, rc = c;
        uint8_t *q = &a[10 * S + 12], *qv = &c[10 * S + 12], *r = &ra[10 * S + 12], *rv = &rc[10 * S + 12];
        switch (it % 4) {
        case 0:
            deblockLumaEdgeV(q, S, e);
            for (int i = 0; i < 16; ++i)
                if (e.bs4 || e.tc0[i >> 2] >= 0)
                    lumaLine(r + i * S, 1, e.alpha, e.beta, e.tc0[i >> 2], e.bs4);
            break;
        case 1:
            deblockLumaEdgeH(q, S, e);
            for (int i = 0; i < 16; ++i)
                if (e.bs4 || e.tc0[i >> 2] >= 0)
                    lumaLine(r + i, S, e.alpha, e.beta, e.tc0[i >> 2], e.bs4);
            break;
        case 2:
            deblockChromaEdgeV(q, qv, S, e);
            for (int i = 0; i < 8; ++i)
                if (e.bs4 || e.tc0[i >> 1] >= 0) {
                    chromaLine(r + i * S, 1, e.alpha, e.beta, e.tc0[i >> 1], e.bs4);
                    chromaLine(rv + i * S, 1, e.alpha, e.beta, e.tc0[i >> 1], e.bs4);
                }
            break;
        default:
            deblockChromaEdgeH(q, qv, S, e);
            for (int i = 0; i < 8; ++i)
                if (e.bs4 || e.tc0[i >> 1] >= 0) {
                    chromaLine(r + i, S, e.alpha, e.beta, e.tc0[i >> 1], e.bs4);
                    chromaLine(rv + i, S, e.alpha, e.beta, e.tc0[i >> 1], e.bs4);
                }
            break;
        }
        if (a != ra || c != rc)
            return false;
        if (it % 8 == 0) {
            // Psy distance of two 8x8 blocks against plain Hadamard transforms: the coefficients' magnitudes
            // compared one by one, without the DC ones
            alignas(16) int16_t ma[128], mc[128];
            hadamardMags8x8U8(a.data(), S, ma);
            hadamardMags8x8U8(c.data(), S, mc);
            int h[2][64], d4 = 0, d8 = 0;
            for (int w = 0; w < 2; ++w)
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; ++x)
                        h[w][y * 8 + x] = (w ? c : a)[size_t(y) * S + x];
            for (int q = 0; q < 4; ++q) {
                int in[2][16], out[2][16];
                for (int w = 0; w < 2; ++w) {
                    for (int i = 0; i < 16; ++i)
                        in[w][i] = h[w][((q >> 1) * 4 + (i >> 2)) * 8 + (q & 1) * 4 + (i & 3)];
                    hadamard4x4(in[w], out[w]);
                }
                for (int i = 1; i < 16; ++i)
                    d4 += std::abs(std::abs(out[0][i]) - std::abs(out[1][i]));
            }
            for (int w = 0; w < 2; ++w)
                for (int pass = 0; pass < 2; ++pass)
                    for (int line = 0; line < 8; ++line)
                        for (int step = 1; step < 8; step <<= 1)
                            for (int i = 0; i < 8; i += 2 * step)
                                for (int j = i; j < i + step; ++j) {
                                    int& u = pass ? h[w][j * 8 + line] : h[w][line * 8 + j];
                                    int& v = pass ? h[w][(j + step) * 8 + line] : h[w][line * 8 + j + step];
                                    const int p = u, m = v;
                                    u = p + m;
                                    v = p - m;
                                }
            for (int i = 1; i < 64; ++i)
                d8 += std::abs(std::abs(h[0][i]) - std::abs(h[1][i]));
            if (hadamardMagsDistance(ma, mc) != ((d4 + 1) >> 1) + ((d8 + 2) >> 2) || hadamardMagsDistance(ma, ma) != 0)
                return false;
            int four[4];
            const uint8_t* pred = c.data(); // 16 x 4 at stride 16
            satd4x4x4U8(a.data(), S, pred, four);
            for (int i = 0; i < 4; ++i)
                if (four[i] != satdU8(a.data(), S, pred + 4 * i, 16))
                    return false;
        }
    }
    return true;
}

// The distortion, transform and quantisation kernels (SSE2 / AVX2 / NEON where available) against
// plain references written out here, on random blocks: the stream must not depend on the platform.
bool vectorKernelsMatchReference()
{
    using namespace transform_detail;
    uint32_t seed = 777;
    auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    constexpr int S = 48;
    std::vector<uint8_t> a(S * 24), b(S * 24);
    auto satdRef = [&](const uint8_t* p, int sp, const uint8_t* q, int sq, int w, int h) {
        int s = 0;
        for (int y = 0; y < h; y += 4)
            for (int x = 0; x < w; x += 4) {
                int d[16];
                for (int i = 0; i < 16; ++i)
                    d[i] = int(p[(y + i / 4) * sp + x + i % 4]) - int(q[(y + i / 4) * sq + x + i % 4]);
                s += satd4x4(d);
            }
        return s;
    };
    for (int it = 0; it < 4000; ++it) {
        const int base = int(rnd() % 256), amp = it % 3 == 0 ? 255 : 1 + int(rnd() % 30);
        for (auto* v : {&a, &b})
            for (auto& x : *v)
                x = it % 5 == 4 ? uint8_t(rnd() & 1 ? 255 : 0) : uint8_t(std::clamp(base + int(rnd() % unsigned(2 * amp + 1)) - amp, 0, 255));
        const uint8_t* p = a.data() + it % 3;
        const uint8_t* q = b.data() + it % 5;
        const int sp = S - it % 2, sq = S;
        if (satdU8(p, sp, q, sq) != satdRef(p, sp, q, sq, 4, 4) || satd8x4U8(p, sp, q, sq) != satdRef(p, sp, q, sq, 8, 4) ||
            satd8x8U8(p, sp, q, sq) != satdRef(p, sp, q, sq, 8, 8) || satd8x8U8Fast(p, sp, q, sq) != satdRef(p, sp, q, sq, 8, 8) ||
            satd16x4U8(p, sp, q, sq) != satdRef(p, sp, q, sq, 16, 4))
            return false;
        for (int w : {8, 16})
            for (int h : {2, 4, 8, 16}) {
                int ref = 0;
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                        ref += std::abs(int(p[y * sp + x]) - int(q[y * sq + x]));
                if (sadBlock(p, sp, q, sq, w, h) != ref)
                    return false;
            }
        for (int w : {4, 8, 16}) {
            int ref = 0;
            for (int x = 0; x < w; ++x)
                ref += std::abs(int(p[x]) - int(q[x]));
            if (sadRow(p, q, w) != ref)
                return false;
        }
        { // sa8d: 8-point Hadamard both ways, (sum + 2) >> 2
            int d[64];
            for (int i = 0; i < 64; ++i)
                d[i] = int(p[(i / 8) * sp + i % 8]) - int(q[(i / 8) * sq + i % 8]);
            for (int pass = 0; pass < 2; ++pass)
                for (int line = 0; line < 8; ++line)
                    for (int step = 1; step < 8; step <<= 1)
                        for (int i = 0; i < 8; i += 2 * step)
                            for (int j = i; j < i + step; ++j) {
                                int& u = pass ? d[j * 8 + line] : d[line * 8 + j];
                                int& v = pass ? d[(j + step) * 8 + line] : d[line * 8 + j + step];
                                const int x = u, y = v;
                                u = x + y;
                                v = x - y;
                            }
            int sum = 0;
            for (int v : d)
                sum += std::abs(v);
            if (sa8dU8(p, sp, q, sq) != (sum + 2) >> 2)
                return false;
        }
        // Transforms: forward 4x4 / 8x8 against the scalar cores, inverses against the standard's steps.
        int res[64], out[64], ref[64];
        for (int i = 0; i < 64; ++i)
            res[i] = int(p[(i / 8) * sp + i % 8]) - int(q[(i / 8) * sq + i % 8]);
        {
            int r16[16];
            for (int i = 0; i < 16; ++i)
                r16[i] = res[(i / 4) * 8 + i % 4];
            int t[16];
            for (int y = 0; y < 4; ++y) {
                const int* r = r16 + y * 4;
                t[y * 4 + 0] = r[0] + r[1] + r[2] + r[3];
                t[y * 4 + 1] = 2 * (r[0] - r[3]) + (r[1] - r[2]);
                t[y * 4 + 2] = r[0] - r[1] - r[2] + r[3];
                t[y * 4 + 3] = (r[0] - r[3]) - 2 * (r[1] - r[2]);
            }
            for (int x = 0; x < 4; ++x) {
                ref[x] = t[x] + t[4 + x] + t[8 + x] + t[12 + x];
                ref[4 + x] = 2 * (t[x] - t[12 + x]) + (t[4 + x] - t[8 + x]);
                ref[8 + x] = t[x] - t[4 + x] - t[8 + x] + t[12 + x];
                ref[12 + x] = (t[x] - t[12 + x]) - 2 * (t[4 + x] - t[8 + x]);
            }
            forward4x4(r16, out);
            if (!std::equal(out, out + 16, ref))
                return false;
            forward4x4Diff(p, sp, q, sq, out);
            if (!std::equal(out, out + 16, ref))
                return false;
        }
        {
            int t[64];
            for (int y = 0; y < 8; ++y)
                fwd8c(res + y * 8, t + y * 8);
            for (int j = 0; j < 8; ++j) {
                int col[8], o[8];
                for (int y = 0; y < 8; ++y)
                    col[y] = t[y * 8 + j];
                fwd8c(col, o);
                for (int i = 0; i < 8; ++i)
                    ref[i * 8 + j] = o[i];
            }
            forward8x8(res, out);
            if (!std::equal(out, out + 64, ref))
                return false;
        }
        int coef[64];
        for (int& c : coef)
            c = int(rnd() % 8001) - 4000;
        {
            int t[64];
            for (int i = 0; i < 8; ++i)
                inv8c(coef + i * 8, t + i * 8);
            for (int j = 0; j < 8; ++j) {
                int col[8], o[8];
                for (int i = 0; i < 8; ++i)
                    col[i] = t[i * 8 + j];
                inv8c(col, o);
                for (int i = 0; i < 8; ++i)
                    ref[i * 8 + j] = (o[i] + 32) >> 6;
            }
            inverse8x8(coef, out);
            if (!std::equal(out, out + 64, ref))
                return false;
            for (int y = 0; y < 4; ++y) {
                const int* d = coef + y * 4;
                const int e = d[0] + d[2], f = d[0] - d[2], g = (d[1] >> 1) - d[3], h = d[1] + (d[3] >> 1);
                t[y * 4 + 0] = e + h;
                t[y * 4 + 1] = f + g;
                t[y * 4 + 2] = f - g;
                t[y * 4 + 3] = e - h;
            }
            for (int x = 0; x < 4; ++x) {
                const int e = t[x] + t[8 + x], f = t[x] - t[8 + x];
                const int g = (t[4 + x] >> 1) - t[12 + x], h = t[4 + x] + (t[12 + x] >> 1);
                ref[x] = (e + h + 32) >> 6;
                ref[4 + x] = (f + g + 32) >> 6;
                ref[8 + x] = (f - g + 32) >> 6;
                ref[12 + x] = (e - h + 32) >> 6;
            }
            inverse4x4(coef, out);
            if (!std::equal(out, out + 16, ref))
                return false;
        }
        { // quantisation, dequantisation, the threshold test, adding the residual back
            int mf[8], lim[8], lv[16], dv[8];
            for (int i = 0; i < 8; ++i) {
                mf[i] = 1 + int(rnd() % 13107);
                lim[i] = int(rnd() % 300);
                dv[i] = 16 * (10 + int(rnd() % 16));
            }
            const int qbits = 15 + it % 9, offset = (1 << qbits) / (it % 2 ? 3 : 6);
            bool any = false;
            for (int i = 0; i < 16; ++i) {
                const int m = std::abs(coef[i]);
                const int l = std::min(int((int64_t(m) * mf[i & 7] + offset) >> qbits), 2063);
                ref[i] = coef[i] < 0 ? -l : l;
                any |= l != 0;
            }
            if (quant4x4(coef, mf, qbits, offset, 2063, lv) != any || !std::equal(lv, lv + 16, ref))
                return false;
            bool above = false;
            for (int i = 0; i < 16; ++i)
                above |= std::abs(coef[i]) > lim[i & 7];
            if (anyAbove4x4(coef, lim) != above)
                return false;
            const int shift = it % 4;
            dequant4x4(lv, dv, shift, out);
            for (int i = 0; i < 16; ++i)
                if (out[i] != int(unsigned(lv[i] * dv[i & 7]) << shift))
                    return false;
            uint8_t o8[64];
            for (int& r : res)
                r = int(rnd() % 1201) - 600;
            addClip8x8(p, sp, res, o8, 8);
            for (int i = 0; i < 64; ++i)
                if (o8[i] != std::clamp(int(p[(i / 8) * sp + i % 8]) + res[i], 0, 255))
                    return false;
            addClip4x4(p, sp, res, o8, 4);
            for (int i = 0; i < 16; ++i)
                if (o8[i] != std::clamp(int(p[(i / 4) * sp + i % 4]) + res[i], 0, 255))
                    return false;
        }
    }
    return true;
}

} // namespace

void testVbv()
{
    // VBV (vbvMaxrateKbps / vbvBufsizeKbits): a clip whose unconstrained stream runs a half-second buffer empty
    // again and again (crf 12, twice the peak rate, a keyframe in the middle) must never do so with VBV - with
    // constant quality, one-pass and two-pass bitrate, B pictures, frame threading and CAVLC - and still decode
    // bit-exact with the HRD SEI; the stream does not depend on the thread count; VBV off (either value 0) is the
    // stream without it; every access unit starts with picture timing SEI, IDR pictures with a buffering period.
    {
        gCabac = true;
        EncoderConfig c = config(320, 192, IntraMode::Auto, 60, 2);
        c.fps = 30;
        c.crf = 12;
        StreamInfo free;
        size_t freeBytes = 0;
        CHECK(roundTrip(c, 90, &freeBytes, 40, nullptr, &free));
        const double freeKbps = double(freeBytes) * 8.0 / 3.0 / 1000.0;
        const int maxrate = std::max(64, int(freeKbps / 2.0) / 64 * 64), bufsize = maxrate / 2;
        CHECK(cpbUnderflows(free.sizes, 30, maxrate, bufsize, 0.9) > 0); // the case is hard
        {
            EncoderConfig off = c;
            off.vbvMaxrateKbps = maxrate; // without a buffer size: off
            StreamInfo info;
            CHECK(roundTrip(off, 90, nullptr, 40, nullptr, &info));
            CHECK(info.hash == free.hash);
        }
        auto check = [&](EncoderConfig v, const char* what, int frames = 90) {
            v.vbvMaxrateKbps = maxrate;
            v.vbvBufsizeKbits = bufsize;
            StreamInfo info;
            size_t bytes = 0;
            CHECK(roundTrip(v, frames, &bytes, 40, nullptr, &info));
            double low = 0;
            const int under = cpbUnderflows(info.sizes, v.fps, maxrate, bufsize, v.vbvInit, &low);
            if (under > 0)
                std::fprintf(stderr, "vbv %s: %d underflows, lowest level %.0f of %d kbit, %.0f kbit/s (max %d, unconstrained %.0f), QP up to %d\n",
                             what, under, low / 1000.0, bufsize, bytes * 8.0 / (frames / v.fps) / 1000.0, maxrate, freeKbps,
                             *std::max_element(info.qps.begin(), info.qps.end()));
            CHECK(under == 0);
            return info.hash;
        };
        const uint64_t crfHash = check(c, "crf");
        {
            EncoderConfig t = c;
            t.threads = 1;
            CHECK(check(t, "crf, one thread") == crfHash);
            t.threads = 4;
            CHECK(check(t, "crf, four threads") == crfHash);
        }
        {
            EncoderConfig b = c;
            b.bframes = 3;
            b.bAdaptive = false;
            check(b, "crf, B pictures");
        }
        {
            EncoderConfig r = c;
            r.crf = -1;
            r.bitrateKbps = int(freeKbps); // the target above the peak rate: VBV decides
            check(r, "one-pass bitrate");
            FirstPass fp(r.width, r.height);
            for (int t = 0; t < 90; ++t) {
                const Yuv src = synthFrame(r.width, r.height, t);
                fp.add(Picture{src.y.data(), src.u.data(), src.v.data(), src.w, src.w / 2}, t == 40);
            }
            r.firstPass = fp.stats();
            check(r, "two-pass bitrate");
        }
        {
            EncoderConfig q = c; // constant QP, no lookahead
            q.crf = -1;
            q.qp = 14;
            q.lookahead = 0;
            q.mbtree = false;
            check(q, "constant QP without lookahead");
        }
        {
            // Chunks: every chunk but the last ends at least kChunkVbvEnd full, the next assumes kChunkVbvFill, so the
            // concatenated stream keeps the buffer too; the seam's picture timing is joined (vbvJoinChunk)
            EncoderConfig v = c;
            v.vbvMaxrateKbps = maxrate;
            v.vbvBufsizeKbits = bufsize;
            FirstPass fp(v.width, v.height);
            for (int t = 0; t < 90; ++t) {
                const Yuv src = synthFrame(v.width, v.height, t);
                fp.add(Picture{src.y.data(), src.u.data(), src.v.data(), src.w, src.w / 2}, t == 40);
            }
            const ChunkPlan plan = planChunks(*fp.stats(), v, 2, 30);
            CHECK(plan.start.size() == 2);
            CHECK(plan.configFor(v, 0, 1).vbvEndFill == kChunkVbvEnd && plan.configFor(v, 1, 1).vbvEndFill == 0);
            CHECK(plan.configFor(v, 1, 1).vbvInit == kChunkVbvFill && plan.configFor(v, 0, 1).vbvInit == v.vbvInit);
            StreamInfo info;
            CHECK(roundTrip(v, 90, nullptr, 40, &plan, &info));
            double low = 0;
            const int under = cpbUnderflows(info.sizes, v.fps, maxrate, bufsize, v.vbvInit, &low);
            if (under > 0)
                std::fprintf(stderr, "vbv chunks: %d underflows, lowest level %.0f of %d kbit\n", under, low / 1000.0, bufsize);
            CHECK(under == 0);
            // vbvJoinChunk rewrites the first picture timing's cpb_removal_delay only
            Encoder enc(v);
            std::vector<uint8_t> au;
            for (int t = 0; au.empty() && t < 60; ++t) {
                const Yuv src = synthFrame(v.width, v.height, t);
                au = enc.encode(Picture{src.y.data(), src.u.data(), src.v.data(), src.w, src.w / 2});
            }
            const std::vector<uint8_t> joined = vbvJoinChunk(au, 37);
            CHECK(joined.size() > 19 && joined[5] == 0 && joined[14] == 1 && joined[16] == 0 && joined[17] == 0 && joined[18] == 74);
            const uint8_t startCode[4] = {0, 0, 0, 1};
            const auto sliceA = std::search(au.begin() + 4, au.end(), startCode, startCode + 4);
            const auto sliceJ = std::search(joined.begin() + 4, joined.end(), startCode, startCode + 4);
            CHECK(sliceA != au.end() && au.end() - sliceA == joined.end() - sliceJ && std::equal(sliceA, au.end(), sliceJ));
            for (au = enc.flush(); !au.empty(); au = enc.flush()) {
            }
        }
        gCabac = false;
        check(config(320, 192, IntraMode::Auto, 60, 2), "CAVLC, constant QP"); // (qp 28: just a smoke test)
        {
            EncoderConfig cv = c;
            cv.cabac = false;
            check(cv, "CAVLC, crf");
        }
        gCabac = true;
        // The SEI
        {
            EncoderConfig v = c;
            v.vbvMaxrateKbps = maxrate;
            v.vbvBufsizeKbits = bufsize;
            Encoder enc(v);
            CHECK(enc.headers().size() > Encoder(c).headers().size()); // hrd_parameters in the VUI
            std::vector<std::vector<uint8_t>> aus;
            for (int t = 0; t < 12; ++t) {
                const Yuv src = synthFrame(v.width, v.height, t);
                auto au = enc.encode(Picture{src.y.data(), src.u.data(), src.v.data(), src.w, src.w / 2}, t == 6);
                if (!au.empty())
                    aus.push_back(std::move(au));
            }
            for (auto au = enc.flush(); !au.empty(); au = enc.flush())
                aus.push_back(std::move(au));
            CHECK(aus.size() == 12);
            int periods = 0;
            for (const auto& au : aus) {
                CHECK(au.size() > 6 && au[4] == 6); // SEI first
                if (au.size() > 6 && au[5] == 0) {   // buffering period, then picture timing
                    ++periods;
                    const size_t next = 7 + au[6];
                    CHECK(next < au.size() && au[next] == 1);
                } else {
                    CHECK(au.size() > 6 && au[5] == 1);
                }
            }
            CHECK(periods == 2); // the first picture and the keyframe at 6
            CHECK(enc.vbvUnderflows() == 0);
        }
    }
}

// The encoder's second build for x86-64-v3 CPUs (EncoderAlt, picked at run time where CMake built it and the CPU
// has the instructions): the same streams, reconstructions and first-pass numbers as the build for every CPU - fixed
// QP with B pictures, constant quality on several threads, two passes - and selectCpuBuild(false) keeps to the
// first build; the own cpuid check agrees with the compiler's. The speed stage "fast" (EncoderConfig::speed 1) decodes bit-exact, does not depend on the thread
// count, and is another stream than the default only with B pictures.
void testCpuBuilds()
{
    gCabac = true;
#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__)
    // The own cpuid check against the compiler's (this one knows every name; older ones do not)
    const bool builtin = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("avx") && __builtin_cpu_supports("bmi")
        && __builtin_cpu_supports("bmi2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("lzcnt")
        && __builtin_cpu_supports("movbe") && __builtin_cpu_supports("f16c") && __builtin_cpu_supports("popcnt")
        && __builtin_cpu_supports("sse4.2") && __builtin_cpu_supports("ssse3");
    CHECK(cpuHasX86_64V3() == builtin);
#endif
    const bool v3 = std::strcmp(selectCpuBuild(true), "x86-64-v3") == 0;
    CHECK(!v3 || cpuHasX86_64V3());
    CHECK(std::strcmp(selectCpuBuild(false), "generic") == 0);
    CHECK(std::strcmp(cpuBuild(), "generic") == 0);
    if (v3) {
        auto run = [](EncoderConfig c, int frames, bool twoPass, bool useV3, std::vector<double>* costs) {
            selectCpuBuild(useV3);
            if (twoPass) {
                FirstPass fp(c.width, c.height, c.threads, c.bframes);
                for (int t = 0; t < frames; ++t) {
                    const Yuv src = synthFrame(c.width, c.height, t);
                    fp.add(Picture{src.y.data(), src.u.data(), src.v.data(), src.w, src.w / 2});
                }
                c.firstPass = fp.stats();
                *costs = c.firstPass->interSum;
                costs->insert(costs->end(), c.firstPass->intraSum.begin(), c.firstPass->intraSum.end());
            }
            StreamInfo info;
            CHECK(roundTrip(c, frames, nullptr, -1, nullptr, &info));
            selectCpuBuild(false);
            return info;
        };
        auto same = [&](const EncoderConfig& c, int frames, bool twoPass = false) {
            std::vector<double> costsGeneric, costsV3;
            const StreamInfo a = run(c, frames, twoPass, false, &costsGeneric), b = run(c, frames, twoPass, true, &costsV3);
            CHECK(a.hash == b.hash);
            CHECK(a.sizes == b.sizes);
            CHECK(costsGeneric == costsV3);
        };
        EncoderConfig c = config(320, 192, IntraMode::Auto, 30, 2);
        c.bframes = 3;
        c.psyRd = 1.0;
        c.bSharp = 1.5;
        same(c, 40);
        c = config(320, 192, IntraMode::Auto, 60, 4);
        c.fps = 30;
        c.crf = 23;
        c.bframes = 8;
        c.aqStrength = 1.5;
        same(c, 50);
        c = config(250, 142, IntraMode::Auto, 60, 3);
        c.bitrateKbps = 300;
        c.bframes = 2;
        same(c, 50, true);
        gCabac = false;
        same(config(176, 144, IntraMode::Auto, 8, 2), 12);
        gCabac = true;
    } else {
        std::printf("test_h264_encoder: no x86-64-v3 build of the encoder here (not built, or the CPU lacks it) - comparison skipped\n");
    }
    {
        EncoderConfig c = config(320, 192, IntraMode::Auto, 60, 1);
        c.bframes = 2;
        c.bAdaptive = false;
        StreamInfo def, fast1, fast4, noB, noBFast;
        CHECK(roundTrip(c, 40, nullptr, -1, nullptr, &def));
        c.speed = 1;
        CHECK(roundTrip(c, 40, nullptr, -1, nullptr, &fast1));
        c.threads = 4;
        CHECK(roundTrip(c, 40, nullptr, -1, nullptr, &fast4));
        CHECK(fast1.hash == fast4.hash);
        CHECK(fast1.hash != def.hash);
        c.bframes = 0;
        CHECK(roundTrip(c, 20, nullptr, -1, nullptr, &noBFast));
        c.speed = 0;
        CHECK(roundTrip(c, 20, nullptr, -1, nullptr, &noB));
        CHECK(noB.hash == noBFast.hash);
    }
}

// EncodeHints: what the caller knows about the pictures (repeats, still and background rectangles).
void testHints()
{
    gCabac = true;
    const int w = 320, h = 192, mbs = (w / 16) * (h / 16);
    auto base = [&](int threads, int bframes) {
        EncoderConfig c = config(w, h, IntraMode::Auto, 60, threads);
        c.bframes = bframes;
        c.bAdaptive = false;
        c.aqStrength = 1.5;
        return c;
    };
    EncodeHints none, repeat, wholeStill, topBackground, both;
    repeat.repeat = true;
    wholeStill.still.push_back({0, 0, w, h});
    topBackground.background.push_back({0, 0, w, 96}); // six rows of macroblocks
    both = topBackground;
    both.repeat = true;
    for (const int bframes : {0, 3}) {
        StreamInfo plain, empty, wrong;
        // No hints, empty hints, and hints that are wrong (the pictures move): one and the same stream
        gHints = nullptr;
        CHECK(roundTrip(base(1, bframes), 24, nullptr, -1, nullptr, &plain));
        gHints = [&](int) { return &none; };
        CHECK(roundTrip(base(1, bframes), 24, nullptr, -1, nullptr, &empty));
        CHECK(plain.hash == empty.hash);
        gHints = [&](int t) { return t % 2 ? &repeat : &wholeStill; };
        CHECK(roundTrip(base(1, bframes), 24, nullptr, -1, nullptr, &wrong));
        CHECK(wrong.hash == plain.hash);
        CHECK(gHintStats.repeats == 0 && gHintStats.stillMbs == 0);

        // Pictures that repeat in groups of four, with the hint on every picture (wrong on each group's first):
        // decoded bit-exact, the repeats found, still macroblocks skipped, the same stream with any thread count
        // and whether the hint says "repeat" or gives the whole picture as a still rectangle
        gSynthHold = 4;
        StreamInfo held, hinted, hinted4, rect;
        gHints = nullptr;
        CHECK(roundTrip(base(1, bframes), 32, nullptr, -1, nullptr, &held));
        gHints = [&](int) { return &repeat; };
        CHECK(roundTrip(base(1, bframes), 32, nullptr, -1, nullptr, &hinted));
        CHECK(gHintStats.repeats == 24);
        CHECK(gHintStats.stillMbs > 12 * mbs);
        CHECK(hinted.types == held.types);
        CHECK(hinted.minPsnrY > held.minPsnrY - 0.5);
        const Encoder::HintStats stats1 = gHintStats;
        CHECK(roundTrip(base(4, bframes), 32, nullptr, -1, nullptr, &hinted4));
        CHECK(hinted4.hash == hinted.hash);
        CHECK(gHintStats.stillMbs == stats1.stillMbs);
        gHints = [&](int) { return &wholeStill; };
        CHECK(roundTrip(base(1, bframes), 32, nullptr, -1, nullptr, &rect));
        CHECK(rect.hash == hinted.hash);
        // A still rectangle over a part: only macroblocks inside it are taken
        EncodeHints part;
        part.still.push_back({16, 16, 160, 96}); // 10 x 6 macroblocks
        gHints = [&](int) { return &part; };
        CHECK(roundTrip(base(1, bframes), 32));
        CHECK(gHintStats.repeats == 0 && gHintStats.stillMbs > 0 && gHintStats.stillMbs <= 24 * 60);
        gSynthHold = 0;

        // Background: the synthetic picture has noise everywhere, so the texture rule would take every macroblock
        // out again (first case: the stream of no hints); with the rule off the top half is coded coarser and with
        // the cheap decisions - smaller, decoded bit-exact, the same with four threads, and coarser = smaller still
        StreamInfo guarded, bg, bg4, bgCoarse;
        gHints = [&](int) { return &topBackground; };
        EncoderConfig c = base(1, bframes);
        c.hintBackgroundEnergy = 0;
        CHECK(roundTrip(c, 24, nullptr, -1, nullptr, &guarded));
        CHECK(guarded.hash == plain.hash);
        CHECK(gHintStats.backgroundMbs == 0);
        c.hintBackgroundEnergy = 99;
        size_t bytes = 0, bytesCoarse = 0;
        CHECK(roundTrip(c, 24, &bytes, -1, nullptr, &bg));
        CHECK(bg.hash != plain.hash);
        CHECK(gHintStats.backgroundMbs > 0);
        c.threads = 4;
        CHECK(roundTrip(c, 24, nullptr, -1, nullptr, &bg4));
        CHECK(bg4.hash == bg.hash);
        c.hintBackgroundQp = 10;
        CHECK(roundTrip(c, 24, &bytesCoarse, -1, nullptr, &bgCoarse));
        CHECK(bytesCoarse < bytes);
        // Background and repeats together, constant quality with the defaults' B pictures and the lookahead
        gSynthHold = 3;
        gHints = [&](int) { return &both; };
        c = base(3, bframes ? 8 : 0);
        c.bAdaptive = true;
        c.crf = 24;
        c.hintBackgroundEnergy = 99;
        CHECK(roundTrip(c, 45));
        CHECK(gHintStats.repeats == 30);
        gSynthHold = 0;
    }
    // Without the lookahead the picture before is not at hand: repeats are not taken, the stream is that of no hints
    {
        EncoderConfig c = config(w, h, IntraMode::Auto, 30, 1);
        c.lookahead = 0;
        c.mbtree = false;
        gSynthHold = 4;
        StreamInfo plain, hinted;
        gHints = nullptr;
        CHECK(roundTrip(c, 16, nullptr, -1, nullptr, &plain));
        gHints = [&](int) { return &repeat; };
        CHECK(roundTrip(c, 16, nullptr, -1, nullptr, &hinted));
        CHECK(hinted.hash == plain.hash);
        CHECK(gHintStats.repeats == 0);
        gSynthHold = 0;
    }
    // Two-pass and the second build of the encoder (where there is one): decoded bit-exact with hints, both builds
    // the same stream
    {
        gSynthHold = 4;
        gHints = [&](int) { return &both; };
        EncoderConfig c = config(w, h, IntraMode::Auto, 60, 2);
        c.bframes = 3;
        c.hintBackgroundEnergy = 99;
        StreamInfo generic, v3;
        CHECK(roundTrip(c, 32, nullptr, -1, nullptr, &generic));
        const Encoder::HintStats statsGeneric = gHintStats;
        if (std::strcmp(selectCpuBuild(true), "x86-64-v3") == 0) {
            CHECK(roundTrip(c, 32, nullptr, -1, nullptr, &v3));
            CHECK(v3.hash == generic.hash);
            CHECK(gHintStats.repeats == statsGeneric.repeats && gHintStats.stillMbs == statsGeneric.stillMbs
                  && gHintStats.backgroundMbs == statsGeneric.backgroundMbs);
        }
        selectCpuBuild(false);
        FirstPass fp(c.width, c.height, c.threads, c.bframes);
        for (int t = 0; t < 32; ++t) {
            const Yuv src = synthFrame(c.width, c.height, t);
            fp.add(Picture{src.y.data(), src.u.data(), src.v.data(), src.w, src.w / 2});
        }
        c.firstPass = fp.stats();
        c.bitrateKbps = 300;
        CHECK(roundTrip(c, 32));
        CHECK(gHintStats.repeats == 24);
        gSynthHold = 0;
    }
    gHints = nullptr;
}

int main()
{
    Check::initEnv();
    // The cases below run the encoder's build for every CPU: the failure injection (ThreadErrors.h) and other
    // hooks reach only that one. testCpuBuilds compares the x86-64-v3 build against it.
    selectCpuBuild(false);
    if (std::getenv("H264_TEST_CPU")) { // only the two builds and the speed stage (debugging)
        testCpuBuilds();
        return Check::result();
    }
    if (std::getenv("H264_TEST_HINTS")) { // only the hints (debugging)
        testHints();
        return Check::result();
    }
    if (std::getenv("H264_TEST_VBV")) { // only the VBV cases (debugging)
        testVbv();
        return Check::result();
    }
    CHECK(selfTestTables());
    CHECK(kernelsMatchReference());
    CHECK(vectorKernelsMatchReference());
    for (const bool cabac : {false, true}) {
    gCabac = cabac;

    // Intra only
    CHECK(roundTrip(config(176, 144, IntraMode::Pcm, 1), 2));
    CHECK(roundTrip(config(176, 144, IntraMode::Intra16, 1), 3));
    CHECK(roundTrip(config(176, 144, IntraMode::Intra4, 1), 3));
    CHECK(roundTrip(config(176, 144, IntraMode::Auto, 1), 3));
    {
        EncoderConfig c = config(176, 144, IntraMode::Auto, 1);
        c.deblock = false;
        CHECK(roundTrip(c, 2));
    }
    // P frames: partitions, skip, quarter-pel, deblocking, slices, odd size (cropping)
    CHECK(roundTrip(config(176, 144, IntraMode::Auto, 8), 12));
    CHECK(roundTrip(config(250, 142, IntraMode::Auto, 6, 3), 10));
    {
        EncoderConfig c = config(176, 144, IntraMode::Auto, 30);
        c.effort = 0;
        CHECK(roundTrip(c, 8));
    }
    {
        EncoderConfig c = config(176, 144, IntraMode::Auto, 8, 2);
        c.aqStrength = 0; // all macroblocks at the slice QP
        CHECK(roundTrip(c, 6));
        c.aqStrength = 3; // large per-macroblock QP jumps (mb_qp_delta, deblocking across QPs)
        CHECK(roundTrip(c, 6));
    }
    if (gCabac) { // Main profile without the 8x8 transform
        EncoderConfig c = config(176, 144, IntraMode::Auto, 8, 2);
        c.transform8x8 = false;
        CHECK(roundTrip(c, 6));
    }
    if (gCabac) { // every inter candidate coded in full instead of ranked by estimate; several QPs and references
        for (int qp : {18, 26, 38}) {
            EncoderConfig c = config(250, 142, IntraMode::Auto, 12, 3);
            c.qp = qp;
            c.refs = qp == 26 ? 2 : 1;
            size_t bytesEst = 0, bytesFull = 0;
            CHECK(roundTrip(c, 12, &bytesEst));
            c.modeEstimate = false;
            CHECK(roundTrip(c, 12, &bytesFull));
            if (std::abs(double(bytesFull) - double(bytesEst)) >= 0.05 * double(bytesFull))
                std::fprintf(stderr, "mode estimate at QP %d: %zu bytes, full %zu\n", qp, bytesEst, bytesFull);
            CHECK(std::abs(double(bytesFull) - double(bytesEst)) < 0.05 * double(bytesFull));
        }
    }
    // A keyframe the caller asks for (a cut in the timeline) in the middle of a GOP
    CHECK(roundTrip(config(176, 144, IntraMode::Auto, 30), 10, nullptr, 4));
    {
        EncoderConfig c = config(176, 144, IntraMode::Auto, 8, 2);
        c.lookahead = 0; // pictures go straight through, no macroblock tree
        c.mbtree = false;
        CHECK(roundTrip(c, 10));
        c.lookahead = 4; // shorter than the stream: pictures come out while others go in
        c.mbtree = true;
        CHECK(roundTrip(c, 10, nullptr, 6));
    }
    for (int refs : {2, 3}) { // several reference pictures (ref_idx te(v)/ue(v), sliding window)
        EncoderConfig c = config(176, 144, IntraMode::Auto, 10, 2);
        c.refs = refs;
        CHECK(roundTrip(c, 12));
    }
    for (int qp : {12, 40, 51}) {
        EncoderConfig c = config(176, 144, IntraMode::Auto, 10);
        c.qp = qp;
        CHECK(roundTrip(c, 6));
    }
    // Rate control lands near its target and keeps the stream decodable
    {
        EncoderConfig c = config(320, 192, IntraMode::Auto, 30, 2);
        c.bitrateKbps = 1000;
        c.fps = 30;
        size_t bytes = 0;
        CHECK(roundTrip(c, 60, &bytes));
        const double kbps = bytes * 8.0 / 2.0 / 1000.0; // 60 frames at 30 fps
        if (std::abs(kbps - 1000.0) >= 250.0)
            std::fprintf(stderr, "rate control: %.0f kbit/s instead of 1000\n", kbps);
        CHECK(std::abs(kbps - 1000.0) < 250.0);
        // The same with a first pass over the whole clip (one QP for all of it)
        FirstPass fp(c.width, c.height);
        for (int t = 0; t < 60; ++t) {
            const Yuv src = synthFrame(c.width, c.height, t);
            fp.add(Picture{src.y.data(), src.u.data(), src.v.data(), src.w, src.w / 2});
        }
        c.firstPass = fp.stats();
        CHECK(c.firstPass->size() == 60);
        bytes = 0;
        CHECK(roundTrip(c, 60, &bytes));
        const double kbps2 = bytes * 8.0 / 2.0 / 1000.0;
        if (std::abs(kbps2 - 1000.0) >= 250.0)
            std::fprintf(stderr, "rate control with first pass: %.0f kbit/s instead of 1000\n", kbps2);
        CHECK(std::abs(kbps2 - 1000.0) < 250.0);
        // The clip's start (startBoostQp): the first half second gets more of the clip's bits than without, the
        // clip keeps its size, the stream decodes bit-exact and is the same when coded again; the
        // boost stays within its share of the clip's bits (a third of the pictures here: far less than 2 QP).
        {
            auto startBytes = [&](const EncoderConfig& cfg, size_t* all, uint64_t* hash) {
                StreamInfo info;
                CHECK(roundTrip(cfg, 60, all, -1, nullptr, &info));
                size_t first = 0;
                for (size_t i = 0; i < info.order.size(); ++i)
                    if (info.order[i] < 15)
                        first += info.sizes[i];
                if (hash)
                    *hash = info.hash;
                return first;
            };
            size_t allPlain = 0, allBoost = 0, allOne = 0;
            uint64_t hashBoost = 0, hashOne = 0;
            const size_t plain = startBytes(c, &allPlain, nullptr);
            EncoderConfig b = c;
            b.startBoostQp = 2.0;
            b.startBoostSeconds = 0.5;
            const size_t boosted = startBytes(b, &allBoost, &hashBoost);
            if (!(boosted > plain) || !(double(boosted) < 1.25 * double(plain)))
                std::fprintf(stderr, "start boost: first half second %zu -> %zu bytes, clip %zu -> %zu\n", plain, boosted, allPlain, allBoost);
            CHECK(boosted > plain);
            CHECK(double(boosted) < 1.25 * double(plain));
            CHECK(std::abs(double(allBoost) - double(allPlain)) < 0.05 * double(allPlain));
            startBytes(b, &allOne, &hashOne); // the same again
            CHECK(allOne == allBoost && hashOne == hashBoost);
        }
        // With threads the first pass analyses its batches on a thread of its own, the pictures of
        // a batch side by side: the same numbers as one picture after the other (also with the
        // B probe, and with a scene change in the clip).
        for (bool forB : {false, true}) {
            auto statsOf = [&](int threads) {
                FirstPass f(c.width, c.height, threads, forB ? 3 : 0);
                for (int t = 0; t < 45; ++t) {
                    const Yuv src = synthFrame(c.width, c.height, t < 30 ? t : 500 + 7 * t);
                    f.add(Picture{src.y.data(), src.u.data(), src.v.data(), src.w, src.w / 2});
                }
                return f.stats();
            };
            const auto one = statsOf(1);
            for (int threads : {2, 3, 8}) {
                const auto many = statsOf(threads);
                bool same = many->size() == one->size() && many->intraSum == one->intraSum && many->interSum == one->interSum &&
                            many->sceneChange == one->sceneChange && many->bGate == one->bGate &&
                            many->analysis.size() == one->analysis.size();
                for (size_t i = 0; same && i < one->analysis.size(); ++i)
                    same = many->analysis[i]->interCost == one->analysis[i]->interCost &&
                           many->analysis[i]->intraCost == one->analysis[i]->intraCost &&
                           many->analysis[i]->mvx == one->analysis[i]->mvx && many->analysis[i]->mvy == one->analysis[i]->mvy;
                CHECK(same);
            }
        }
        // The encoder takes the first pass's analysis instead of repeating it: the same stream with
        // none of it kept and with only the first pictures kept (memory limit).
        CHECK(c.firstPass->analysis.size() == 60);
        const size_t perPicture = size_t((c.width + 15) / 16) * ((c.height + 15) / 16) * 12;
        for (size_t keep : {size_t(0), 25 * perPicture}) {
            FirstPass fpKeep(c.width, c.height, 1, 0, keep);
            for (int t = 0; t < 60; ++t) {
                const Yuv src = synthFrame(c.width, c.height, t);
                fpKeep.add(Picture{src.y.data(), src.u.data(), src.v.data(), src.w, src.w / 2});
            }
            EncoderConfig k = c;
            k.firstPass = fpKeep.stats();
            CHECK(k.firstPass->analysis.size() == (keep ? 25u : 0u));
            size_t bytesKeep = 0;
            CHECK(roundTrip(k, 60, &bytesKeep));
            CHECK(bytesKeep == bytes);
        }
    }
    // Duplicated pictures cost next to nothing: a picture that repeats the one before is a run of skipped
    // macroblocks (fixed QP, P pictures only; every picture shown four times).
    {
        gSynthHold = 4;
        EncoderConfig c = config(320, 192, IntraMode::Auto, 60, 2);
        StreamInfo info;
        CHECK(roundTrip(c, 40, nullptr, -1, nullptr, &info));
        size_t worst = 0, fresh = 0;
        for (size_t i = 0; i < info.order.size(); ++i)
            (info.order[i] % 4 ? worst : fresh) = std::max(info.order[i] % 4 ? worst : fresh, info.sizes[i]);
        if (worst * 50 > fresh)
            std::fprintf(stderr, "duplicated pictures: up to %zu bytes each (new pictures up to %zu)\n", worst, fresh);
        CHECK(worst * 50 <= fresh);
        gSynthHold = 0;
    }
    // Constant quality (crf): no size target. Every picture decodes bit-exact, a higher value gives a smaller
    // file and a lower quality, a still clip a far smaller file than a moving one, the stream does not depend
    // on the thread count, and a bitrate target switches the mode off.
    {
        EncoderConfig c = config(320, 192, IntraMode::Auto, 30, 2);
        c.fps = 30;
        size_t size[3] = {};
        double worstPsnr[3] = {};
        const double values[3] = {18, 23, 28};
        for (int k = 0; k < 3; ++k) {
            c.crf = values[k];
            StreamInfo info;
            CHECK(roundTrip(c, 45, &size[k], -1, nullptr, &info));
            worstPsnr[k] = info.minPsnrY;
        }
        if (!(size[0] > size[1] && size[1] > size[2]) || !(double(size[0]) < 8.0 * double(size[2])))
            std::fprintf(stderr, "crf 18/23/28: %zu %zu %zu bytes\n", size[0], size[1], size[2]);
        CHECK(size[0] > size[1] && size[1] > size[2]);
        CHECK(double(size[0]) < 8.0 * double(size[2])); // ten steps: x264 5.8x on game footage, never an eighth
        if (!(worstPsnr[0] > worstPsnr[2]))
            std::fprintf(stderr, "crf 18/23/28: worst picture %.2f %.2f %.2f dB\n", worstPsnr[0], worstPsnr[1], worstPsnr[2]);
        CHECK(worstPsnr[0] > worstPsnr[2]); // (the clip's noise caps the quality of the two finer ones)
        c.crf = 23;
        // qp is not used
        {
            EncoderConfig other = c;
            other.qp = 40;
            size_t bytes = 0;
            CHECK(roundTrip(other, 45, &bytes));
            CHECK(bytes == size[1]);
        }
        // Threads
        uint64_t hashOne = 0;
        for (int threads : {1, 2, 4}) {
            EncoderConfig t = c;
            t.threads = threads;
            StreamInfo info;
            size_t bytes = 0;
            CHECK(roundTrip(t, 45, &bytes, -1, nullptr, &info));
            if (threads == 1)
                hashOne = info.hash;
            CHECK(gCabac ? info.hash == hashOne : bytes > 0); // (CAVLC: one slice per thread)
        }
        // qcomp moves bits between the clip's stretches against the clip's own mean and leaves its level
        // to the value: about the same size without it
        {
            EncoderConfig q = c;
            q.qcomp = 0;
            size_t flat = 0;
            CHECK(roundTrip(q, 45, &flat));
            if (!(double(flat) < 1.3 * double(size[1]) && double(size[1]) < 1.3 * double(flat)))
                std::fprintf(stderr, "crf 23: %zu bytes with qcomp, %zu without\n", size[1], flat);
            CHECK(double(flat) < 1.3 * double(size[1]) && double(size[1]) < 1.3 * double(flat));
        }
        // A still clip (only the noise changes)
        gSynthKind = 1;
        size_t still = 0;
        CHECK(roundTrip(c, 45, &still));
        gSynthKind = 0;
        if (!(still * 2 < size[1]))
            std::fprintf(stderr, "crf 23: still clip %zu bytes, moving %zu\n", still, size[1]);
        CHECK(still * 2 < size[1]);
        // With a bitrate target the value is ignored
        {
            EncoderConfig b = c;
            b.bitrateKbps = 800;
            StreamInfo withCrf, without;
            CHECK(roundTrip(b, 45, nullptr, -1, nullptr, &withCrf));
            b.crf = -1;
            CHECK(roundTrip(b, 45, nullptr, -1, nullptr, &without));
            CHECK(withCrf.hash == without.hash);
        }
        // Without lookahead or with intra pictures only: one QP from the value
        {
            EncoderConfig n = c;
            n.lookahead = 0;
            n.mbtree = false;
            CHECK(roundTrip(n, 20));
            n = c;
            n.gop = 1;
            CHECK(roundTrip(n, 6));
        }
        // The encoder's defaults (B pictures with the probe and the pyramid, psy, AQ 1.5), and in chunks
        if (gCabac) {
            EncoderConfig d;
            d.width = 320;
            d.height = 192;
            d.gop = 30;
            d.threads = 3;
            d.fps = 30;
            d.crf = 23;
            StreamInfo info, again;
            CHECK(roundTrip(d, 70, nullptr, -1, nullptr, &info));
            CHECK(roundTrip(d, 70, nullptr, -1, nullptr, &again));
            CHECK(info.hash == again.hash);
            FirstPass fp(d.width, d.height, 2, d.bframes);
            for (int t = 0; t < 70; ++t) {
                const Yuv src = synthFrame(d.width, d.height, t);
                fp.add(Picture{src.y.data(), src.u.data(), src.v.data(), src.w, src.w / 2});
            }
            const ChunkPlan plan = planChunks(*fp.stats(), d, 2, 20);
            CHECK(plan.start.size() == 2);
            // every chunk measures its complexity against the whole clip's mean (no QP step at the seam)
            CHECK(plan.crfComplexity > 0);
            CHECK(plan.crfComplexity == crfClipComplexity(*fp.stats(), d));
            CHECK(plan.configFor(d, 1, 1).crfClipComplexity == plan.crfComplexity);
            CHECK(roundTrip(d, 70, nullptr, -1, &plan));
            EncoderConfig rated = d;
            rated.bitrateKbps = 500;
            CHECK(planChunks(*fp.stats(), rated, 2, 20).crfComplexity < 0);
        }
    }
    // B pictures (CABAC only): coding order, display order and timestamps, every picture bit-exact
    if (gCabac) {
        for (int bframes : {1, 2, 3, 4}) { // 4 = 3 with the B pyramid
            const bool pyramid = bframes == 4;
            if (pyramid)
                bframes = 3;
            for (int threads : {1, 3}) {
                EncoderConfig c = config(176, 144, IntraMode::Auto, 30, threads);
                c.bframes = bframes;
                c.bAdaptive = false;
                c.bPyramid = pyramid;
                StreamInfo info;
                size_t bytes = 0;
                CHECK(roundTrip(c, 14, &bytes, -1, nullptr, &info));
                // I first, then each reference picture before the B pictures in front of it; the last picture is P.
                const char* expect = bframes == 1 ? "IPBPBPBPBPBPBP" : bframes == 2 ? "IPBBPBBPBBPBBP" : "IPBBBPBBBPBBBP";
                if (info.types != expect)
                    std::fprintf(stderr, "bframes %d: types %s\n", bframes, info.types.c_str());
                CHECK(info.types == expect);
                // A B picture lies between the two reference pictures coded before it, B pictures ascend.
                bool orderOk = info.order.size() == 14 && info.order[0] == 0;
                int before = -1, after = 0, lastB = -1;
                for (size_t i = 1; orderOk && i < info.order.size(); ++i) {
                    if (info.types[i] == 'B') {
                        // B pyramid: the middle B picture of a run comes first, the others ascend around it.
                        orderOk = info.order[i] > (pyramid ? before : std::max(before, lastB)) && info.order[i] < after;
                        lastB = info.order[i];
                    } else {
                        orderOk = info.order[i] > after;
                        before = after;
                        after = info.order[i];
                        lastB = -1;
                    }
                }
                CHECK(orderOk);
                if (pyramid) {
                    const std::vector<int> expectOrder = {0, 4, 2, 1, 3, 8, 6, 5, 7, 12, 10, 9, 11, 13};
                    CHECK(info.order == expectOrder);
                }
                if (info.minPsnrY < 30.0)
                    std::fprintf(stderr, "bframes %d: PSNR-Y %.2f dB\n", bframes, info.minPsnrY);
                CHECK(info.minPsnrY >= 30.0);
                // The same stream with any thread count.
                static size_t bytesOne[5] = {};
                if (threads == 1)
                    bytesOne[bframes + pyramid] = bytes;
                CHECK(bytes == bytesOne[bframes + pyramid]);
            }
            if (pyramid)
                break;
        }
        {
            // Smaller than without B pictures at the same QP on this (easy) material: with fixed runs, and
            // without the macroblock tree (since 15.11.2026 it gives the P pictures next to short runs its
            // full strength, so at one nominal QP they are finer and bigger than without B pictures). Placed
            // adaptively (the default) the stream must not be bigger than without them by more than a
            // few bytes of picture order counts.
            EncoderConfig c = config(320, 192, IntraMode::Auto, 30, 2);
            c.mbtree = false;
            size_t plain = 0, withB = 0, adaptive = 0;
            CHECK(roundTrip(c, 31, &plain));
            c.bframes = 2;
            c.bAdaptive = false;
            CHECK(roundTrip(c, 31, &withB));
            if (withB >= plain)
                std::fprintf(stderr, "B pictures: %zu bytes, without %zu\n", withB, plain);
            CHECK(withB < plain);
            c.bAdaptive = true;
            CHECK(roundTrip(c, 31, &adaptive));
            if (adaptive >= plain + plain / 100)
                std::fprintf(stderr, "adaptive B pictures: %zu bytes, without %zu\n", adaptive, plain);
            CHECK(adaptive < plain + plain / 100);
        }
        {
            // bSharp: on a slowly zooming texture the two reference pictures of a B picture show it at different
            // sub-sample positions and their average is softer than either. With the lost detail as a cost the B
            // pictures copy from one picture more often: another stream, bit-exact, the same with any thread
            // count; with a strong weight the B pictures keep more fine detail (at the default weight the
            // material's noise, new in every picture, makes the average the better prediction nearly everywhere).
            gSynthKind = 2;
            EncoderConfig c = config(320, 192, IntraMode::Auto, 30, 1);
            c.qp = 30;
            c.bframes = 3;
            c.bAdaptive = false;
            StreamInfo off, on, strong, onThreads;
            size_t bytesOff = 0, bytesOn = 0;
            CHECK(roundTrip(c, 17, &bytesOff, -1, nullptr, &off));
            c.bSharp = 1.5;
            CHECK(roundTrip(c, 17, &bytesOn, -1, nullptr, &on));
            CHECK(on.hash != off.hash);
            CHECK(on.types == off.types);
            c.threads = 3;
            CHECK(roundTrip(c, 17, nullptr, -1, nullptr, &onThreads));
            CHECK(onThreads.hash == on.hash);
            c.bSharp = 4.0;
            CHECK(roundTrip(c, 17, nullptr, -1, nullptr, &strong));
            if (!(strong.detailB > off.detailB && strong.detailB > on.detailB) || std::getenv("H264_TEST_PSY"))
                std::fprintf(stderr, "bSharp: detail of the B pictures %.0f, with 1.5 %.0f, with 4.0 %.0f; %zu -> %zu bytes, PSNR-Y %.2f -> %.2f\n",
                             off.detailB, on.detailB, strong.detailB, bytesOff, bytesOn, off.minPsnrY, on.minPsnrY);
            CHECK(strong.detailB > off.detailB);
            CHECK(strong.detailB > on.detailB);
            CHECK(on.minPsnrY >= off.minPsnrY - 1.0);
            gSynthKind = 0;
        }
        {
            // A keyframe the caller asks for and the GOP end: the picture before a keyframe is a P picture
            EncoderConfig c = config(176, 144, IntraMode::Auto, 8, 2);
            c.bframes = 2;
            c.bAdaptive = false;
            StreamInfo info;
            CHECK(roundTrip(c, 20, nullptr, 5, nullptr, &info));
            std::string display(20, '?');
            for (size_t i = 0; i < info.order.size(); ++i)
                display[size_t(info.order[i])] = info.types[i];
            if (display != "IBBBPIBBPBBPPIBBPBBP" && display != "IBBPPIBBPBBPPIBBPBBP")
                std::fprintf(stderr, "types in display order: %s\n", display.c_str());
            CHECK(display == "IBBPPIBBPBBPPIBBPBBP");
        }
        {
            // Without the 8x8 transform (Main), with several references, without frame threading, adaptive QP extremes
            EncoderConfig c = config(250, 142, IntraMode::Auto, 12, 3);
            c.bframes = 2;
            c.transform8x8 = false;
            CHECK(roundTrip(c, 14));
            c.transform8x8 = true;
            c.refs = 3;
            CHECK(roundTrip(c, 14));
            c.refs = 1;
            c.frameThreads = false;
            c.aqStrength = 3;
            CHECK(roundTrip(c, 14));
            c.aqStrength = 0;
            c.lookahead = 0; // the queue grows to what the B pictures need
            c.mbtree = false;
            c.effort = 0;
            CHECK(roundTrip(c, 14));
            for (int qp : {14, 44}) {
                EncoderConfig q = config(176, 144, IntraMode::Auto, 30, 2);
                q.bframes = 3;
                q.qp = qp;
                CHECK(roundTrip(q, 12));
            }
            for (int bframes : {2, 3}) { // implicit weighted prediction: unequal weights off the middle
                EncoderConfig q = config(176, 144, IntraMode::Auto, 30, 2);
                q.bframes = bframes;
                q.weightedBipred = true;
                CHECK(roundTrip(q, 12));
            }
        }
        {
            // The defaults as they are (B pictures by the probe, AQ, psy by QP, weighted prediction): bit-exact,
            // the same with any thread count, also with rate control and a first pass.
            StreamInfo one, four;
            EncoderConfig d;
            d.width = 320;
            d.height = 192;
            d.gop = 30;
            d.qp = 30;
            d.threads = 1;
            CHECK(roundTrip(d, 40, nullptr, -1, nullptr, &one));
            d.threads = 4;
            CHECK(roundTrip(d, 40, nullptr, -1, nullptr, &four));
            CHECK(one.hash == four.hash);
            d.bitrateKbps = 800;
            d.fps = 30;
            CHECK(roundTrip(d, 40));
        }
        {
            // Explicit weighted prediction: a fade in from black decodes bit-exactly (one and several references,
            // with B pictures, rate control), is clearly smaller than without the weights and the same with any
            // thread count; without a fade the option changes nothing but the headers (a second parameter set).
            gFadeFrames = 20;
            for (int refs : {1, 3}) {
                size_t with = 0, without = 0;
                StreamInfo one, four;
                EncoderConfig c = config(320, 192, IntraMode::Auto, 60, 1);
                c.refs = refs;
                CHECK(roundTrip(c, 26, &with, -1, nullptr, &one));
                c.threads = 4;
                CHECK(roundTrip(c, 26, nullptr, -1, nullptr, &four));
                CHECK(one.hash == four.hash);
                CHECK(one.minPsnrY >= 30.0);
                c.weightedPred = false;
                CHECK(roundTrip(c, 26, &without));
                std::fprintf(stderr, "fade, %d refs: %zu bytes with weighted prediction, %zu without\n", refs, with, without);
                CHECK(with * 10 < without * 9);
            }
            {
                EncoderConfig c = config(320, 192, IntraMode::Auto, 60, 3);
                c.bframes = 2;
                c.bAdaptive = false;
                CHECK(roundTrip(c, 26));
                EncoderConfig r = config(320, 192, IntraMode::Auto, 60, 2);
                r.bitrateKbps = 600;
                r.fps = 30;
                CHECK(roundTrip(r, 40));
            }
            gFadeFrames = 0;
            size_t with = 0, without = 0;
            EncoderConfig c = config(320, 192, IntraMode::Auto, 30, 2);
            CHECK(roundTrip(c, 12, &with));
            c.weightedPred = false;
            CHECK(roundTrip(c, 12, &without));
            CHECK(with > without && with - without < 16);
        }
        {
            // Adaptive placement (the default): any mix of run lengths decodes, with any thread count the same
            size_t bytesOne = 0;
            for (int threads : {1, 4}) {
                EncoderConfig c = config(320, 192, IntraMode::Auto, 30, threads);
                c.bframes = 3;
                StreamInfo info;
                size_t bytes = 0;
                CHECK(roundTrip(c, 24, &bytes, -1, nullptr, &info));
                CHECK(info.types.size() == 24 && info.types[0] == 'I' && info.minPsnrY >= 30.0);
                if (threads == 1)
                    bytesOne = bytes;
                CHECK(bytes == bytesOne);
            }
        }
        {
            // Placement: a slow zoom gets runs of B pictures (the planner finds them worth it from the lookahead's
            // costs; one pass and from a first pass with the same plan) and is smaller with them at the same QP.
            gSynthKind = 2;
            EncoderConfig c = config(320, 192, IntraMode::Auto, 30, 2);
            size_t plain = 0, withB = 0;
            CHECK(roundTrip(c, 24, &plain));
            c.bframes = 3;
            StreamInfo info;
            CHECK(roundTrip(c, 24, &withB, -1, nullptr, &info));
            const auto bCount = std::count(info.types.begin(), info.types.end(), 'B');
            if (bCount < 12 || withB >= plain)
                std::fprintf(stderr, "slow zoom: types %s, %zu bytes, without B %zu\n", info.types.c_str(), withB, plain);
            CHECK(bCount >= 12);
            CHECK(withB < plain);
            {
                gSynthKind = 1;
                // A still picture (with a little noise): runs of three (with the pyramid they gain a little on
                // screen recordings and the standing phases of game clips). They must not cost anything here.
                StreamInfo still;
                size_t stillB = 0, stillPlain = 0;
                CHECK(roundTrip(c, 24, &stillB, -1, nullptr, &still));
                EncoderConfig noB = c;
                noB.bframes = 0;
                CHECK(roundTrip(noB, 24, &stillPlain));
                // (The planner sees them as a clear gain on this noisy still, so the clip starts with them; a
                // clean still would keep its first twelve pictures as P pictures for the rate control.)
                const auto stillCount = std::count(still.types.begin(), still.types.end(), 'B');
                if (stillCount < 9 || stillB > stillPlain + 160)
                    std::fprintf(stderr, "still picture: types %s, %zu bytes, without B %zu\n", still.types.c_str(), stillB, stillPlain);
                CHECK(stillCount >= 9);
                CHECK(stillB <= stillPlain + 160);
                gSynthKind = 2;
            }
            FirstPass fp(c.width, c.height, 2, c.bframes);
            for (int t = 0; t < 24; ++t) {
                const Yuv src = synthFrame(c.width, c.height, t);
                fp.add(Picture{src.y.data(), src.u.data(), src.v.data(), src.w, src.w / 2});
            }
            c.firstPass = fp.stats();
            c.bitrateKbps = 300;
            c.fps = 30;
            StreamInfo info2;
            CHECK(roundTrip(c, 24, nullptr, -1, nullptr, &info2));
            if (info2.types != info.types)
                std::fprintf(stderr, "slow zoom, first pass: types %s instead of %s\n", info2.types.c_str(), info.types.c_str());
            CHECK(info2.types == info.types);
            gSynthKind = 0;
        }
        {
            // Long runs on moving material (the planner's "moving": costs growing with the distance to the
            // reference): coded like short runs, with a reference B picture in the middle of up to eight -
            // decodes bit-exactly, one pass and from a first pass with the same plan, any thread count the same.
            gSynthKind = 2;
            std::string typesOne;
            size_t bytesOne = 0;
            for (int threads : {1, 3}) {
                EncoderConfig c = config(320, 192, IntraMode::Auto, 30, threads);
                c.bframes = 8;
                StreamInfo info;
                size_t bytes = 0;
                CHECK(roundTrip(c, 40, &bytes, -1, nullptr, &info));
                CHECK(std::count(info.types.begin(), info.types.end(), 'B') >= 20);
                if (threads == 1) {
                    typesOne = info.types;
                    bytesOne = bytes;
                }
                CHECK(info.types == typesOne && bytes == bytesOne);
            }
            EncoderConfig c = config(320, 192, IntraMode::Auto, 30, 2);
            c.bframes = 8;
            FirstPass fp(c.width, c.height, 2, c.bframes);
            for (int t = 0; t < 40; ++t) {
                const Yuv src = synthFrame(c.width, c.height, t);
                fp.add(Picture{src.y.data(), src.u.data(), src.v.data(), src.w, src.w / 2});
            }
            c.firstPass = fp.stats();
            c.bitrateKbps = 300;
            c.fps = 30;
            StreamInfo info2;
            CHECK(roundTrip(c, 40, nullptr, -1, nullptr, &info2));
            if (info2.types != typesOne)
                std::fprintf(stderr, "long runs, first pass: types %s instead of %s\n", info2.types.c_str(), typesOne.c_str());
            CHECK(info2.types == typesOne);
            gSynthKind = 0;
        }
        {
            // A fade in: the planner's costs know no weighted prediction (the run's P picture looks several times
            // too expensive) - B pictures are placed all the same, and the stream decodes.
            gFadeFrames = 24;
            EncoderConfig c = config(320, 192, IntraMode::Auto, 30, 2);
            c.bframes = 3;
            StreamInfo info;
            CHECK(roundTrip(c, 24, nullptr, -1, nullptr, &info));
            if (std::count(info.types.begin(), info.types.end(), 'B') < 6)
                std::fprintf(stderr, "fade: types %s\n", info.types.c_str());
            CHECK(std::count(info.types.begin(), info.types.end(), 'B') >= 6);
            gFadeFrames = 0;
        }
        {
            // Duplicated pictures (a game at 12 pictures per second recorded at 60): every picture is shown five
            // times, B pictures come in runs of three. The first pass sees each picture against the one before - a
            // P picture that repeats the B picture before it looks free and carries the change, the B picture on
            // the change looks expensive and copies. The two-pass plan goes by the pictures' real references: the
            // clip meets its size and the P pictures' QP stays put after the first pictures.
            gSynthHold = 5;
            EncoderConfig c = config(320, 192, IntraMode::Auto, 60, 2);
            c.bframes = 3;
            c.bAdaptive = false;
            c.bitrateKbps = 300;
            c.fps = 60;
            FirstPass fp(c.width, c.height, 2, c.bframes);
            for (int t = 0; t < 120; ++t) {
                const Yuv src = synthFrame(c.width, c.height, t);
                fp.add(Picture{src.y.data(), src.u.data(), src.v.data(), src.w, src.w / 2});
            }
            c.firstPass = fp.stats();
            size_t bytes = 0;
            StreamInfo info;
            CHECK(roundTrip(c, 120, &bytes, -1, nullptr, &info));
            const double kbps = bytes * 8.0 / 2.0 / 1000.0;
            int qpLow = 99, qpHigh = 0;
            for (size_t i = 0; i < info.types.size(); ++i)
                if (info.types[i] == 'P' && info.order[i] >= 20) {
                    qpLow = std::min(qpLow, info.qps[i]);
                    qpHigh = std::max(qpHigh, info.qps[i]);
                }
            if (std::abs(kbps - 300.0) >= 30.0 || qpHigh - qpLow > 4)
                std::fprintf(stderr, "duplicated pictures, two-pass: %.0f kbit/s instead of 300, P QP %d..%d\n", kbps, qpLow, qpHigh);
            CHECK(std::abs(kbps - 300.0) < 30.0);
            CHECK(qpHigh - qpLow <= 4);
            // One pass (the lookahead measures every picture against its real references) and constant quality.
            c.firstPass.reset();
            CHECK(roundTrip(c, 120, &bytes));
            if (std::abs(bytes * 8.0 / 2.0 / 1000.0 - 300.0) >= 75.0)
                std::fprintf(stderr, "duplicated pictures, one pass: %.0f kbit/s instead of 300\n", bytes * 8.0 / 2.0 / 1000.0);
            CHECK(std::abs(bytes * 8.0 / 2.0 / 1000.0 - 300.0) < 75.0);
            c.bitrateKbps = 0;
            c.crf = 23;
            size_t held = 0, moving = 0;
            CHECK(roundTrip(c, 120, &held));
            gSynthHold = 0;
            CHECK(roundTrip(c, 120, &moving));
            // (a fifth of the pictures are new, each kept five times as long and so coded finer: fewer bytes
            // than with every picture new, and the repeats do not count as a calm clip that earns a finer QP still)
            if (!(held < moving))
                std::fprintf(stderr, "duplicated pictures, crf: %zu bytes held, %zu moving\n", held, moving);
            CHECK(held < moving);
        }
        {
            // A clip that stands for its first pictures and then moves (a game's menu or a held shot before the
            // action): the probe that starts the two-pass model takes the queued picture that needs the most as its
            // P picture, not the second one - a picture that repeats the first says nothing about the moving ones,
            // the plan began several QP too fine and the first pictures took the bits of the rest.
            gSynthKind = 2;
            gSynthStart = 20;
            EncoderConfig c = config(320, 192, IntraMode::Auto, 120, 2);
            c.bframes = 3;
            c.bitrateKbps = 600;
            c.fps = 60;
            FirstPass fp(c.width, c.height, 2, c.bframes);
            for (int t = 0; t < 120; ++t) {
                const Yuv src = synthFrame(c.width, c.height, t);
                fp.add(Picture{src.y.data(), src.u.data(), src.v.data(), src.w, src.w / 2});
            }
            c.firstPass = fp.stats();
            size_t bytes = 0;
            StreamInfo info;
            CHECK(roundTrip(c, 120, &bytes, -1, nullptr, &info));
            gSynthKind = 0;
            gSynthStart = 0;
            const double kbps = bytes * 8.0 / 2.0 / 1000.0;
            std::vector<int> late;
            size_t startBytes = 0;
            for (size_t i = 0; i < info.types.size(); ++i) {
                if (info.types[i] == 'P' && info.order[i] >= 60)
                    late.push_back(info.qps[i]);
                if (info.order[i] < 20)
                    startBytes += info.sizes[i];
            }
            CHECK(!late.empty());
            std::sort(late.begin(), late.end());
            const int lateQp = late.empty() ? 0 : late[late.size() / 2];
            // (the I picture is coded 3 below the P QP; the plan's first QP within 4 of where it settles)
            if (info.qps[0] < lateQp - 7 || startBytes * 4 > bytes || std::abs(kbps - 600.0) >= 60.0)
                std::fprintf(stderr, "standing start, two-pass: I QP %d, P QP later %d, %zu of %zu bytes in the standing pictures, %.0f kbit/s\n",
                             info.qps[0], lateQp, startBytes, bytes, kbps);
            CHECK(info.qps[0] >= lateQp - 7);
            CHECK(startBytes * 4 <= bytes);
            CHECK(std::abs(kbps - 600.0) < 60.0);
        }
        {
            // Rate control with B pictures: one pass, with a first pass, and in chunks
            EncoderConfig c = config(320, 192, IntraMode::Auto, 30, 2);
            c.bframes = 2;
            c.bitrateKbps = 1000;
            c.fps = 30;
            size_t bytes = 0;
            CHECK(roundTrip(c, 60, &bytes));
            double kbps = bytes * 8.0 / 2.0 / 1000.0;
            if (std::abs(kbps - 1000.0) >= 250.0)
                std::fprintf(stderr, "rate control with B pictures: %.0f kbit/s instead of 1000\n", kbps);
            CHECK(std::abs(kbps - 1000.0) < 250.0);
            FirstPass fp(c.width, c.height, 2, c.bframes); // with the costs of the adaptive placement
            for (int t = 0; t < 60; ++t) {
                const Yuv src = synthFrame(c.width, c.height, t);
                fp.add(Picture{src.y.data(), src.u.data(), src.v.data(), src.w, src.w / 2});
            }
            c.firstPass = fp.stats();
            CHECK(c.firstPass->bMax == 2 && c.firstPass->bGate.size() == 60);
            bytes = 0;
            CHECK(roundTrip(c, 60, &bytes));
            kbps = bytes * 8.0 / 2.0 / 1000.0;
            if (std::abs(kbps - 1000.0) >= 250.0)
                std::fprintf(stderr, "first pass with B pictures: %.0f kbit/s instead of 1000\n", kbps);
            CHECK(std::abs(kbps - 1000.0) < 250.0);
            // Mode decision by estimate (the default) and with every candidate coded in full: both
            // bit-exact, about the same size (two-pass with B pictures).
            {
                EncoderConfig full = c;
                full.modeEstimate = false;
                size_t bytesFull = 0;
                CHECK(roundTrip(full, 60, &bytesFull));
                CHECK(std::abs(double(bytesFull) - double(bytes)) < 0.05 * double(bytes));
            }
            // Frame threading (two pictures in flight at first, then three; rows parked instead of
            // waited in): the same stream with one thread, without frame threading and with more
            // threads than rows can run - two-pass with B pictures and one pass without.
            for (const bool withB : {true, false}) {
                EncoderConfig t = c;
                if (!withB) {
                    t.bframes = 0;
                    t.firstPass.reset();
                }
                size_t ref = 0;
                t.threads = 1;
                CHECK(roundTrip(t, 60, &ref));
                for (const int threads : {2, 5, 16}) {
                    for (const bool frameThreads : {true, false}) {
                        t.threads = threads;
                        t.frameThreads = frameThreads;
                        size_t got = 0;
                        CHECK(roundTrip(t, 60, &got));
                        if (got != ref)
                            std::fprintf(stderr, "threads %d frame threading %d B %d: %zu bytes, one thread %zu\n", threads,
                                         int(frameThreads), int(withB), got, ref);
                        CHECK(got == ref);
                    }
                }
            }
            c.firstPass.reset();
            const ChunkPlan rated = planChunks(*fp.stats(), c, 3, 10);
            bytes = 0;
            CHECK(roundTrip(c, 60, &bytes, -1, &rated));
        }
    }
    // Psycho-visual mode decision (psyRd)
    {
        // Off (0, the default) is the stream without the option; on, every picture still decodes bit-exact, with
        // one thread or several the same stream, and the reconstruction is closer to the source by the psy distance.
        for (int qp : {22, 28}) { // where the source's noise is not below the quantiser altogether
            EncoderConfig c = config(320, 192, IntraMode::Auto, 30, 1);
            c.qp = qp;
            StreamInfo plain, zero, on, onThreads, strong;
            size_t bytesPlain = 0, bytesOn = 0;
            CHECK(roundTrip(c, 16, &bytesPlain, -1, nullptr, &plain));
            c.psyRd = 0.0;
            CHECK(roundTrip(c, 16, nullptr, -1, nullptr, &zero));
            CHECK(zero.hash == plain.hash);
            c.psyRd = 1.0;
            CHECK(roundTrip(c, 16, &bytesOn, -1, nullptr, &on));
            CHECK(on.hash != plain.hash);
            CHECK(on.minPsnrY >= plain.minPsnrY - 1.5);
            c.threads = 3; // CAVLC: a slice per thread, another stream
            CHECK(roundTrip(c, 16, nullptr, -1, nullptr, &onThreads));
            CHECK(onThreads.hash == on.hash || !gCabac);
            c.frameThreads = false;
            StreamInfo onRows;
            CHECK(roundTrip(c, 16, nullptr, -1, nullptr, &onRows));
            CHECK(onRows.hash == onThreads.hash);
            c.psyRd = 3.0;
            CHECK(roundTrip(c, 16, nullptr, -1, nullptr, &strong));
            if (!(on.psyDistance < plain.psyDistance && strong.psyDistance < on.psyDistance) || std::getenv("H264_TEST_PSY"))
                std::fprintf(stderr, "psy at QP %d: distance %.0f, with 1.0 %.0f, with 3.0 %.0f of %.0f; %zu -> %zu bytes\n", qp,
                             plain.psyDistance, on.psyDistance, strong.psyDistance, plain.psyFlat, bytesPlain, bytesOn);
            CHECK(on.psyDistance < plain.psyDistance);
            CHECK(strong.psyDistance < on.psyDistance);
        }
        // With the other tools: intra only, without the 8x8 transform (CABAC), every candidate coded in full,
        // several references, adaptive QP extremes, QP extremes, odd size with slices or the wavefront
        {
            EncoderConfig c = config(176, 144, IntraMode::Auto, 1);
            c.psyRd = 1.0;
            CHECK(roundTrip(c, 3));
            c = config(250, 142, IntraMode::Auto, 12, 3);
            c.psyRd = 2.0;
            CHECK(roundTrip(c, 12));
            c.transform8x8 = false;
            CHECK(roundTrip(c, 12));
            c.transform8x8 = true;
            c.modeEstimate = false;
            c.refs = 2;
            CHECK(roundTrip(c, 12));
            c.modeEstimate = true;
            c.aqStrength = 3;
            CHECK(roundTrip(c, 12));
            c.aqStrength = 0;
            c.effort = 0;
            CHECK(roundTrip(c, 12));
            for (int qp : {12, 44, 51}) {
                EncoderConfig q = config(176, 144, IntraMode::Auto, 10, 2);
                q.psyRd = 1.0;
                q.qp = qp;
                CHECK(roundTrip(q, 6));
            }
        }
        if (gCabac) { // B pictures: fixed runs and the adaptive placement, one thread and several the same
            for (const bool adaptive : {false, true}) {
                uint64_t hashOne = 0;
                for (int threads : {1, 4}) {
                    EncoderConfig c = config(320, 192, IntraMode::Auto, 30, threads);
                    c.bframes = 2;
                    c.bAdaptive = adaptive;
                    c.psyRd = 1.0;
                    StreamInfo info;
                    CHECK(roundTrip(c, 20, nullptr, -1, nullptr, &info));
                    CHECK(info.minPsnrY >= 30.0);
                    if (threads == 1)
                        hashOne = info.hash;
                    CHECK(info.hash == hashOne);
                }
            }
        }
        { // Rate control (one pass and with a first pass) still lands near its target
            EncoderConfig c = config(320, 192, IntraMode::Auto, 30, 2);
            c.bitrateKbps = 1000;
            c.fps = 30;
            c.psyRd = 1.0;
            size_t bytes = 0;
            CHECK(roundTrip(c, 60, &bytes));
            double kbps = bytes * 8.0 / 2.0 / 1000.0;
            if (std::abs(kbps - 1000.0) >= 250.0)
                std::fprintf(stderr, "rate control with psy: %.0f kbit/s instead of 1000\n", kbps);
            CHECK(std::abs(kbps - 1000.0) < 250.0);
            FirstPass fp(c.width, c.height);
            for (int t = 0; t < 60; ++t) {
                const Yuv src = synthFrame(c.width, c.height, t);
                fp.add(Picture{src.y.data(), src.u.data(), src.v.data(), src.w, src.w / 2});
            }
            c.firstPass = fp.stats();
            bytes = 0;
            CHECK(roundTrip(c, 60, &bytes));
            kbps = bytes * 8.0 / 2.0 / 1000.0;
            if (std::abs(kbps - 1000.0) >= 250.0)
                std::fprintf(stderr, "first pass with psy: %.0f kbit/s instead of 1000\n", kbps);
            CHECK(std::abs(kbps - 1000.0) < 250.0);
        }
    }
    // level_idc (Table A-1) in the SPS: 720p30 3.1, 1080p60 4.2, 4K30 5.1; VUI colours decode
    {
        auto level = [](int w, int h, double fps) {
            EncoderConfig c;
            c.width = w;
            c.height = h;
            c.fps = fps;
            const std::vector<uint8_t> hdr = Encoder(c).headers();
            return int(hdr[7]); // start code, NAL header, profile_idc, constraint flags, level_idc
        };
        CHECK(level(1280, 720, 30) == 31);
        CHECK(level(1920, 1080, 60) == 42);
        CHECK(level(3840, 2160, 30) == 51);
        CHECK(level(3840, 2160, 60) == 52);
        EncoderConfig c = config(176, 144, IntraMode::Auto, 6, 2);
        c.colourPrimaries = c.transferCharacteristics = c.matrixCoefficients = 1;
        c.fullRange = true;
        CHECK(roundTrip(c, 4));
    }
    // Chunks side by side: independent encoders, streams concatenated, one decoder
    {
        EncoderConfig c = config(320, 192, IntraMode::Auto, 30, 2);
        c.fps = 30;
        FirstPass fp(c.width, c.height);
        for (int t = 0; t < 60; ++t) {
            const Yuv src = synthFrame(c.width, c.height, t);
            fp.add(Picture{src.y.data(), src.u.data(), src.v.data(), src.w, src.w / 2});
        }
        const ChunkPlan plan = planChunks(*fp.stats(), c, 3, 10);
        CHECK(plan.start.size() == 3);
        CHECK(roundTrip(c, 60, nullptr, -1, &plan)); // fixed QP
        c.bitrateKbps = 1000;
        const ChunkPlan rated = planChunks(*fp.stats(), c, 3, 10);
        CHECK(rated.bitrateKbps.size() == 3);
        size_t bytes = 0;
        CHECK(roundTrip(c, 60, &bytes, -1, &rated));
        const double kbps = bytes * 8.0 / 2.0 / 1000.0;
        if (std::abs(kbps - 1000.0) >= 250.0)
            std::fprintf(stderr, "chunks: %.0f kbit/s instead of 1000\n", kbps);
        CHECK(std::abs(kbps - 1000.0) < 250.0);
    }
    }
    // Failures on the encoder's own threads come out of the caller's next call (not std::terminate),
    // and the encoder stays failed; a new encoder works again.
    {
        auto encodeAll = [](FailPoint p, int after, int threads, bool frameThreads) {
            EncoderConfig c = config(320, 192, IntraMode::Auto, 30, threads);
            c.frameThreads = frameThreads;
            injectFailure(p, after);
            int thrown = 0;
            {
                Encoder enc(c);
                for (int t = 0; t < 50; ++t) {
                    const Yuv src = synthFrame(c.width, c.height, t);
                    try {
                        enc.encode(Picture{src.y.data(), src.u.data(), src.v.data(), src.w, src.w / 2});
                    } catch (const std::runtime_error&) {
                        ++thrown;
                    }
                }
                try {
                    while (!enc.flush().empty()) {
                    }
                } catch (const std::runtime_error&) {
                    ++thrown;
                }
            } // the destructor must not hang or terminate either
            injectFailure(p, 0);
            return thrown;
        };
        CHECK(encodeAll(FailPoint::WaveRow, 30, 4, true) >= 2);   // pool worker, frame threading
        CHECK(encodeAll(FailPoint::WaveRow, 30, 4, false) >= 2);  // wavefront threads of one picture
        CHECK(encodeAll(FailPoint::Writer, 3, 4, true) >= 2);     // entropy writer thread
        CHECK(encodeAll(FailPoint::LookaheadThread, 10, 4, true) >= 2);
        CHECK(encodeAll(FailPoint::WaveRow, 5, 1, true) >= 2);     // one thread: the caller's own
        CHECK(encodeAll(FailPoint::WaveRow, 0, 4, true) == 0);
        injectFailure(FailPoint::FirstPassWorker, 30);
        bool firstPassThrew = false;
        try {
            FirstPass fp(320, 192, 4);
            for (int t = 0; t < 60; ++t) {
                const Yuv src = synthFrame(320, 192, t);
                fp.add(Picture{src.y.data(), src.u.data(), src.v.data(), src.w, src.w / 2});
            }
            fp.stats();
        } catch (const std::runtime_error&) {
            firstPassThrew = true;
        }
        injectFailure(FailPoint::FirstPassWorker, 0);
        CHECK(firstPassThrew);
        CHECK(roundTrip(config(176, 144, IntraMode::Auto, 8, 4), 8));
    }
    testVbv();
    testCpuBuilds();
    testHints();
    return Check::result();
}
