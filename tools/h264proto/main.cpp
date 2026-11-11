// h264-proto: test driver for schneidi's own H.264 encoder.
//   h264-proto selftest
//   h264-proto encode <in.yuv> <width> <height> <out.h264> [--qp N] [--mode pcm|i16|i4|auto]
//                     [--frames N] [--gop N] [--threads N] [--bitrate kbps] [--effort 0|1] [--aq strength] [--refs N] [--cavlc] [--no-t8] [--no-deblock]
//                     [--lookahead N] [--no-mbtree] [--mbtree-strength S] [--recon out.yuv]
// Input is raw yuv420p ("-" reads stdin). Prints size, speed and PSNR of the encoder's reconstruction.
#include "engine/encoder/h264/Cavlc.h"
#include "engine/encoder/h264/H264Encoder.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

using namespace schneidi::h264;

static double psnr(const uint8_t* a, int strideA, const uint8_t* b, int strideB, int w, int h)
{
    double sse = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int d = int(a[size_t(y) * strideA + x]) - int(b[size_t(y) * strideB + x]);
            sse += double(d * d);
        }
    if (sse == 0)
        return 99.0;
    return 10.0 * std::log10(255.0 * 255.0 * w * h / sse);
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
                             "[--mode pcm|i16|i4|auto] [--frames N] [--gop N] [--threads N] [--bitrate kbps] [--effort 0|1] [--aq strength] [--refs N] [--cavlc] [--no-t8] [--no-deblock] [--recon out.yuv]\n");
        return 2;
    }
    EncoderConfig cfg;
    const char* inPath = argv[2];
    cfg.width = std::atoi(argv[3]);
    cfg.height = std::atoi(argv[4]);
    const char* outPath = argv[5];
    int maxFrames = 1 << 30;
    const char* reconPath = nullptr;
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
        else if (a == "--no-t8")
            cfg.transform8x8 = false;
        else if (a == "--cavlc")
            cfg.cabac = false;
        else if (a == "--no-deblock")
            cfg.deblock = false;
        else if (a == "--recon" && i + 1 < argc)
            reconPath = argv[++i];
    }

    FILE* in = std::strcmp(inPath, "-") == 0 ? stdin : std::fopen(inPath, "rb"); // "-" = stdin
    FILE* out = std::fopen(outPath, "wb");
    FILE* recon = reconPath ? std::fopen(reconPath, "wb") : nullptr;
    if (!in || !out || (reconPath && !recon)) {
        std::fprintf(stderr, "cannot open files\n");
        return 1;
    }

    Encoder enc(cfg);
    const auto hdr = enc.headers();
    std::fwrite(hdr.data(), 1, hdr.size(), out);
    size_t totalBytes = hdr.size();

    const int w = cfg.width, h = cfg.height, cw = w / 2, ch = h / 2;
    std::vector<uint8_t> frame(size_t(w) * h * 3 / 2);
    int frames = 0, fed = 0;
    double psnrY = 0, psnrU = 0, psnrV = 0, seconds = 0;
    // The encoder may hold pictures back (lookahead): PSNR against the source picture that came out.
    std::deque<std::vector<uint8_t>> pending;
    auto took = [&](const std::vector<uint8_t>& au) {
        if (au.empty())
            return;
        std::fwrite(au.data(), 1, au.size(), out);
        if (std::getenv("H264_PROTO_TRACE"))
            std::fprintf(stderr, "frame %d qp %d bytes %zu%s\n", frames, enc.lastQp(), au.size(),
                         enc.lastWasSceneCut() ? " scene cut" : "");
        totalBytes += au.size();
        const std::vector<uint8_t>& src = pending.front();
        const uint8_t* sy = src.data();
        const uint8_t* su = sy + size_t(w) * h;
        const uint8_t* sv = su + size_t(cw) * ch;
        const int pw = enc.paddedWidth();
        psnrY += psnr(sy, w, enc.reconY().data(), pw, w, h);
        psnrU += psnr(su, cw, enc.reconU().data(), pw / 2, cw, ch);
        psnrV += psnr(sv, cw, enc.reconV().data(), pw / 2, cw, ch);
        if (recon) {
            for (int y = 0; y < h; ++y)
                std::fwrite(enc.reconY().data() + size_t(y) * pw, 1, size_t(w), recon);
            for (const auto* p : {&enc.reconU(), &enc.reconV()})
                for (int y = 0; y < ch; ++y)
                    std::fwrite(p->data() + size_t(y) * (pw / 2), 1, size_t(cw), recon);
        }
        pending.pop_front();
        ++frames;
    };
    while (fed < maxFrames && std::fread(frame.data(), 1, frame.size(), in) == frame.size()) {
        Picture pic;
        pic.y = frame.data();
        pic.u = pic.y + size_t(w) * h;
        pic.v = pic.u + size_t(cw) * ch;
        pic.strideY = w;
        pic.strideC = cw;
        pending.push_back(frame);
        ++fed;
        const auto t0 = std::chrono::steady_clock::now();
        const auto au = enc.encode(pic);
        seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
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
    if (recon)
        std::fclose(recon);
    if (!frames) {
        std::fprintf(stderr, "no frames read\n");
        return 1;
    }
    std::printf("last qp %d  ", enc.lastQp());
    std::printf("frames %d  bytes %zu (%.1f KB/frame)  %.2f fps  PSNR Y %.2f U %.2f V %.2f\n", frames, totalBytes,
                totalBytes / 1024.0 / frames, frames / seconds, psnrY / frames, psnrU / frames, psnrV / frames);
    return 0;
}
