// SPDX-License-Identifier: AGPL-3.0-only
// HDR -> SDR (engine/HdrTone): known values (black, HDR reference white of HLG and PQ alike, peak), a grey ramp that
// only rises, BT.2020 colours inside BT.709 with their hue, the per-pixel tables against the exact function, the card's
// 3D LUT (65 nodes, trilinear, as Gpu::Context::gradeLut bakes it) against the CPU, detection by the reader and the
// conversion of real 10-bit HLG/PQ files (and the same file untagged: unchanged), Clip::hdrToSdr in the project file.
#include "check.h"

#include "core/ProjectFile.h"
#include "core/Types.h"
#include "engine/HdrTone.h"
#include "engine/video/Convert.h"
#include "engine/video/Reader.h"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <vector>

namespace {

using HdrTone::Tone;
using HdrTone::Transfer;

std::array<float, 3> sdr(const Tone& t, float r, float g, float b)
{
    const float in[3] = {r, g, b};
    std::array<float, 3> out{};
    HdrTone::toSdr(t, in, out.data());
    return out;
}

void testDetection()
{
    CHECK(HdrTone::ofColorTrc(18) == Transfer::Hlg); // AVCOL_TRC_ARIB_STD_B67
    CHECK(HdrTone::ofColorTrc(16) == Transfer::Pq);  // AVCOL_TRC_SMPTE2084
    CHECK(HdrTone::ofColorTrc(1) == Transfer::None); // BT.709
    CHECK(HdrTone::ofColorTrc(2) == Transfer::None); // unspecified
    CHECK(QByteArray(HdrTone::name(Transfer::Hlg)) == "HLG");
    CHECK(QByteArray(HdrTone::name(Transfer::None)).isEmpty());
}

void testKnownValues()
{
    const Tone hlg{Transfer::Hlg, true}, pq{Transfer::Pq, true};
    for (const Tone& t : {hlg, pq}) {
        const auto black = sdr(t, 0, 0, 0);
        CHECK(black[0] < 1e-4f && black[1] < 1e-4f && black[2] < 1e-4f);
        const auto peak = sdr(t, 1, 1, 1);
        CHECK(peak[0] > 0.995f && peak[1] > 0.995f);
    }
    // HDR reference white (BT.2408): HLG 75 %, PQ 203 nits (code 0.5806) - the same SDR grey, bright but below white
    const auto wh = sdr(hlg, 0.75f, 0.75f, 0.75f), wp = sdr(pq, 0.5806f, 0.5806f, 0.5806f);
    CHECK(wh[1] > 0.85f && wh[1] < 0.96f);
    CHECK(std::abs(wh[1] - wp[1]) < 0.01f);
    // Grey stays grey
    CHECK(std::abs(wh[0] - wh[1]) < 1e-4f && std::abs(wh[2] - wh[1]) < 1e-4f);
    // Off: the signal as it is
    const auto off = sdr(Tone{}, 0.3f, 0.5f, 0.7f);
    CHECK(off[0] == 0.3f && off[1] == 0.5f && off[2] == 0.7f);
}

void testRampRises()
{
    for (const Tone& t : {Tone{Transfer::Hlg, true}, Tone{Transfer::Pq, true}, Tone{Transfer::Hlg, false}}) {
        float last = -1;
        int falls = 0;
        for (int i = 0; i <= 1023; ++i) {
            const float v = i / 1023.0f;
            const float o = sdr(t, v, v, v)[1];
            if (o < last - 1e-6f) ++falls;
            last = o;
        }
        CHECK_EQ(falls, 0);
    }
}

void testGamut()
{
    const Tone t{Transfer::Hlg, true};
    // Pure BT.2020 primaries land inside 0..1 and keep their hue (their channel the largest)
    for (int c = 0; c < 3; ++c) {
        float in[3] = {0, 0, 0};
        in[c] = 0.8f;
        float out[3];
        HdrTone::toSdr(t, in, out);
        for (float v : out) CHECK(v >= 0 && v <= 1);
        CHECK(out[c] >= out[(c + 1) % 3] && out[c] >= out[(c + 2) % 3]);
        CHECK(out[c] > 0.3f);
    }
}

// The tables (16-bit input) against the exact function: at most one 8-bit step apart
void testTables()
{
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> d(0, 65535);
    for (const Tone& t : {Tone{Transfer::Hlg, true}, Tone{Transfer::Pq, true}}) {
        const int n = 4096;
        std::vector<uint16_t> in(size_t(n) * 4);
        for (int i = 0; i < n; ++i) {
            for (int c = 0; c < 3; ++c) in[size_t(i) * 4 + c] = uint16_t(i < 256 ? i * 257 : d(rng)); // greys first
            if (i < 256) in[size_t(i) * 4 + 1] = in[size_t(i) * 4 + 2] = in[size_t(i) * 4];
            in[size_t(i) * 4 + 3] = 65535;
        }
        std::vector<uint8_t> out(size_t(n) * 4);
        HdrTone::toSdrRgba64(t, in.data(), n * 8, out.data(), n * 4, n, 1);
        int worst = 0;
        for (int i = 0; i < n; ++i) {
            const auto e = sdr(t, in[size_t(i) * 4] / 65535.0f, in[size_t(i) * 4 + 1] / 65535.0f, in[size_t(i) * 4 + 2] / 65535.0f);
            for (int c = 0; c < 3; ++c)
                worst = std::max(worst, std::abs(int(out[size_t(i) * 4 + c]) - int(std::lround(e[c] * 255))));
            CHECK_EQ(int(out[size_t(i) * 4 + 3]), 255);
        }
        std::printf("tables vs exact (%s): %d\n", HdrTone::name(t.transfer), worst);
        CHECK(worst <= 1);
    }
}

// The card samples the mapping from a 3D LUT of kToneLutSize (65) nodes holding 8-bit results, trilinearly: close to
// the CPU's per-pixel result (preview on the card = export on the CPU)
void testLutAgainstCpu()
{
    const int n = 65;
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> d(0, 1);
    for (const Tone& t : {Tone{Transfer::Hlg, true}, Tone{Transfer::Pq, true}}) {
        std::vector<uint8_t> lattice(size_t(n) * n * n * 3);
        for (int b = 0; b < n; ++b)
            for (int g = 0; g < n; ++g)
                for (int r = 0; r < n; ++r) {
                    const auto o = sdr(t, float(r) / (n - 1), float(g) / (n - 1), float(b) / (n - 1));
                    for (int c = 0; c < 3; ++c)
                        lattice[((size_t(b) * n + g) * n + r) * 3 + c] = uint8_t(std::lround(std::clamp(o[c], 0.0f, 1.0f) * 255));
                }
        auto node = [&](int r, int g, int b, int c) { return float(lattice[((size_t(b) * n + g) * n + r) * 3 + c]); };
        int worst = 0;
        double sum = 0;
        const int samples = 20000;
        for (int i = 0; i < samples; ++i) {
            const float v[3] = {d(rng), d(rng), d(rng)};
            const auto e = sdr(t, v[0], v[1], v[2]);
            int i0[3];
            float f[3];
            for (int c = 0; c < 3; ++c) {
                const float p = v[c] * (n - 1);
                i0[c] = std::min(int(p), n - 2);
                f[c] = p - i0[c];
            }
            for (int c = 0; c < 3; ++c) {
                float acc = 0;
                for (int k = 0; k < 8; ++k) {
                    const int dx = k & 1, dy = (k >> 1) & 1, dz = k >> 2;
                    const float w = (dx ? f[0] : 1 - f[0]) * (dy ? f[1] : 1 - f[1]) * (dz ? f[2] : 1 - f[2]);
                    acc += w * node(i0[0] + dx, i0[1] + dy, i0[2] + dz, c);
                }
                const int diff = std::abs(int(std::lround(acc)) - int(std::lround(e[c] * 255)));
                worst = std::max(worst, diff);
                sum += diff;
            }
        }
        std::printf("LUT vs CPU (%s): worst %d, mean %.3f\n", HdrTone::name(t.transfer), worst, sum / (samples * 3));
        CHECK(sum / (samples * 3) < 0.6);
        CHECK(worst <= 8); // saturated corners at the gamut clip, where the mapping bends hardest
    }
}

// A flat colour as a 10-bit file with the given transfer tags; the converted centre pixel
std::array<int, 3> convertedCentre(const QString& path)
{
    const auto r = Video::Reader::open(path);
    if (!CHECK(r)) return {-1, -1, -1};
    const auto pic = r->picture(0);
    if (!CHECK(pic && pic->frame)) return {-1, -1, -1};
    const int w = pic->frame->width, h = pic->frame->height;
    std::vector<uint8_t> out(size_t(w) * h * 4);
    if (!CHECK(Video::toRgba(*pic, r->info(), out.data(), w, h, false))) return {-1, -1, -1};
    const uint8_t* p = out.data() + (size_t(h / 2) * w + w / 2) * 4;
    // scaled preview conversion too: same picture
    std::vector<uint8_t> small(size_t(w / 2) * (h / 2) * 4);
    CHECK(Video::toRgba(*pic, r->info(), small.data(), w / 2, h / 2, true));
    const uint8_t* q = small.data() + (size_t(h / 4) * (w / 2) + w / 4) * 4;
    for (int c = 0; c < 3; ++c) CHECK(std::abs(int(p[c]) - int(q[c])) <= 2);
    return {p[0], p[1], p[2]};
}

void testFiles(const QString& dir)
{
    const auto make = [&](const QString& name, const QString& color, const QStringList& tags) {
        return Check::makeMedia(dir + "/" + name,
                                QStringList{"-f", "lavfi", "-i", "color=c=" + color + ":size=128x72:rate=25:duration=0.2",
                                            "-vf", "format=yuv420p10le", "-c:v", "ffv1", "-colorspace", "bt2020nc"}
                                    + tags);
    };
    const QStringList hlgTags{"-color_primaries", "bt2020", "-color_trc", "arib-std-b67"};
    const QStringList pqTags{"-color_primaries", "bt2020", "-color_trc", "smpte2084"};
    const QStringList sdrTags{"-color_primaries", "bt2020", "-color_trc", "bt2020-10"};
    // 0xbf = 75 %: HLG reference white
    const QString hlg = make("hlg.mkv", "0xbfbfbf", hlgTags), plain = make("plain.mkv", "0xbfbfbf", sdrTags);
    const QString pq = make("pq.mkv", "0x949494", pqTags); // 0x94 = 58 %: PQ 203 nits
    const QString red = make("hlg-red.mkv", "0xc00000", hlgTags);
    if (!CHECK(!hlg.isEmpty() && !plain.isEmpty() && !pq.isEmpty() && !red.isEmpty())) return;
    {
        const auto r = Video::Reader::open(hlg);
        if (CHECK(r)) {
            CHECK(r->info().tone.transfer == Transfer::Hlg);
            CHECK(r->info().tone.bt2020);
        }
        const auto p = Video::Reader::open(plain);
        if (CHECK(p)) CHECK(!p->info().tone.active());
        const auto q = Video::Reader::open(pq);
        if (CHECK(q)) CHECK(q->info().tone.transfer == Transfer::Pq);
    }
    const auto h = convertedCentre(hlg), s = convertedCentre(plain), q = convertedCentre(pq), rd = convertedCentre(red);
    std::printf("HLG 75%%: %d %d %d, untagged: %d %d %d, PQ 203: %d %d %d, HLG red: %d %d %d\n", h[0], h[1], h[2], s[0],
                s[1], s[2], q[0], q[1], q[2], rd[0], rd[1], rd[2]);
    CHECK(std::abs(s[1] - 191) <= 2);        // SDR: the signal as it is
    CHECK(h[1] > 215 && h[1] < 245);         // reference white: bright, not clipped
    CHECK(std::abs(h[1] - q[1]) <= 4);       // HLG and PQ reference white alike
    CHECK(std::abs(h[0] - h[2]) <= 2);       // grey stays grey
    CHECK(rd[0] > 150 && rd[1] < 40 && rd[2] < 40); // red stays red
    // Cleared tone (Clip::hdrToSdr off): the signal as it is
    const auto r = Video::Reader::open(hlg);
    if (CHECK(r)) {
        Video::Info info = r->info();
        info.tone = {};
        const auto pic = r->picture(0);
        std::vector<uint8_t> out(size_t(128) * 72 * 4);
        if (CHECK(pic && Video::toRgba(*pic, info, out.data(), 128, 72, false)))
            CHECK(std::abs(int(out[(36 * 128 + 64) * 4 + 1]) - 191) <= 2);
    }
}

void testProjectFile()
{
    ProjectData d;
    Clip c;
    c.id = 1;
    c.kind = ClipKind::Media;
    c.mediaPath = "/fixtures/media/a.mov";
    c.length = 25;
    Clip off = c;
    off.id = 2;
    off.start = 30;
    off.hdrToSdr = false;
    d.timeline.video.resize(1);
    d.timeline.video[0].clips << c << off;
    CHECK(c.hdrToSdr);
    const QByteArray json = ProjectFile::toJson(d, "/fixtures/p.schneidi");
    CHECK(!ProjectFile::clipJson(c).contains("hdrToSdr")); // default not written
    CHECK(ProjectFile::clipJson(off).value("hdrToSdr") == false);
    ProjectData back;
    QString error;
    if (CHECK(ProjectFile::fromJson(json, "/fixtures/p.schneidi", &back, &error)) && CHECK(back.timeline.video.size() == 1)
        && CHECK(back.timeline.video[0].clips.size() == 2)) {
        CHECK(back.timeline.video[0].clips[0].hdrToSdr);
        CHECK(!back.timeline.video[0].clips[1].hdrToSdr);
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testDetection();
    testKnownValues();
    testRampRises();
    testGamut();
    testTables();
    testLutAgainstCpu();
    testProjectFile();
    if (Check::haveFfmpeg()) {
        QTemporaryDir dir;
        testFiles(dir.path());
    }
    return Check::result();
}
