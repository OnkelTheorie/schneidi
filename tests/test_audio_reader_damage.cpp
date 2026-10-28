// Own audio engine: Audio::Reader with damaged or vanishing sources
// - the file disappears while it is open (deleted, USB disk unplugged): a jump to the start reads silence and reports
//   an error instead of crashing
// - a long timestamp gap (muted microphone track) is known silence, not a buffer of the gap's whole length
// - a single packet with a timestamp far ahead (broken recording) does not swallow the rest of the file
// - Matroska without a duration (the recorder crashed): the end is estimated from the last packets

#include "check.h"

#include "engine/audio/Reader.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

extern "C" {
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr int kRate = 48000;

using Planar = std::vector<std::vector<float>>;

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

double peak(const std::vector<float>& s, size_t from, size_t to)
{
    double m = 0;
    for (size_t i = from; i < to && i < s.size(); ++i) m = std::max(m, double(std::abs(s[i])));
    return m;
}

// 10 ms windows of [from, to) without sound (a steady sine: every window has some)
int silentWindows(const std::vector<float>& s, size_t from, size_t to)
{
    int n = 0;
    for (size_t w = from; w + 480 <= to && w + 480 <= s.size(); w += 480)
        if (peak(s, w, w + 480) < 0.05) ++n;
    return n;
}

// Copies `in` to `out` (Matroska), the audio packet number `which` gets a presentation timestamp `ahead` seconds
// later (decode timestamp unchanged: the muxer takes it, FFmpeg's command line would "fix" the following packets)
bool writeOutlier(const QString& in, const QString& out, int which, double ahead)
{
    AVFormatContext* ic = nullptr;
    if (avformat_open_input(&ic, in.toUtf8().constData(), nullptr, nullptr) < 0) return false;
    AVFormatContext* oc = nullptr;
    avformat_alloc_output_context2(&oc, nullptr, "matroska", out.toUtf8().constData());
    bool ok = oc && avformat_find_stream_info(ic, nullptr) >= 0;
    for (unsigned i = 0; ok && i < ic->nb_streams; ++i) {
        AVStream* os = avformat_new_stream(oc, nullptr);
        ok = os && avcodec_parameters_copy(os->codecpar, ic->streams[i]->codecpar) >= 0;
        if (ok) os->time_base = ic->streams[i]->time_base;
    }
    ok = ok && avio_open(&oc->pb, out.toUtf8().constData(), AVIO_FLAG_WRITE) >= 0 && avformat_write_header(oc, nullptr) >= 0;
    AVPacket* pkt = av_packet_alloc();
    int n = 0;
    while (ok && av_read_frame(ic, pkt) >= 0) {
        AVStream* is = ic->streams[pkt->stream_index];
        if (is->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && n++ == which && pkt->pts != AV_NOPTS_VALUE)
            pkt->pts += std::llround(ahead * is->time_base.den / is->time_base.num);
        av_packet_rescale_ts(pkt, is->time_base, oc->streams[pkt->stream_index]->time_base);
        ok = av_write_frame(oc, pkt) >= 0;
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    if (ok) ok = av_write_trailer(oc) >= 0;
    if (oc) avio_closep(&oc->pb);
    avformat_free_context(oc);
    avformat_close_input(&ic);
    return ok;
}

// The file's data vanishes while it is open (disk unplugged; on Linux an open file keeps its data when deleted, emptying
// it makes reads fail like a gone disk): jumps never crash (what is still buffered may sound). The crash of a failed
// fresh demuxer (fmt == nullptr after a failed reopen) needs a demuxer whose seek fails – none of FFmpeg's common ones
// did here, so that path is guarded in the code without a file to reach it.
void testVanished(const QString& dir)
{
    std::printf("-- file vanishes while open\n");
    for (const char* ext : {"mkv", "m4a", "wav"}) {
        const QString path = Check::makeMedia(dir + "/vanish." + ext, {"-f", "lavfi", "-i", "sine=f=440:d=6"});
        if (!CHECK(!path.isEmpty())) continue;
        auto r = Audio::Reader::open(path);
        if (!CHECK(r)) continue;
        std::vector<float> l(4800), rr(4800);
        float* out[] = {l.data(), rr.data()};
        r->read(3 * kRate, 4800, out);
        CHECK(peak(l, 0, l.size()) > 0.1);
        if (!CHECK(QFile::resize(path, 0))) continue;
        for (int64_t p : {r->info().start, int64_t(5 * kRate), int64_t(kRate), int64_t(0), int64_t(4 * kRate)}) {
            std::fill(l.begin(), l.end(), 7.f);
            r->read(p, 4800, out);
            CHECK(peak(l, 0, l.size()) < 1.0); // filled: sound still buffered or silence
        }
    }
}

// A track that has no packets for an hour (muted microphone of a multi-track recording)
void testLongGap(const QString& dir)
{
    std::printf("-- one hour without packets\n");
    const QString base = Check::makeMedia(dir + "/gapbase.mkv", {"-f", "lavfi", "-i", "sine=f=440:d=4", "-c:a", "aac"});
    // from packet 100 (2.32 s) on, everything an hour later (Matroska: milliseconds)
    const QString gap = Check::makeMedia(dir + "/gap.mkv", {"-i", base, "-c", "copy", "-bsf:a", "setts=ts=if(gte(N\\,100)\\,TS+3600000\\,TS)"});
    if (!CHECK(!gap.isEmpty())) return;
    auto r = Audio::Reader::open(gap);
    if (!CHECK(r)) return;
    // Sequential reading over the start of the gap, as playback does (256 samples per block)
    QElapsedTimer t;
    t.start();
    const Planar a = readRange(*r, kRate, 4 * kRate, 256);
    const double ms = t.nsecsElapsed() / 1e6;
    std::printf("       3 s over the gap's start in %.1f ms, %d gaps\n", ms, r->stats().gaps);
    CHECK(ms < 2000); // a whole hour of silence (1.4 GB) took several seconds
    CHECK_EQ(r->stats().gaps, 1);
    CHECK(peak(a[0], 0, size_t(kRate)) > 0.1);                    // sound before the gap
    CHECK_EQ(peak(a[0], size_t(1.4 * kRate), size_t(3 * kRate)), 0.0); // silence in it
    // Inside the gap and after it
    const Planar mid = readRange(*r, 1800 * kRate, 1800 * kRate + kRate, 256);
    CHECK_EQ(peak(mid[0], 0, mid[0].size()), 0.0);
    const int64_t after = int64_t(3602.5 * kRate);
    const Planar b = readRange(*r, after, after + kRate / 2, 256);
    CHECK(peak(b[0], 0, b[0].size()) > 0.1);
    // Sequentially from shortly before the end of the gap into the sound
    const Planar c = readRange(*r, 3600 * kRate, int64_t(3603.5 * kRate), 256);
    CHECK_EQ(peak(c[0], 0, size_t(1.5 * kRate)), 0.0);
    CHECK(peak(c[0], size_t(2.4 * kRate), size_t(3.4 * kRate)) > 0.1);
    CHECK_EQ(silentWindows(c[0], size_t(2.4 * kRate), size_t(3.4 * kRate)), 0);
}

// One packet with a timestamp two minutes ahead (broken recording); the rest of the file follows normally
void testOutlier(const QString& dir)
{
    std::printf("-- one packet far ahead\n");
    const QString base = Check::makeMedia(dir + "/outbase.mkv", {"-f", "lavfi", "-i", "sine=f=440:d=6", "-c:a", "aac"});
    const QString out = dir + "/outlier.mkv";
    if (!CHECK(!base.isEmpty() && writeOutlier(base, out, 100, 120))) return;
    auto r = Audio::Reader::open(out);
    if (!CHECK(r)) return;
    const Planar a = readRange(*r, 0, 6 * kRate, 256);
    // At most the outlier's own packet is missing (21 ms), the sound after it is there
    const int silent = silentWindows(a[0], size_t(kRate / 10), size_t(5.8 * kRate));
    std::printf("       %d silent 10 ms windows; gaps %d, overlaps %d\n", silent, r->stats().gaps, r->stats().overlaps);
    CHECK(silent <= 3);
    CHECK(peak(a[0], size_t(4 * kRate), size_t(5.8 * kRate)) > 0.1);
    CHECK(r->stats().overlaps <= 1);
}

// Matroska written as a live stream (or by a recorder that crashed): no duration in the header
void testNoDuration(const QString& dir)
{
    std::printf("-- Matroska without duration\n");
    for (bool video : {true, false}) {
        QStringList args;
        if (video) args << "-f" << "lavfi" << "-i" << "color=c=black:s=160x90:r=25:d=6";
        args << "-f" << "lavfi" << "-i" << "sine=f=440:d=6";
        if (video) args << "-c:v" << "libx264" << "-preset" << "ultrafast" << "-pix_fmt" << "yuv420p";
        args << "-c:a" << "aac" << "-live" << "1";
        const QString path = Check::makeMedia(dir + (video ? "/live_av.mkv" : "/live_a.mkv"), args);
        if (!CHECK(!path.isEmpty())) continue;
        auto r = Audio::Reader::open(path);
        if (!CHECK(r)) continue;
        std::printf("       %s: end %lld\n", video ? "with picture" : "sound only", (long long)r->info().end);
        CHECK(std::abs(r->info().end - 6 * kRate) <= kRate / 20);
        // Still reads from the start after estimating the end
        const Planar a = readRange(*r, 0, kRate, 1920);
        CHECK(peak(a[0], size_t(kRate / 10), size_t(kRate)) > 0.1);
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("audio_reader_damage");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir dir;
    testVanished(dir.path());
    testLongGap(dir.path());
    testOutlier(dir.path());
    testNoDuration(dir.path());
    return Check::result();
}
