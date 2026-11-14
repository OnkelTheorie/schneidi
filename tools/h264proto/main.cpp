// h264-proto: test driver for schneidi's own H.264 encoder.
//   h264-proto selftest
//   h264-proto encode <in.yuv> <width> <height> <out.h264> [--qp N] [--mode pcm|i16|i4|auto]
//                     [--frames N] [--gop N (120)] [--threads N] [--bitrate kbps] [--effort 0|1] [--aq strength] [--refs N] [--cavlc] [--no-t8] [--no-deblock]
//                     [--lookahead N] [--no-mbtree] [--mbtree-strength S] [--mbtree-strength-b S] [--2pass] [--recon out.yuv]
//                     [--bframes N (8)] [--b-qp offset (2)] [--no-b-adapt] [--weightb] [--psy-rd S (1)] [--psy-qp full off (26 38)] [--qcomp S (0.3)]
// --bframes N: up to N B pictures in a row (CABAC only). Access units are written in coding order,
// PSNR and --recon go by display order.
// --psy-rd S: psycho-visual mode decision (keeps texture and grain at the cost of PSNR), 0 = off.
// --2pass (with --bitrate): analyse the whole input first (lookahead only), then one QP for the clip,
// moved per picture by --qcomp S times its complexity against the clip's (0 = one QP throughout).
// --order file: one line per access unit, "display index, bytes", in coding order (a muxer needs it for
// the timestamps of streams with B pictures; not with --chunks).
// --chunks N: N independent encoders side by side on parts of the clip (planChunks), --threads split
// between them; implies the first pass. Input must be a file. --chunk-min F: shortest chunk in
// pictures (default 15 s).
// Input is raw yuv420p ("-" reads stdin). Prints size, speed and PSNR of the encoder's reconstruction.
#include "engine/encoder/h264/Cavlc.h"
#include "engine/encoder/h264/H264Encoder.h"
#include "engine/encoder/h264/ThreadErrors.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <thread>
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include <vector>

using namespace schneidi::h264;

static double psnr(const uint8_t* a, int strideA, const uint8_t* b, int strideB, int w, int h)
{
    // Whole-number sums per row (the compiler vectorises them); the total is exact either way.
    uint64_t total = 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* ra = a + size_t(y) * strideA;
        const uint8_t* rb = b + size_t(y) * strideB;
        uint32_t row = 0;
        for (int x = 0; x < w; ++x) {
            const int d = int(ra[x]) - int(rb[x]);
            row += uint32_t(d * d);
        }
        total += row;
    }
    const double sse = double(total);
    if (sse == 0)
        return 99.0;
    return 10.0 * std::log10(255.0 * 255.0 * w * h / sse);
}

// Peak resident memory in MB (0 where unknown).
static long peakMb()
{
#ifndef _WIN32
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return u.ru_maxrss / 1024;
#else
    return 0;
#endif
}

static bool seekFrame(FILE* f, int64_t offset)
{
#ifdef _WIN32
    return _fseeki64(f, offset, SEEK_SET) == 0;
#else
    return fseeko(f, off_t(offset), SEEK_SET) == 0;
#endif
}

// One chunk: pictures [start, start + count) of the file through its own encoder.
struct ChunkResult {
    std::vector<uint8_t> bytes;
    double psnrY = 0, psnrU = 0, psnrV = 0;
    int frames = 0;
};

