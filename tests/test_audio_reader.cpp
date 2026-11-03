// SPDX-License-Identifier: AGPL-3.0-only
// Own audio engine, step 1 (docs/archiv/plan-0.10-ton.md): Audio::Reader reads one audio stream sample-exact.
// Per source (containers, codecs, late streams, rates, channel layouts):
// - content: the samples equal FFmpeg's own decode of the stream (bit-exact for every codec: same decoder), placed
//   where the container says the stream starts (sound later than the picture: positive, picture later: negative)
// - placement: beeps written at known times relative to the picture start land there (onset in samples)
// - random access: reads at random places, backwards and in odd sizes return the same samples as one sequential read
//   (bit-exact for PCM/FLAC; lossy codecs within a small threshold, their decoders converge after a jump)

#include "check.h"

#include "engine/audio/Reader.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QRandomGenerator>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr int kRate = 48000;
constexpr int kFps = 25;
constexpr double kSeconds = 6;
// Beep times relative to the picture start (= reader position 0)
constexpr double kBeeps[] = {1.0, 2.32, 3.68, 5.24};
constexpr double kStream2Shift = 0.4; // second stream of "streams": its beeps come this much later

struct Case {
    const char* name;
    const char* ext;
    const char* codec;
    bool video = true;
    double audioDelay = 0; // the sound stream starts this much after the picture
    double videoDelay = 0; // the picture starts this much after the sound
    int rate = kRate;
    int channels = 2;
    bool lossless = true;  // random access bit-exact
    double placeTol = 2;   // beep onset tolerance in samples
    int streams = 1;
    bool cover = false;
    // AAC noise substitution (PNS, on in FFmpeg's encoder by default): the decoder fills noise bands from its random
    // generator, so after a jump the noise has other values (same level). Bit-exact jumps are impossible there.
    bool pns = false;
    // Where the first sample may be off from (audio delay - video delay): FFmpeg's MP4 muxer stores a late AAC
    // stream from 0.278 s without its priming information (1024 + 32 samples early), as the file says
    int startTol = 0;
};

// Matroska stores whole milliseconds: placement within 1 ms (+ onset detection)
constexpr double kMkvTol = 48 + 4;
// Lossy codecs smear the beep onset by a few samples
constexpr double kLossyTol = 12;

const Case kCases[] = {
    {"wav", "wav", "pcm_s16le", false},
    {"flac", "flac", "flac", false},
    {"mov", "mov", "pcm_s16le"},
    {"mov_late_audio", "mov", "pcm_s16le", true, 0.3, 0, kRate, 2, true},
    {"mov_late_video", "mov", "pcm_s16le", true, 0, 0.2},
    {"mp4_aac", "mp4", "aac", true, 0, 0, kRate, 2, false, kLossyTol},
    {"mp4_aac_late_audio", "mp4", "aac", true, 0.3, 0, kRate, 2, false, 33, 1, false, false, 1056},
    {"m4a", "m4a", "aac", false, 0, 0, kRate, 2, false, kLossyTol},
    {"m4a_pns", "m4a", "aac", false, 0, 0, kRate, 2, false, kLossyTol, 1, false, true},
    {"mkv_pcm", "mkv", "pcm_s16le", true, 0, 0, kRate, 2, true, kMkvTol},
    {"mkv_aac", "mkv", "aac", true, 0, 0, kRate, 2, false, kMkvTol},
    {"mkv_aac_late_audio", "mkv", "aac", true, 0.3, 0, kRate, 2, false, kMkvTol},
    {"opus", "opus", "libopus", false, 0, 0, kRate, 2, false, kLossyTol},
    {"mp3", "mp3", "libmp3lame", false, 0, 0, kRate, 2, false, kLossyTol},
    {"mp3_cover", "mp3", "libmp3lame", false, 0, 0, kRate, 2, false, kLossyTol, 1, true},
    {"wav44", "wav", "pcm_s16le", false, 0, 0, 44100, 2, true, 3},
    {"mono", "wav", "pcm_s16le", false, 0, 0, kRate, 1},
    {"surround", "wav", "pcm_s16le", false, 0, 0, kRate, 6},
    {"streams", "mov", "pcm_s16le", true, 0, 0, kRate, 2, true, 2, 2},
};

