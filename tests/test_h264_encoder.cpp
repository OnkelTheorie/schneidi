// SPDX-License-Identifier: AGPL-3.0-only
// schneidi's own H.264 encoder (prototype): every stream must decode with FFmpeg's decoder to exactly the
// pictures the encoder reconstructed (CAVLC and CABAC; I and P frames, all intra/inter modes, slices, adaptive QP,
// rate control, odd sizes). With B pictures the access units come in coding order: the decoder must hand the
// pictures out in display order with the right timestamps. The psycho-visual mode decision (psyRd): off = the same
// stream as without the option, on = decodable, the same with any thread count, and closer to the source's AC energy.

#include "check.h"

#include "engine/encoder/h264/Cavlc.h"
#include "engine/encoder/h264/Deblock.h"
#include "engine/encoder/h264/Satd.h"
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

// Moving gradient, a moving textured square and a little noise: exercises intra modes, motion and skip.
Yuv synthFrame(int w, int h, int t)
{
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
            const int sx = (t * 3) % w, sy = (t * 2) % h;
            if (x >= sx && x < sx + 48 && y >= sy && y < sy + 40)
                v = ((x / 4 + y / 4) & 1) ? 220 : 40;
            f.y[size_t(y) * w + x] = uint8_t(std::clamp(v, 0, 255));
        }
    for (int y = 0; y < h / 2; ++y)
        for (int x = 0; x < w / 2; ++x) {
            f.u[size_t(y) * (w / 2) + x] = uint8_t(128 + (x - w / 4) / 3 + t);
            f.v[size_t(y) * (w / 2) + x] = uint8_t(128 + (y - h / 4) / 2);
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
    double minPsnrY = 99;   // lowest luma PSNR of a reconstruction against its source picture
    uint64_t hash = 1469598103934665603ull; // FNV-1a of the whole stream
    // AC energy (acEnergy8x8U8, both scales) over the whole 8x8 luma blocks of all pictures: the source's, and the
    // summed difference between each block's and its reconstruction's
    double energySource = 0, energyGap = 0;
};

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
            const Yuv src = synthFrame(cfg.width, cfg.height, display);
            double sse = 0;
            for (size_t i = 0; i < src.y.size(); ++i)
                sse += double(int(src.y[i]) - int(r.y[i])) * double(int(src.y[i]) - int(r.y[i]));
            info->minPsnrY = std::min(info->minPsnrY, sse > 0 ? 10.0 * std::log10(255.0 * 255.0 * double(src.y.size()) / sse) : 99.0);
            for (int y = 0; y + 8 <= r.h; y += 8)
                for (int x = 0; x + 8 <= r.w; x += 8) {
                    int s4, s8, r4, r8;
                    acEnergy8x8U8(&src.y[size_t(y) * r.w + x], r.w, s4, s8);
                    acEnergy8x8U8(&r.y[size_t(y) * r.w + x], r.w, r4, r8);
                    info->energySource += s4 + s8;
                    info->energyGap += std::abs(s4 - r4) + std::abs(s8 - r8);
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
        std::vector<uint8_t> au = encPtr->encode(pic, t == keyframeAt);
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
    ok = ok && decoded == size_t(frames);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    if (bytesOut)
        *bytesOut = bytes;
    return ok;
}