static void encodeChunk(const EncoderConfig& cfg, const char* path, int start, int count, ChunkResult& res)
{
    const int w = cfg.width, h = cfg.height, cw = w / 2, ch = h / 2;
    const size_t frameSize = size_t(w) * h * 3 / 2;
    FILE* in = std::fopen(path, "rb");
    if (!in || !seekFrame(in, int64_t(start) * int64_t(frameSize))) {
        if (in)
            std::fclose(in);
        return;
    }
    Encoder enc(cfg);
    std::map<int, std::vector<uint8_t>> pending; // by display index: B pictures come out of order
    auto took = [&](const std::vector<uint8_t>& au) {
        if (au.empty())
            return;
        res.bytes.insert(res.bytes.end(), au.begin(), au.end());
        const std::vector<uint8_t>& src = pending.at(enc.lastDisplayIndex());
        const uint8_t* sy = src.data();
        const uint8_t* su = sy + size_t(w) * h;
        const uint8_t* sv = su + size_t(cw) * ch;
        const int pw = enc.paddedWidth();
        res.psnrY += psnr(sy, w, enc.reconY().data(), pw, w, h);
        res.psnrU += psnr(su, cw, enc.reconU().data(), pw / 2, cw, ch);
        res.psnrV += psnr(sv, cw, enc.reconV().data(), pw / 2, cw, ch);
        pending.erase(enc.lastDisplayIndex());
        ++res.frames;
    };
    std::vector<uint8_t> frame(frameSize);
    for (int i = 0; i < count && std::fread(frame.data(), 1, frameSize, in) == frameSize; ++i) {
        Picture pic;
        pic.y = frame.data();
        pic.u = pic.y + size_t(w) * h;
        pic.v = pic.u + size_t(cw) * ch;
        pic.strideY = w;
        pic.strideC = cw;
        pending[i] = frame;
        took(enc.encode(pic));
    }
    for (;;) {
        const auto au = enc.flush();
        if (au.empty())
            break;
        took(au);
    }
    std::fclose(in);
}