// Quiet noise plus 1 kHz beeps (20 ms, starting at phase 0) at `beeps` (picture time); `delay` = stream start
QString signal(double delay, double shift, int channel)
{
    QString any;
    for (double b : kBeeps) {
        const double t = b + shift - delay;
        any += QString("%1between(t\\,%2\\,%3)").arg(any.isEmpty() ? "" : "+").arg(t, 0, 'f', 5).arg(t + 0.02, 0, 'f', 5);
    }
    return QString("0.02*(2*random(%1)-1)+if(%2\\,0.5*sin(2*PI*1000*t)\\,0)").arg(channel).arg(any);
}

QString audioSource(const Case& c, double shift)
{
    QStringList ch;
    for (int i = 0; i < c.channels; ++i) ch << signal(c.audioDelay, shift, i);
    return QString("aevalsrc=%1:s=%2:d=%3").arg(ch.join('|')).arg(c.rate).arg(kSeconds - c.audioDelay);
}

QString makeSource(const QString& dir, const Case& c)
{
    QStringList a;
    int inputs = 0;
    if (c.video) {
        if (c.videoDelay > 0) a << "-itsoffset" << QString::number(c.videoDelay);
        a << "-f" << "lavfi" << "-i" << QString("color=c=black:s=160x90:r=25:d=%1").arg(kSeconds - c.videoDelay);
        ++inputs;
    }
    const int firstAudio = inputs;
    for (int s = 0; s < c.streams; ++s) {
        if (c.audioDelay > 0) a << "-itsoffset" << QString::number(c.audioDelay);
        a << "-f" << "lavfi" << "-i" << audioSource(c, s * kStream2Shift);
        ++inputs;
    }
    // Cover picture: a one-frame source ("-frames:v 1" makes FFmpeg end the whole file after 48 ms)
    if (c.cover) a << "-f" << "lavfi" << "-i" << "color=c=red:s=64x64:r=1:d=1";
    if (c.video) a << "-map" << "0:v";
    for (int s = 0; s < c.streams; ++s) a << "-map" << QString("%1:a").arg(firstAudio + s);
    if (c.cover) a << "-map" << QString("%1:v").arg(inputs) << "-c:v" << "mjpeg" << "-disposition:v" << "attached_pic"
                   << "-id3v2_version" << "3";
    // The picture's own time stamps (a late start stays late): FFmpeg 6.1 makes MOV/MP4 constant frame rate by
    // default and fills a late start with copies of the first picture, the file then starts with both together
    if (c.video)
        a << "-c:v" << "libx264" << "-preset" << "ultrafast" << "-pix_fmt" << "yuv420p" << "-g" << "25" << "-fps_mode"
          << "passthrough";
    a << "-c:a" << c.codec;
    if (QString(c.codec) == "aac" || QString(c.codec) == "libmp3lame") a << "-b:a" << "192k";
    if (QString(c.codec) == "aac" && !c.pns) a << "-aac_pns" << "0";
    return Check::makeMedia(QString("%1/%2.%3").arg(dir, c.name, c.ext), a);
}

using Planar = std::vector<std::vector<float>>;

