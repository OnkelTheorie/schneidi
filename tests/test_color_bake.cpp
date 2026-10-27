// Baked Color page correction (ColorGrade::bake/apply) against the per-pixel reference (applyReference): test
// pictures (a grid through all colors, a testsrc2 frame, skin tones, smooth gradients) x corrections (wheels,
// strong/negative lift, gain, gamma, saturation 0 and 100, temperature/tint/exposure, contrast) x LUTs (none, 1D,
// 3D 17/33/65, with pre-curve and domain, with values outside 0..1). At most 1 code value apart, no banding on
// gradients. Prints max/mean deviation per picture and LUT.
#include "check.h"

#include "engine/ColorGrade.h"

#include <QCoreApplication>
#include <QImage>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

namespace {

using CP = ColorGrade::Params;
using Fn = std::function<void(float, float, float, float*)>;

struct Picture {
    const char* name;
    int w = 0, h = 0;
    std::vector<uint8_t> px;
};

Picture fromColors(const char* name, const std::vector<std::array<int, 3>>& colors, int w)
{
    Picture p{name, w, int((colors.size() + w - 1) / w), {}};
    p.px.assign(size_t(p.w) * p.h * 4, 255);
    for (size_t i = 0; i < colors.size(); ++i)
        for (int c = 0; c < 3; ++c) p.px[i * 4 + c] = uint8_t(colors[i][c]);
    return p;
}

std::vector<Picture> pictures()
{
    std::vector<Picture> out;
    std::vector<std::array<int, 3>> grid;
    for (int r = 0; r < 256; r += 5)
        for (int g = 0; g < 256; g += 5)
            for (int b = 0; b < 256; b += 5) grid.push_back({r, g, b});
    for (int i = 0; i < 256; ++i) // grays and pure ramps
        for (const auto& c : {std::array<int, 3>{i, i, i}, {i, 0, 0}, {0, i, 0}, {0, 0, i}, {255, i, 255 - i}})
            grid.push_back(c);
    out.push_back(fromColors("color grid", grid, 1024));
    std::vector<std::array<int, 3>> skin;
    for (int r = 140; r < 256; r += 3)
        for (int g = 90; g < 211; g += 3)
            for (int b = 70; b < 181; b += 5)
                if (r > g && g > b) skin.push_back({r, g, b});
    out.push_back(fromColors("skin tones", skin, 512));
    // Smooth gradients, one per row (checked for banding below)
    Picture grad{"gradients", 1024, 6, {}};
    grad.px.assign(size_t(grad.w) * grad.h * 4, 255);
    for (int x = 0; x < grad.w; ++x) {
        const int v = x / 4, f = x * 255 / 1023;
        const int rows[6][3] = {{v, v, v}, {f, f / 2 + 40, 200 - f / 2}, {255, f, f / 3}, {f / 4, f / 4 + 10, f / 3},
                                {200 + f / 5, 150 + f / 4, 120 + f / 4}, {255 - v, 255 - v, 255}};
        for (int y = 0; y < grad.h; ++y)
            for (int c = 0; c < 3; ++c) grad.px[(size_t(y) * grad.w + x) * 4 + c] = uint8_t(rows[y][c]);
    }
    out.push_back(grad);
    QTemporaryDir dir;
    const QString png =
        Check::makeMedia(dir.filePath("f.png"), {"-f", "lavfi", "-i", "testsrc2=size=640x360", "-frames:v", "1"});
    QImage img(png);
    CHECK(!img.isNull());
    if (!img.isNull()) {
        img = img.convertToFormat(QImage::Format_RGBA8888);
        out.push_back({"testsrc2", img.width(), img.height(),
                       std::vector<uint8_t>(img.constBits(), img.constBits() + img.sizeInBytes())});
    }
    return out;
}

std::shared_ptr<ColorGrade::Lut> makeLut(int n, bool is3d, const Fn& fn)
{
    auto lut = std::make_shared<ColorGrade::Lut>();
    lut->size = n;
    lut->is3d = is3d;
    float o[3];
    if (is3d) {
        for (int b = 0; b < n; ++b)
            for (int g = 0; g < n; ++g)
                for (int r = 0; r < n; ++r) {
                    fn(r / float(n - 1), g / float(n - 1), b / float(n - 1), o);
                    lut->data << o[0] << o[1] << o[2];
                }
    } else {
        for (int i = 0; i < n; ++i) {
            fn(i / float(n - 1), i / float(n - 1), i / float(n - 1), o);
            lut->data << o[0] << o[1] << o[2];
        }
    }
    ColorGrade::prepare(*lut);
    return lut;
}

void look(float r, float g, float b, float* o)
{
    auto s = [](float x) { return x + 0.12f * std::sin(6.2831853f * x) * (1 - x); };
    const float y = 0.3f * r + 0.6f * g + 0.1f * b;
    o[0] = s(0.85f * r + 0.15f * y) + 0.04f * std::sin(6 * b);
    o[1] = s(0.9f * g + 0.1f * y);
    o[2] = s(0.8f * b + 0.2f * y) - 0.03f * std::cos(5 * r);
}
// Strong: crushes and lifts, values outside 0..1 (clamped only after the interpolation)
void wild(float r, float g, float b, float* o)
{
    o[0] = 1.4f * r - 0.3f * g + 0.1f - 0.2f * b;
    o[1] = std::pow(g, 2.2f) * 1.3f - 0.1f;
    o[2] = 0.5f + (b - 0.5f) * 1.8f + 0.2f * (r - g);
}
void curves(float r, float g, float b, float* o)
{
    o[0] = std::pow(r, 0.6f);
    o[1] = g * g * (3 - 2 * g);
    o[2] = std::sqrt(b) * 0.9f + 0.05f;
}

std::vector<std::pair<const char*, CP>> corrections()
{
    std::vector<std::pair<const char*, CP>> out;
    out.push_back({"neutral", CP()});
    CP p;
    p.lift[0] = 0.05, p.gamma[0] = 0.1, p.gamma[3] = -0.05, p.gain[0] = 1.15, p.gain[1] = 1.05, p.contrast = 1.2;
    p.temperature = 600;
    out.push_back({"wheels", p});
    p = CP();
    p.lift[0] = 0.3, p.lift[2] = 0.1, p.gain[0] = 1.8, p.gamma[0] = 0.4;
    out.push_back({"strong lift/gain/gamma", p});
    p = CP();
    p.lift[0] = -0.3, p.lift[1] = -0.1, p.gain[0] = 0.8, p.gamma[0] = -0.5, p.offset[3] = 30;
    out.push_back({"negative lift", p});
    p = CP();
    p.saturation = 0;
    out.push_back({"saturation 0", p});
    p.saturation = 100, p.lift[0] = 0.02, p.gain[2] = 1.2;
    out.push_back({"saturation 100", p});
    p = CP();
    p.temperature = -2500, p.tint = 40, p.exposure = 1.5, p.saturation = 65;
    out.push_back({"temp/tint/exposure", p});
    p = CP();
    p.contrast = 2.0, p.pivot = 0.3, p.saturation = 35, p.offset[0] = 20;
    out.push_back({"contrast 2", p});
    return out;
}

struct Stats {
    int max = 0;
    double sum = 0;
    size_t n = 0, over1 = 0;
};

void compare(const std::vector<uint8_t>& want, const std::vector<uint8_t>& got, Stats& s)
{
    for (size_t i = 0; i < want.size(); ++i) {
        if (i % 4 == 3) {
            if (want[i] != got[i]) s.max = 999; // alpha must stay
            continue;
        }
        const int d = std::abs(int(want[i]) - int(got[i]));
        s.max = std::max(s.max, d);
        s.sum += d;
        s.over1 += d > 1;
        ++s.n;
    }
}

// Gradient picture without banding: on no row the baked picture jumps further from one pixel to the next than
// the reference does anywhere on that row (+1 for rounding), and the gray ramp (first row) rises wherever the
// reference rises. (Single pixels may round the other way where the exact value is near .5.)
bool smooth(const Picture& pic, const std::vector<uint8_t>& want, const std::vector<uint8_t>& got)
{
    for (int y = 0; y < pic.h; ++y)
        for (int c = 0; c < 3; ++c) {
            bool up = true, gotUp = true;
            int jumpWant = 0, jumpGot = 0;
            for (int x = 1; x < pic.w; ++x) {
                const size_t i = (size_t(y) * pic.w + x) * 4 + c, j = i - 4;
                const int dw = int(want[i]) - want[j], dg = int(got[i]) - got[j];
                jumpWant = std::max(jumpWant, std::abs(dw));
                jumpGot = std::max(jumpGot, std::abs(dg));
                up &= dw >= 0, gotUp &= dg >= 0;
            }
            if (jumpGot > jumpWant + 1 || (y == 0 && up && !gotUp)) {
                std::printf("  row %d channel %d: largest step %d instead of %d, rising %d/%d\n", y, c, jumpGot,
                            jumpWant, up, gotUp);
                return false;
            }
        }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const std::vector<Picture> pics = pictures();
    auto lut1d = makeLut(1024, false, curves);
    auto lut17 = makeLut(17, true, look);
    auto lut33 = makeLut(33, true, look);
    auto lut65 = makeLut(65, true, look);
    auto wild33 = makeLut(33, true, wild);
    // Pre-curve and a wider domain (like .csp / DOMAIN_MIN/MAX)
    auto shaped = std::make_shared<ColorGrade::Lut>(*makeLut(17, true, look));
    for (int c = 0; c < 3; ++c) {
        shaped->shaperIn[c] = {0.0f, 0.25f, 1.0f};
        shaped->shaperOut[c] = {0.0f, 0.5f, 1.1f};
        shaped->domainMin[c] = -0.05f;
        shaped->domainMax[c] = 1.15f;
    }
    ColorGrade::prepare(*shaped);
    const std::pair<const char*, const ColorGrade::Lut*> luts[] = {
        {"no LUT", nullptr},          {"1D 1024", lut1d.get()},  {"3D 17", lut17.get()},
        {"3D 33", lut33.get()},       {"3D 65", lut65.get()},    {"3D 33 outside 0..1", wild33.get()},
        {"3D 17 pre-curve+domain", shaped.get()},
    };
    const auto corr = corrections();
    std::printf("%-12s %-24s %4s %8s %10s\n", "picture", "LUT", "max", "mean", "share > 1");
    for (const Picture& pic : pics)
        for (const auto& [lname, lut] : luts) {
            Stats s;
            for (const auto& [cname, p] : corr) {
                std::vector<uint8_t> want = pic.px, got = pic.px;
                ColorGrade::applyReference(want.data(), pic.w, pic.h, p, lut);
                ColorGrade::apply(got.data(), pic.w, pic.h, p, lut);
                Stats one;
                compare(want, got, one);
                if (one.max > 1) std::printf("  %s / %s / %s: max %d\n", pic.name, lname, cname, one.max);
                CHECK(one.max <= 1);
                if (!lut) CHECK(one.max == 0); // the same tables as before
                if (std::strcmp(pic.name, "gradients") == 0) CHECK(smooth(pic, want, got));
                s.max = std::max(s.max, one.max);
                s.sum += one.sum, s.n += one.n, s.over1 += one.over1;
            }
            std::printf("%-12s %-24s %4d %8.4f %10.6f\n", pic.name, lname, s.max, s.sum / double(s.n),
                        double(s.over1) / double(s.n));
        }

    // The same tables again give the same picture; a hand-made LUT without prepare() gives the prepared result
    {
        const Picture& pic = pics.front();
        CP p = corr[1].second;
        p.saturation = 70;
        auto b = ColorGrade::bake(p, lut33.get());
        std::vector<uint8_t> a = pic.px, c = pic.px, d = pic.px, e = pic.px;
        ColorGrade::apply(a.data(), pic.w, pic.h, *b, 1);
        ColorGrade::apply(c.data(), pic.w, pic.h, *b, 3);
        ColorGrade::Lut plain = *lut33;
        plain.tables.reset();
        ColorGrade::apply(d.data(), pic.w, pic.h, p, &plain);
        CHECK(a == c && a == d);
        p.lift[0] += 0.01; // other values -> other tables
        ColorGrade::apply(e.data(), pic.w, pic.h, *ColorGrade::bake(p, lut33.get()));
        CHECK(a != e);
    }
    // A 3D LUT too big for packed nodes is computed per pixel: exactly the reference
    {
        auto big = makeLut(130, true, look);
        const Picture& pic = pics[1];
        const CP p = corr[1].second;
        std::vector<uint8_t> want = pic.px, got = pic.px;
        ColorGrade::applyReference(want.data(), pic.w, pic.h, p, big.get());
        ColorGrade::apply(got.data(), pic.w, pic.h, p, big.get());
        CHECK(want == got);
    }
    return Check::result();
}