int main(int argc, char** argv)
{
    if (argc >= 2 && std::strcmp(argv[1], "selftest") == 0) {
        const bool ok = selfTestTables();
        std::printf("tables: %s\n", ok ? "ok" : "FAILED");
        return ok ? 0 : 1;
    }
    if (argc < 6 || std::strcmp(argv[1], "encode") != 0) {
        std::fprintf(stderr, "usage: h264-proto selftest | encode <in.yuv> <w> <h> <out.h264> [--qp N] "
                             "[--mode pcm|i16|i4|auto] [--frames N] [--gop N (120)] [--threads N] [--bitrate kbps] [--effort 0|1] [--aq strength] [--refs N] [--cavlc] [--no-t8] [--no-deblock] [--recon out.yuv]\n");
        return 2;
    }
    EncoderConfig cfg;
    cfg.gop = 120; // P pictures by default, like the export (the library's own default is intra only)
    const char* inPath = argv[2];
    cfg.width = std::atoi(argv[3]);
    cfg.height = std::atoi(argv[4]);
    const char* outPath = argv[5];
    int maxFrames = 1 << 30;
    const char* reconPath = nullptr;
    const char* orderPath = nullptr; // --order file: per access unit "display index, bytes" (for a muxer: pts with B pictures)
    bool twoPass = false;
    int chunks = 1, chunkMin = 0;
    for (int i = 6; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--qp" && i + 1 < argc)
            cfg.qp = std::atoi(argv[++i]);
        else if (a == "--mode" && i + 1 < argc)
            {
                const std::string m = argv[++i];
                cfg.mode = m == "pcm" ? IntraMode::Pcm : m == "i16" ? IntraMode::Intra16 : m == "i4" ? IntraMode::Intra4 : IntraMode::Auto;
            }
        else if (a == "--frames" && i + 1 < argc)
            maxFrames = std::atoi(argv[++i]);
        else if (a == "--gop" && i + 1 < argc)
            cfg.gop = std::atoi(argv[++i]);
        else if (a == "--threads" && i + 1 < argc)
            cfg.threads = std::atoi(argv[++i]);
        else if (a == "--bitrate" && i + 1 < argc)
            cfg.bitrateKbps = std::atoi(argv[++i]);
        else if (a == "--effort" && i + 1 < argc)
            cfg.effort = std::atoi(argv[++i]);
        else if (a == "--aq" && i + 1 < argc)
            cfg.aqStrength = std::atof(argv[++i]);
        else if (a == "--refs" && i + 1 < argc)
            cfg.refs = std::atoi(argv[++i]);
        else if (a == "--no-scenecut")
            cfg.sceneCut = false;
        else if (a == "--lookahead" && i + 1 < argc)
            cfg.lookahead = std::atoi(argv[++i]);
        else if (a == "--fps" && i + 1 < argc)
            cfg.fps = std::atof(argv[++i]);
        else if (a == "--no-mbtree")
            cfg.mbtree = false;
        else if (a == "--mbtree-strength" && i + 1 < argc)
            cfg.mbtreeStrength = std::atof(argv[++i]);
        else if (a == "--mbtree-strength-b" && i + 1 < argc)
            cfg.mbtreeStrengthB = std::atof(argv[++i]);
        else if (a == "--no-t8")
            cfg.transform8x8 = false;
        else if (a == "--cavlc")
            cfg.cabac = false;
        else if (a == "--no-deblock")
            cfg.deblock = false;
        else if (a == "--chunk-min" && i + 1 < argc)
            chunkMin = std::atoi(argv[++i]);
        else if (a == "--chunks" && i + 1 < argc)
            chunks = std::max(1, std::atoi(argv[++i]));
        else if (a == "--no-frame-threads")
            cfg.frameThreads = false;
        else if (a == "--bt709")
            cfg.colourPrimaries = cfg.transferCharacteristics = cfg.matrixCoefficients = 1;
        else if (a == "--full-range")
            cfg.fullRange = true;
        else if (a == "--2pass")
            twoPass = true;
        else if (a == "--bframes" && i + 1 < argc)
            cfg.bframes = std::atoi(argv[++i]);
        else if (a == "--b-qp" && i + 1 < argc)
            cfg.bQpOffset = std::atoi(argv[++i]);
        else if (a == "--no-b-adapt")
            cfg.bAdaptive = false;
        else if (a == "--chroma-qp" && i + 1 < argc)
            cfg.chromaQpOffset = std::atoi(argv[++i]);
        else if (a == "--chroma-rd" && i + 1 < argc)
            cfg.chromaRdWeight = std::atoi(argv[++i]);
        else if (a == "--no-weightp")
            cfg.weightedPred = false;
        else if (a == "--weightb")
            cfg.weightedBipred = true;
        else if (a == "--psy-rd" && i + 1 < argc)
            cfg.psyRd = std::atof(argv[++i]);
        else if (a == "--psy-qp" && i + 2 < argc) {
            cfg.psyQpFull = std::atoi(argv[++i]);
            cfg.psyQpOff = std::atoi(argv[++i]);
        } else if (a == "--qcomp" && i + 1 < argc)
            cfg.qcomp = std::atof(argv[++i]);
        else if (a == "--recon" && i + 1 < argc)
            reconPath = argv[++i];
        else if (a == "--order" && i + 1 < argc)
            orderPath = argv[++i];
    }

    FILE* in = std::strcmp(inPath, "-") == 0 ? stdin : std::fopen(inPath, "rb"); // "-" = stdin
    FILE* out = std::fopen(outPath, "wb");
    FILE* recon = reconPath ? std::fopen(reconPath, "wb") : nullptr;
    if (!in || !out || (reconPath && !recon)) {
        std::fprintf(stderr, "cannot open files\n");
        return 1;
    }

    const int w = cfg.width, h = cfg.height, cw = w / 2, ch = h / 2;
    std::vector<uint8_t> frame(size_t(w) * h * 3 / 2);
    double firstPassSeconds = 0;
    if (chunks > 1 && in != stdin) {
        // First pass over the whole clip, then the chunks side by side.
        auto t0 = std::chrono::steady_clock::now();
        FirstPass fp(w, h, cfg.threads, cfg.bAdaptive ? cfg.bframes : 0);
        for (int n = 0; n < maxFrames && std::fread(frame.data(), 1, frame.size(), in) == frame.size(); ++n) {
            Picture pic;
            pic.y = frame.data();
            pic.strideY = w;
            fp.add(pic);
        }
        std::fclose(in);
        const int total = int(fp.stats()->size());
        const ChunkPlan plan = planChunks(*fp.stats(), cfg, chunks, chunkMin);
        firstPassSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const int count = int(plan.start.size());
        const int per = std::max(1, cfg.threads / count);
        std::vector<ChunkResult> res(static_cast<size_t>(count));
        t0 = std::chrono::steady_clock::now();
        std::atomic<int> nextChunk{0};
        runOnThreads(count, [&] {
            for (int c; (c = nextChunk.fetch_add(1)) < count;) {
                const int start = plan.start[size_t(c)];
                const int end = c + 1 < count ? plan.start[size_t(c) + 1] : total;
                encodeChunk(plan.configFor(cfg, c, per), inPath, start, end - start, res[size_t(c)]);
            }
        });
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const auto hdr = Encoder(cfg).headers();
        std::fwrite(hdr.data(), 1, hdr.size(), out);
        size_t totalBytes = hdr.size();
        int frames = 0;
        double py = 0, pu = 0, pv = 0;
        std::printf("chunks");
        for (int c = 0; c < count; ++c) {
            const ChunkResult& r = res[size_t(c)];
            std::fwrite(r.bytes.data(), 1, r.bytes.size(), out);
            totalBytes += r.bytes.size();
            frames += r.frames;
            py += r.psnrY;
            pu += r.psnrU;
            pv += r.psnrV;
            std::printf(" %d", plan.start[size_t(c)]);
            if (size_t(c) < plan.bitrateKbps.size())
                std::printf("@%d", plan.bitrateKbps[size_t(c)]);
        }
        std::fclose(out);
        if (!frames) {
            std::fprintf(stderr, "no frames read\n");
            return 1;
        }
        std::printf("  threads %dx%d  first pass %.2f s  peak %ld MB  ", count, per, firstPassSeconds, peakMb());
        std::printf("frames %d  bytes %zu (%.1f KB/frame)  %.2f fps  PSNR Y %.2f U %.2f V %.2f\n", frames, totalBytes,
                    totalBytes / 1024.0 / frames, frames / seconds, py / frames, pu / frames, pv / frames);
        return 0;
    }
    if (twoPass && in != stdin) {
        const auto t0 = std::chrono::steady_clock::now();
        FirstPass fp(w, h, cfg.threads, cfg.bAdaptive ? cfg.bframes : 0);
        for (int n = 0; n < maxFrames && std::fread(frame.data(), 1, frame.size(), in) == frame.size(); ++n) {
            Picture pic;
            pic.y = frame.data();
            pic.strideY = w;
            fp.add(pic);
        }
        cfg.firstPass = fp.stats();
        std::rewind(in);
        firstPassSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    Encoder enc(cfg);
    const auto hdr = enc.headers();
    std::fwrite(hdr.data(), 1, hdr.size(), out);
    size_t totalBytes = hdr.size();

    int frames = 0, fed = 0;
    double psnrY = 0, psnrU = 0, psnrV = 0, seconds = 0;
    // The encoder may hold pictures back (lookahead): PSNR against the source picture that came out.
    std::map<int, std::vector<uint8_t>> pending; // by display index: B pictures come out of order
    std::vector<std::vector<uint8_t>> spare;
    double typePsnr[3] = {}, typeBytes[3] = {};
    int typeCount[3] = {};
    FILE* order = orderPath ? std::fopen(orderPath, "w") : nullptr;
    auto took = [&](const std::vector<uint8_t>& au) {
        if (au.empty())
            return;
        std::fwrite(au.data(), 1, au.size(), out);
        if (order)
            std::fprintf(order, "%d %zu\n", enc.lastDisplayIndex(), au.size());
        totalBytes += au.size();
        const std::vector<uint8_t>& src = pending.at(enc.lastDisplayIndex());
        const uint8_t* sy = src.data();
        const uint8_t* su = sy + size_t(w) * h;
        const uint8_t* sv = su + size_t(cw) * ch;
        const int pw = enc.paddedWidth();
        const double py = psnr(sy, w, enc.reconY().data(), pw, w, h);
        psnrY += py;
        psnrU += psnr(su, cw, enc.reconU().data(), pw / 2, cw, ch);
        psnrV += psnr(sv, cw, enc.reconV().data(), pw / 2, cw, ch);
        const int t = enc.lastType() == 'I' ? 0 : enc.lastType() == 'P' ? 1 : 2;
        typePsnr[t] += py;
        typeBytes[t] += double(au.size());
        ++typeCount[t];
        if (std::getenv("H264_PROTO_TRACE"))
            std::fprintf(stderr, "frame %d display %d %c qp %d bytes %zu psnr %.2f%s\n", frames, enc.lastDisplayIndex(),
                         enc.lastType(), enc.lastQp(), au.size(), py, enc.lastWasSceneCut() ? " scene cut" : "");
        if (recon) {
            if (cfg.bframes > 0) // display order
                seekFrame(recon, int64_t(enc.lastDisplayIndex()) * int64_t(frame.size()));
            for (int y = 0; y < h; ++y)
                std::fwrite(enc.reconY().data() + size_t(y) * pw, 1, size_t(w), recon);
            for (const auto* p : {&enc.reconU(), &enc.reconV()})
                for (int y = 0; y < ch; ++y)
                    std::fwrite(p->data() + size_t(y) * (pw / 2), 1, size_t(cw), recon);
        }
        spare.push_back(std::move(pending.at(enc.lastDisplayIndex()))); // read buffers go round
        pending.erase(enc.lastDisplayIndex());
        ++frames;
    };
    while (fed < maxFrames && std::fread(frame.data(), 1, frame.size(), in) == frame.size()) {
        Picture pic;
        pic.y = frame.data();
        pic.u = pic.y + size_t(w) * h;
        pic.v = pic.u + size_t(cw) * ch;
        pic.strideY = w;
        pic.strideC = cw;
        const auto t0 = std::chrono::steady_clock::now();
        const auto au = enc.encode(pic); // copies the picture
        seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        pending[fed] = std::move(frame);
        ++fed;
        if (spare.empty()) {
            frame = std::vector<uint8_t>(pending.begin()->second.size());
        } else {
            frame = std::move(spare.back());
            spare.pop_back();
        }
        took(au);
    }
    for (;;) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto au = enc.flush();
        seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (au.empty())
            break;
        took(au);
    }
    std::fclose(in);
    std::fclose(out);
    if (order)
        std::fclose(order);
    if (recon)
        std::fclose(recon);
    if (!frames) {
        std::fprintf(stderr, "no frames read\n");
        return 1;
    }
    if (cfg.firstPass)
        std::printf("first pass %.2f s (%.1f ms/frame)  ", firstPassSeconds, firstPassSeconds * 1000.0 / frames);
    if (typeCount[2])
        std::printf("I %d (%.1f KB, %.2f dB) P %d (%.1f KB, %.2f dB) B %d (%.1f KB, %.2f dB)  ", typeCount[0],
                    typeBytes[0] / 1024.0 / std::max(1, typeCount[0]), typePsnr[0] / std::max(1, typeCount[0]), typeCount[1],
                    typeBytes[1] / 1024.0 / std::max(1, typeCount[1]), typePsnr[1] / std::max(1, typeCount[1]), typeCount[2],
                    typeBytes[2] / 1024.0 / typeCount[2], typePsnr[2] / typeCount[2]);
    std::printf("peak %ld MB  last qp %d  ", peakMb(), enc.lastQp());
    std::printf("frames %d  bytes %zu (%.1f KB/frame)  %.2f fps  PSNR Y %.2f U %.2f V %.2f\n", frames, totalBytes,
                totalBytes / 1024.0 / frames, frames / seconds, psnrY / frames, psnrU / frames, psnrV / frames);
    return 0;
}