// FFmpeg's own decode of audio stream `stream`, from its first sample, at `rate` (0 = native)
Planar ffmpegDecode(const QString& path, int stream, int channels, int rate = 0)
{
    QProcess p;
    QStringList args{"-v", "error", "-nostdin", "-i", path, "-map", QString("0:a:%1").arg(stream)};
    if (rate > 0) args << "-ar" << QString::number(rate);
    args << "-f" << "f32le" << "-c:a" << "pcm_f32le" << "-";
    p.start("ffmpeg", args);
    p.waitForFinished(60000);
    const QByteArray d = p.readAllStandardOutput();
    const auto* f = reinterpret_cast<const float*>(d.constData());
    const size_t n = size_t(d.size()) / sizeof(float) / size_t(channels);
    Planar out(static_cast<size_t>(channels), std::vector<float>(n));
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < channels; ++c) out[size_t(c)][i] = f[i * size_t(channels) + size_t(c)];
    return out;
}

Planar readRange(Audio::Reader& r, int64_t from, int64_t to, int block)
{
    Planar out(size_t(r.channels()), std::vector<float>(size_t(to - from)));
    std::vector<float*> ptr(size_t(r.channels()));
    for (int64_t p = from; p < to; p += block) {
        const int n = int(std::min<int64_t>(block, to - p));
        for (int c = 0; c < r.channels(); ++c) ptr[size_t(c)] = out[size_t(c)].data() + (p - from);
        r.read(p, n, ptr.data());
    }
    return out;
}

// Largest difference of a[ia..] and b[ib..] over n samples, all channels
double maxDiff(const Planar& a, size_t ia, const Planar& b, size_t ib, size_t n)
{
    double m = 0;
    for (size_t c = 0; c < a.size() && c < b.size(); ++c)
        for (size_t i = 0; i < n && ia + i < a[c].size() && ib + i < b[c].size(); ++i)
            m = std::max(m, double(std::abs(a[c][ia + i] - b[c][ib + i])));
    return m;
}

// Beep onsets (first sample above 0.25 after 0.3 s of quiet) of channel 0
std::vector<int64_t> onsets(const std::vector<float>& s, int64_t origin)
{
    std::vector<int64_t> out;
    int64_t quiet = kRate;
    for (size_t i = 0; i < s.size(); ++i) {
        if (std::abs(s[i]) > 0.25f) {
            if (quiet >= kRate * 3 / 10) out.push_back(int64_t(i) + origin);
            quiet = 0;
        } else {
            ++quiet;
        }
    }
    return out;
}