bool gCabac = true; // every case runs with both entropy coders

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
            // AC energy of an 8x8 block against plain Hadamard sums without their DC coefficients
            int e4, e8, ref4 = 0, ref8;
            acEnergy8x8U8(a.data(), S, e4, e8);
            int h8[64], sum = 0;
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x)
                    sum += h8[y * 8 + x] = a[size_t(y) * S + x];
            for (int q = 0; q < 4; ++q) {
                int in[16], out[16], abs4 = 0;
                for (int i = 0; i < 16; ++i)
                    in[i] = h8[((q >> 1) * 4 + (i >> 2)) * 8 + (q & 1) * 4 + (i & 3)];
                hadamard4x4(in, out);
                for (int v : out)
                    abs4 += std::abs(v);
                ref4 += (abs4 + 1) >> 1;
            }
            ref4 -= sum >> 1;
            for (int pass = 0; pass < 2; ++pass)
                for (int line = 0; line < 8; ++line)
                    for (int step = 1; step < 8; step <<= 1)
                        for (int i = 0; i < 8; i += 2 * step)
                            for (int j = i; j < i + step; ++j) {
                                int& u = pass ? h8[j * 8 + line] : h8[line * 8 + j];
                                int& v = pass ? h8[(j + step) * 8 + line] : h8[line * 8 + j + step];
                                const int p = u, m = v;
                                u = p + m;
                                v = p - m;
                            }
            int abs8 = 0;
            for (int v : h8)
                abs8 += std::abs(v);
            ref8 = ((abs8 + 2) >> 2) - (sum >> 2);
            if (e4 != ref4 || e8 != ref8 || e4 < 0 || e8 < 0)
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

} // namespace

int main()
{
    Check::initEnv();
    CHECK(selfTestTables());
    CHECK(kernelsMatchReference());
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
        // The encoder takes the first pass's analysis instead of repeating it: the same stream with
        // none of it kept and with only the first pictures kept (memory limit).
        CHECK(c.firstPass->analysis.size() == 60);
        const size_t perPicture = size_t((c.width + 15) / 16) * ((c.height + 15) / 16) * 12;
        for (size_t keep : {size_t(0), 25 * perPicture}) {
            FirstPass fpKeep(c.width, c.height, 1, false, keep);
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
    // B pictures (CABAC only): coding order, display order and timestamps, every picture bit-exact
    if (gCabac) {
        for (int bframes : {1, 2, 3}) {
            for (int threads : {1, 3}) {
                EncoderConfig c = config(176, 144, IntraMode::Auto, 30, threads);
                c.bframes = bframes;
                c.bAdaptive = false;
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
                        orderOk = info.order[i] > std::max(before, lastB) && info.order[i] < after;
                        lastB = info.order[i];
                    } else {
                        orderOk = info.order[i] > after;
                        before = after;
                        after = info.order[i];
                        lastB = -1;
                    }
                }
                CHECK(orderOk);
                if (info.minPsnrY < 30.0)
                    std::fprintf(stderr, "bframes %d: PSNR-Y %.2f dB\n", bframes, info.minPsnrY);
                CHECK(info.minPsnrY >= 30.0);
                // The same stream with any thread count.
                static size_t bytesOne[4] = {};
                if (threads == 1)
                    bytesOne[bframes] = bytes;
                CHECK(bytes == bytesOne[bframes]);
            }
        }
        {
            // Smaller than without B pictures at the same QP on this (easy) material
            EncoderConfig c = config(320, 192, IntraMode::Auto, 30, 2);
            size_t plain = 0, withB = 0;
            CHECK(roundTrip(c, 31, &plain));
            c.bframes = 2;
            CHECK(roundTrip(c, 31, &withB));
            if (withB >= plain)
                std::fprintf(stderr, "B pictures: %zu bytes, without %zu\n", withB, plain);
            CHECK(withB < plain);
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
            FirstPass fp(c.width, c.height, 2, true); // with the scores of the adaptive placement
            for (int t = 0; t < 60; ++t) {
                const Yuv src = synthFrame(c.width, c.height, t);
                fp.add(Picture{src.y.data(), src.u.data(), src.v.data(), src.w, src.w / 2});
            }
            c.firstPass = fp.stats();
            CHECK(c.firstPass->bScore.size() == 60);
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
        // one thread or several the same stream, and the reconstruction is closer to the source's AC energy.
        for (int qp : {24, 34}) {
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
            if (!(on.energyGap < plain.energyGap && strong.energyGap < on.energyGap))
                std::fprintf(stderr, "psy at QP %d: energy gap %.0f, with 1.0 %.0f, with 3.0 %.0f of %.0f; %zu -> %zu bytes\n", qp,
                             plain.energyGap, on.energyGap, strong.energyGap, plain.energySource, bytesPlain, bytesOn);
            CHECK(on.energyGap < plain.energyGap);
            CHECK(strong.energyGap < on.energyGap);
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
    return Check::result();
}
