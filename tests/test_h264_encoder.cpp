// SPDX-License-Identifier: AGPL-3.0-only
// schneidi's own H.264 encoder (prototype): every stream must decode with FFmpeg's decoder to exactly the
// pictures the encoder reconstructed (CAVLC and CABAC; I and P frames, all intra/inter modes, slices, adaptive QP,
// rate control, odd sizes).

#include "check.h"

#include "engine/encoder/h264/Cavlc.h"
#include "engine/encoder/h264/H264Encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
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
bool roundTrip(const EncoderConfig& cfg, int frames, size_t* bytesOut = nullptr, int keyframeAt = -1,
               const ChunkPlan* plan = nullptr)
{
    auto encPtr = std::make_unique<Encoder>(plan ? plan->configFor(cfg, 0, cfg.threads) : cfg);
    Encoder& enc0 = *encPtr;
    (void)enc0;
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec)
        return false;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->thread_count = 1;
    if (avcodec_open2(ctx, codec, nullptr) < 0)
        return false;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    std::vector<Yuv> recon;
    size_t decoded = 0, bytes = 0;
    bool ok = true;
    auto receive = [&] {
        while (avcodec_receive_frame(ctx, frame) == 0) {
            if (decoded >= recon.size()) {
                ok = false;
                return;
            }
            const Yuv& r = recon[decoded++];
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
        recon.push_back(std::move(r));

        av_new_packet(pkt, int(au.size()));
        std::memcpy(pkt->data, au.data(), au.size());
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

} // namespace

int main()
{
    Check::initEnv();
    CHECK(selfTestTables());
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
    return Check::result();
}