void testCase(const QString& dir, const Case& c)
{
    std::printf("-- %s\n", c.name);
    const QString path = makeSource(dir, c);
    if (!CHECK(!path.isEmpty())) return;
    for (int s = 0; s < c.streams; ++s) {
        QElapsedTimer t;
        t.start();
        QString error;
        auto r = Audio::Reader::open(path, s, {}, &error);
        const double openMs = t.nsecsElapsed() / 1e6;
        if (!CHECK(r)) {
            std::printf("       %s\n", qPrintable(error));
            return;
        }
        std::printf("       stream %d opened in %.1f ms\n", s, openMs);
        const Audio::StreamInfo& info = r->info();
        CHECK_EQ(r->channels(), c.channels);
        CHECK_EQ(info.sourceRate, c.rate);
        CHECK_EQ(info.hasPicture, c.video);
        // Where the stream starts against the picture (PCM: exact; lossy: the codec's priming is not sound)
        const int64_t wantStart = std::llround((c.audioDelay - c.videoDelay) * kRate);
        if (!CHECK(std::abs(info.start - wantStart) <= c.startTol))
            std::printf("       start %lld, expected %lld\n", (long long)info.start, (long long)wantStart);

        // Content: FFmpeg's decode, from the stream's first sample on
        const int64_t from = std::min<int64_t>(0, info.start) - kRate / 2;
        const int64_t to = std::max<int64_t>(info.end, std::llround(kSeconds * kRate)) + kRate / 2;
        const Planar seq = readRange(*r, from, to, 1920);
        const Planar ref = ffmpegDecode(path, s, c.channels, c.rate != kRate ? kRate : 0);
        const size_t at = size_t(info.start - from);
        // FFmpeg keeps priming its demuxer does not mark (Matroska AAC: CodecDelay): the reader drops it
        size_t primed = 0;
        if (!c.lossless) {
            double best = 1e300;
            for (size_t lag = 0; lag <= 2112; ++lag) {
                double e = 0;
                for (size_t i = 20000; i < 24000; ++i) e += std::abs(seq[0][at + i] - ref[0][i + lag]);
                if (e < best) {
                    best = e;
                    primed = lag;
                }
            }
            if (primed) std::printf("       FFmpeg's decode has %zu priming samples\n", primed);
        }
        Planar refCut = ref;
        for (auto& ch : refCut) ch.erase(ch.begin(), ch.begin() + qsizetype(primed));
        // Resampled (44.1 kHz): FFmpeg's resampler starts without the silence before the first sample, 2 samples differ
        const size_t skipStart = c.rate != kRate ? 64 : 0;
        const double content = maxDiff(seq, at + skipStart, refCut, skipStart, refCut[0].size());
        const double contentTol = c.rate != kRate ? 1e-6 : 0;
        if (!CHECK(content <= contentTol)) {
            size_t worst = 0;
            for (size_t i = 0; i < refCut[0].size() && at + i < seq[0].size(); ++i)
                if (std::abs(seq[0][at + i] - refCut[0][i]) > std::abs(seq[0][at + worst] - refCut[0][worst])) worst = i;
            std::printf("       content differs from FFmpeg's decode by %.3g (sample %zu of %zu)\n", content, worst, refCut[0].size());
        }
        double silent = 0;
        for (const auto& ch : seq)
            for (size_t i = 0; i < at; ++i) silent = std::max(silent, double(std::abs(ch[i])));
        if (!CHECK(silent == 0)) std::printf("       %.3g before the stream start\n", silent);

        // Placement: the beeps where they were written (picture time)
        const std::vector<int64_t> got = onsets(seq[0], from);
        if (CHECK_EQ(int(got.size()), int(std::size(kBeeps)))) {
            double worst = 0;
            for (size_t i = 0; i < got.size(); ++i) {
                // the 1 kHz sine passes 0.25 + noise 4-5 samples after its start
                const double want = (kBeeps[i] + s * kStream2Shift - c.videoDelay) * kRate + 4.5 * kRate / c.rate;
                worst = std::max(worst, std::abs(double(got[i]) - want));
            }
            if (!CHECK(worst <= c.placeTol)) std::printf("       beep off by %.1f samples (%.2f ms)\n", worst, worst * 1000 / kRate);
            else std::printf("       beeps within %.1f samples\n", worst);
        }

        // Random access against the sequential read
        QRandomGenerator rng(4711 + uint(s));
        double worstJump = 0, worstLevel = 0;
        std::vector<float*> ptr(size_t(r->channels()));
        Planar block(size_t(r->channels()));
        auto readAt = [&](int64_t pos, int n) {
            for (int ch = 0; ch < r->channels(); ++ch) {
                block[size_t(ch)].assign(size_t(n), 7.f);
                ptr[size_t(ch)] = block[size_t(ch)].data();
            }
            r->read(pos, n, ptr.data());
            worstJump = std::max(worstJump, maxDiff(block, 0, seq, size_t(pos - from), size_t(n)));
            if (n >= 1000) { // level of the block against the sequential read (dB)
                double a = 0, b = 0;
                for (int k = 0; k < n; ++k) {
                    a += double(block[0][size_t(k)]) * block[0][size_t(k)];
                    b += double(seq[0][size_t(pos - from + k)]) * seq[0][size_t(pos - from + k)];
                }
                if (a > 1e-12 && b > 1e-12) worstLevel = std::max(worstLevel, std::abs(10 * std::log10(a / b)));
            }
        };
        for (int i = 0; i < 80; ++i) {
            const int n = 1 + int(rng.bounded(6000));
            const int64_t pos = from + int64_t(rng.bounded(quint32(to - from - n)));
            readAt(pos, n);
            if (i % 5 == 0 && pos + 3 * n + 100 < to) readAt(pos + n + 100, n); // small forward step
            if (i % 7 == 0 && pos - 300 >= from) readAt(pos - 300, n);          // small step back
        }
        for (int64_t p = to - 1920; p >= from; p -= 1920) readAt(p, 1920); // backwards like reverse play
        // Lossless and AAC without PNS: bit-exact; Opus converges after its pre-roll; PNS: same level only
        const double jumpTol = c.pns ? 1.0 : (QString(c.codec) == "libopus" ? 1e-3 : 0);
        if (!CHECK(worstJump <= jumpTol)) std::printf("       random access differs by %.3g\n", worstJump);
        else std::printf("       random access within %.3g, level within %.2f dB (%d seeks)\n", worstJump, worstLevel, r->stats().seeks);
        CHECK(worstLevel <= 0.5);

        const Audio::Reader::Stats& st = r->stats();
        std::printf("       %s %d Hz %d ch, start %lld, end %lld; gaps %d, overlaps %d, errors %d, reopens %d\n",
                    qPrintable(info.codec), info.sourceRate, info.sourceChannels, (long long)info.start,
                    (long long)info.end, st.gaps, st.overlaps, st.decodeErrors, st.reopens);
        CHECK_EQ(st.gaps + st.overlaps + st.decodeErrors, 0);
    }
}

