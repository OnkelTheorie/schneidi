// SPDX-License-Identifier: AGPL-3.0-only
// Custom effect "shader" (0.19) on the graphics card: compiled at run time (qsb), drawn through Gpu::Service.
// Inverts like the CPU would, reads p1..p8, colours, time, keeps up = row 0 and the size. Skips (77) without a
// hardware GPU (SCHNEIDI_GPU=1: a software device too) or without qsb.
#include "engine/picture/CustomEffects.h"
#include "engine/picture/gpu/Gpu.h"

#include "check.h"

#include <QElapsedTimer>
#include <QGuiApplication>

#include <vector>

namespace PC = Picture::Custom;

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("custom_shader");
    QString why;
    if (!PC::shaderCompilerAvailable(&why)) return Check::skip(qPrintable(why));
    Picture::Gpu::Service::prepare();
    if (!Picture::Gpu::Service::instance())
        return Check::skip(qPrintable("no GPU (" + Picture::Gpu::probe().error + ")"));
    std::printf("GPU: %s on %s\n", qPrintable(Picture::Gpu::probe().api), qPrintable(Picture::Gpu::probe().device));

    const int w = 96, h = 54;
    std::vector<uint8_t> src(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &src[(size_t(y) * w + x) * 4];
            p[0] = uint8_t(x * 2), p[1] = uint8_t(y * 4), p[2] = uint8_t(200 - x), p[3] = 255;
        }
    auto draw = [&](const char* code, const PC::ShaderValues& v, std::vector<uint8_t>* px) {
        *px = src;
        QString err;
        const bool ok = PC::applyShader(QString::fromUtf8(code), v, px->data(), w, h, &err);
        if (!ok) std::printf("       %s\n", qPrintable(err));
        return ok;
    };
    PC::ShaderValues v;
    std::vector<uint8_t> px;
    // Invert: 255 - value on every pixel (texelFetch at the pixel centre: no filtering)
    if (CHECK(draw("vec4 effect(vec2 uv) { vec4 c = pixel(uv); return vec4(1.0 - c.rgb, c.a); }", v, &px))) {
        int wrong = 0;
        for (size_t i = 0; i < px.size(); i += 4)
            for (int c = 0; c < 3; ++c)
                if (std::abs(int(px[i + c]) - (255 - src[i + c])) > 1) ++wrong;
        CHECK_EQ(wrong, 0);
        CHECK(px != src);
    }
    // Values: p1 mixes color1 in; up is row 0 (uv.y < 0.5 = top half)
    v.p[0] = 1.0;
    v.color1 = QColor(255, 0, 0);
    v.color2 = QColor(0, 0, 255);
    if (CHECK(draw("vec4 effect(vec2 uv) { return uv.y < 0.5 ? mix(pixel(uv), color1, p1) : color2; }", v, &px))) {
        CHECK(px[0] == 255 && px[1] == 0 && px[2] == 0);
        const size_t last = (size_t(h - 1) * w) * 4;
        CHECK(px[last] == 0 && px[last + 1] == 0 && px[last + 2] == 255);
    }
    // Time and p8
    v.seconds = 5;
    v.p[7] = 0.5;
    if (CHECK(draw("vec4 effect(vec2 uv) { return vec4(vec3(time / 10.0), p8); }", v, &px)))
        CHECK(std::abs(int(px[0]) - 128) <= 1 && std::abs(int(px[3]) - 128) <= 1);
    // A broken shader: not drawn, the picture stays, the reason says why
    {
        px = src;
        QString err;
        CHECK(!PC::applyShader("vec4 effect(vec2 uv) { return nope; }", v, px.data(), w, h, &err));
        CHECK(err.contains("nope"));
        CHECK(px == src);
    }
    // Rough cost at 1920x1080 (upload, pass, readback)
    {
        const int W = 1920, H = 1080;
        std::vector<uint8_t> big(size_t(W) * H * 4, 128);
        const char* code = "vec4 effect(vec2 uv) { return pixel(uv + vec2(sin(uv.y * 40.0 + time) * 0.01, 0.0)); }";
        PC::applyShader(code, v, big.data(), W, H);
        QElapsedTimer t;
        t.start();
        for (int i = 0; i < 10; ++i) PC::applyShader(code, v, big.data(), W, H);
        std::printf("shader 1920x1080: %.1f ms per frame\n", t.nsecsElapsed() / 1e7);
    }
    Picture::Gpu::Service::shutdown();
    return Check::result();
}