// Audio packets with overlapping timestamps (the user's clip program writes some, e.g. 2026-10-11_19-17-02): the
// sound follows the timestamps like a player (FFmpeg with aresample async), not the raw packet order
void testOverlaps(const QString& dir)
{
    std::printf("-- overlapping timestamps\n");
    const QString clean = Check::makeMedia(dir + "/beeps.mkv", {
        "-f", "lavfi", "-i", "color=c=black:s=160x90:r=25:d=8", "-f", "lavfi", "-i",
        "aevalsrc=if(lt(mod(t-1\\,2)\\,0.1)*gte(t\\,1)\\,0.5*sin(2*PI*1000*t)\\,0):s=48000:d=8",
        "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "192k", "-shortest"});
    // every 40th packet onwards 21 ms earlier: overlaps the one before, 9 times in 8 s (~190 ms)
    const QString overlapping = Check::makeMedia(dir + "/overlap.mkv", {"-i", clean, "-c", "copy", "-bsf:a", "setts=ts=TS-21*floor(N/40)"});
    if (!CHECK(!clean.isEmpty() && !overlapping.isEmpty())) return;
    auto ffmpegBeeps = [](const QString& path, const QString& filter) {
        QProcess p;
        p.start("ffmpeg", {"-v", "error", "-nostdin", "-i", path, "-map", "0:a:0", "-af", "pan=mono|c0=c0" + filter, "-f",
                           "f32le", "-ar", "48000", "-"});
        p.waitForFinished(60000);
        const QByteArray raw = p.readAllStandardOutput();
        const auto* f = reinterpret_cast<const float*>(raw.constData());
        return onsets(std::vector<float>(f, f + raw.size() / qsizetype(sizeof(float))), 0);
    };
    // FFmpeg plays the AAC priming here (Matroska marks it only as CodecDelay), the reader drops it: FFmpeg's beeps
    // come that much later in the clean file already
    auto cleanReader = Audio::Reader::open(clean);
    if (!CHECK(cleanReader)) return;
    const std::vector<int64_t> cleanOurs = onsets(readRange(*cleanReader, 0, 8 * kRate, 1920)[0], 0);
    const std::vector<int64_t> cleanFfmpeg = ffmpegBeeps(clean, {});
    if (!CHECK_EQ(int(cleanOurs.size()), 4) || !CHECK_EQ(int(cleanFfmpeg.size()), 4)) return;
    const int64_t primed = cleanFfmpeg[0] - cleanOurs[0];
    std::printf("       FFmpeg plays %lld priming samples\n", (long long)primed);
    std::vector<int64_t> want = ffmpegBeeps(overlapping, ",aresample=async=1:min_hard_comp=0.01:first_pts=0");
    for (int64_t& w : want) w -= primed;
    auto r = Audio::Reader::open(overlapping);
    if (!CHECK(r) || !CHECK_EQ(int(want.size()), 4)) return;
    const Planar got = readRange(*r, 0, 8 * kRate, 1920);
    const std::vector<int64_t> beeps = onsets(got[0], 0);
    if (!CHECK_EQ(int(beeps.size()), 4)) return;
    double worst = 0;
    for (size_t i = 0; i < 4; ++i) worst = std::max(worst, std::abs(double(beeps[i] - want[i])));
    // FFmpeg corrects at the packet where 10 ms are exceeded too; the beeps were written at whole milliseconds
    if (!CHECK(worst <= kMkvTol))
        for (size_t i = 0; i < 4; ++i)
            std::printf("       beep %zu at %lld, FFmpeg %lld\n", i, (long long)beeps[i], (long long)want[i]);
    else std::printf("       beeps within %.1f samples of FFmpeg's; %d overlaps\n", worst, r->stats().overlaps);
    CHECK(r->stats().overlaps >= 9);
}

// DTS stored as 16-bit PCM in a WAV (DTS CDs): FFmpeg finds it while reading the first packets
void testDtsInWav(const QString& dir)
{
    std::printf("-- DTS in WAV\n");
    const QString dts = Check::makeMedia(dir + "/x.dts", {"-f", "lavfi", "-i", "sine=f=440:d=3:sample_rate=44100", "-ac", "2",
                                                          "-c:a", "dca", "-strict", "-2", "-b:a", "1411k"});
    if (dts.isEmpty()) return; // no DTS encoder
    const QString wav = Check::makeMedia(dir + "/dts.wav", {"-f", "s16le", "-ac", "2", "-ar", "44100", "-i", dts, "-c:a", "copy"});
    auto r = Audio::Reader::open(wav);
    if (!CHECK(r)) return;
    CHECK_EQ(r->info().codec, QString("dca"));
    const Planar p = readRange(*r, 0, kRate, 1920);
    float peak = 0;
    for (float v : p[0]) peak = std::max(peak, std::abs(v));
    if (!CHECK(peak > 0.05f && peak < 1.f)) std::printf("       peak %.3f\n", peak);
}

void testErrors(const QString& dir)
{
    QString error;
    CHECK(!Audio::Reader::open(dir + "/missing.wav", 0, {}, &error));
    CHECK(!error.isEmpty());
    CHECK(!Audio::Reader::open(dir + "/wav.wav", 1)); // only one stream
    const QString silent = Check::makeMedia(dir + "/picture.mp4", {"-f", "lavfi", "-i", "color=c=black:s=160x90:r=25:d=1",
                                                                    "-c:v", "libx264", "-preset", "ultrafast"});
    CHECK(!Audio::Reader::open(silent, 0)); // no sound at all

    // Remixing to stereo, other output rate
    auto mono = Audio::Reader::open(dir + "/mono.wav", 0, {44100, 2});
    if (CHECK(mono)) {
        CHECK_EQ(mono->channels(), 2);
        CHECK_EQ(mono->rate(), 44100);
        const Planar p = readRange(*mono, 0, 44100 * 2, 1000);
        CHECK(maxDiff(p, 0, Planar{p[1]}, 0, p[0].size()) == 0); // both channels the same
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("audio_reader");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    Check::initEngine();
    QTemporaryDir dir;
    for (const Case& c : kCases) testCase(dir.path(), c);
    testOverlaps(dir.path());
    testDtsInWav(dir.path());
    testErrors(dir.path());
    return Check::result();
}
