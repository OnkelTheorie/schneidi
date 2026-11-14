// SPDX-License-Identifier: AGPL-3.0-only
// Multichannel sound (0.23, docs/plan-0.23.md Block 1), with synthetic files only (ffmpeg lavfi):
// - ChannelLayout: names, panner gains, ITU downmix, loudness weights
// - routing: a clip that plays channels 3/4 (or one channel) of a four-channel file
// - the mixer in 5.1: a stereo clip placed by the surround panner (front, surround, centre, LFE send low-passed),
//   a 5.1 file passing through channel by channel, the stereo downmix for the monitor, 7.1
// - loudness over all channels (surround +1.5 dB, LFE not counted)
// - export in 5.1/7.1 (AAC, FLAC, WAV: ffprobe channel_layout), MP3 falls back to stereo with a note
// - the project file: format 8 only when used (stereo projects unchanged), roundtrip, older versions refuse it

#include "check.h"

#include "core/ChannelLayout.h"
#include "core/Loudness.h"
#include "core/ProjectFile.h"
#include "core/RenderJob.h"
#include "engine/AudioAnalysis.h"
#include "engine/Exporter.h"
#include "engine/MediaProbe.h"
#include "engine/RenderQueue.h"
#include "engine/audio/Mixer.h"

#include <QApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <cmath>
#include <vector>

namespace {

constexpr int kRate = 48000;
const ProjectFormat kFmt{320, 180, {25, 1}};
// Frequencies per source channel (1-based channel n gets kFreq[n - 1])
constexpr double kFreq[8] = {300, 500, 700, 900, 1100, 1300, 1500, 1700};

Clip audioClip(int id, const QString& path, int frames)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = 0;
    c.in = 0;
    c.out = frames - 1;
    return c;
}

Timeline oneClip(const Clip& c, int channels)
{
    Timeline tl;
    tl.audio.resize(1);
    tl.audio[0].clips << c;
    tl.audioChannels = channels;
    return tl;
}

// Renders [from, from + n) of the mix; planar per output channel
std::vector<std::vector<float>> renderMix(const Timeline& tl, int64_t from, int n, int outputChannels = 0)
{
    Audio::Mixer::Options o;
    o.rate = kRate;
    o.declick = false;
    o.outputChannels = outputChannels;
    Audio::Mixer m(tl, kFmt.rate, o);
    std::vector<std::vector<float>> out(size_t(m.outputChannels()), std::vector<float>(size_t(n)));
    float* p[8];
    for (int c = 0; c < m.outputChannels(); ++c) p[c] = out[size_t(c)].data();
    m.render(from, n, p);
    return out;
}

double rms(const std::vector<float>& x)
{
    double s = 0;
    for (float v : x) s += double(v) * v;
    return x.empty() ? 0 : std::sqrt(s / double(x.size()));
}

// Amplitude of frequency f in x (single-bin DFT; f a whole number of periods in x at 48 kHz)
double tone(const std::vector<float>& x, double f)
{
    double re = 0, im = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        const double w = 2 * M_PI * f * double(i) / kRate;
        re += x[i] * std::cos(w);
        im += x[i] * std::sin(w);
    }
    return 2 * std::hypot(re, im) / double(x.size());
}

QJsonObject ffprobeAudio(const QString& path)
{
    QProcess p;
    p.start("ffprobe", {"-v", "error", "-select_streams", "a:0", "-show_streams", "-of", "json", path});
    p.waitForFinished(60000);
    return QJsonDocument::fromJson(p.readAllStandardOutput()).object().value("streams").toArray().first().toObject();
}

bool exportTo(const Timeline& tl, const QString& path, const QString& format, int channels, QString* note = nullptr,
              int* written = nullptr)
{
    RenderJob job;
    job.id = 1;
    job.path = path;
    job.settings.format = format;
    job.settings.audioChannels = channels;
    Exporter ex;
    QEventLoop loop;
    bool ok = false;
    QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString&) {
        ok = success;
        loop.quit();
    });
    QString error;
    if (!CHECK(ex.start(tl, RenderQueue::exportSettings(job, kFmt), &error))) {
        std::printf("       %s\n", qPrintable(error));
        return false;
    }
    QTimer::singleShot(120000, &loop, &QEventLoop::quit);
    loop.exec();
    if (note) *note = ex.note();
    if (written) *written = ex.audioChannels();
    return CHECK(ok);
}

QJsonObject cli(const QStringList& args)
{
    QProcess p;
    p.start(Check::cliPath(), args);
    p.closeWriteChannel();
    if (!p.waitForFinished(180000)) {
        p.kill();
        p.waitForFinished(1000);
    }
    const QJsonObject o = QJsonDocument::fromJson(p.readAllStandardOutput()).object();
    if (!o.value("ok").toBool()) std::printf("       %s: %s\n", qPrintable(args.value(0)), qPrintable(QJsonDocument(o).toJson(QJsonDocument::Compact).left(400)));
    return o;
}

QJsonObject ops(const QString& project, const QJsonArray& list)
{
    return cli({"edit", project, "--ops", QString::fromUtf8(QJsonDocument(list).toJson(QJsonDocument::Compact))});
}

// CLI/MCP: format channels, route, mixer depth/center/lfe_db, probe audio_channels, render channels
void testCli(const QTemporaryDir& tmp, const QString& quad)
{
    if (Check::cliPath().isEmpty()) {
        std::printf("       schneidi-cli nicht gefunden, CLI-Teil übersprungen\n");
        return;
    }
    const QJsonObject probe = cli({"probe", quad});
    const QJsonObject layout = probe.value("audio_channels").toArray().first().toObject();
    CHECK(layout.value("channels").toInt() == 4 && layout.value("layout").toString() == "4.0");

    const QString project = tmp.filePath("cli.schneidi");
    CHECK(cli({"new", project, "--template", "neutral", "--media", QString("[\"%1\"]").arg(quad)}).value("ok").toBool());
    CHECK(ops(project, {QJsonObject{{"op", "append"}, {"media", quad}}}).value("ok").toBool());
    QJsonObject info = cli({"info", project});
    CHECK(info.value("format").toObject().value("channels").toString() == "stereo");
    // The clip on A1 (an audio file: one clip)
    int id = 0;
    for (const QJsonValue& t : info.value("timeline").toObject().value("tracks").toArray())
        for (const QJsonValue& c : t.toObject().value("clips").toArray())
            if (!id) id = c.toObject().value("id").toInt();
    CHECK(id > 0);
    // Channels 1/2 on the clip, 3/4 as a copy on a new track
    CHECK(ops(project, {QJsonObject{{"op", "route"}, {"clips", QJsonArray{id}}, {"channels", QJsonArray{1, 2}}}}).value("ok").toBool());
    CHECK(ops(project, {QJsonObject{{"op", "route"}, {"clips", QJsonArray{id}}, {"channels", "3-4"}, {"track", "new"},
                                    {"name", "Funk"}}}).value("ok").toBool());
    // Too many channels / a channel the file lacks: refused
    CHECK(!ops(project, {QJsonObject{{"op", "route"}, {"clips", QJsonArray{id}}, {"channels", QJsonArray{1, 2, 3}}}}).value("ok").toBool());
    CHECK(!ops(project, {QJsonObject{{"op", "route"}, {"clips", QJsonArray{id}}, {"channels", 5}}}).value("ok").toBool());
    // 5.1 and the panner
    CHECK(ops(project, {QJsonObject{{"op", "format"}, {"channels", "5.1"}},
                        QJsonObject{{"op", "mixer"}, {"track", "A1"}, {"depth", 100}, {"lfe_db", -6}}}).value("ok").toBool());
    CHECK(!ops(project, {QJsonObject{{"op", "format"}, {"channels", "quad"}}}).value("ok").toBool());
    info = cli({"info", project});
    CHECK(info.value("format").toObject().value("channels").toString() == "5.1");
    ProjectData d;
    QString error;
    CHECK(ProjectFile::load(project, &d, &error));
    CHECK(d.format.audioChannels == 6 && d.timeline.audio.size() >= 2);
    if (d.timeline.audio.size() >= 2) {
        const Track& a1 = d.timeline.audio[0];
        CHECK(a1.panDepth == 100 && a1.lfeDb == -6 && a1.clips.value(0).sourceChannels == QVector<int>({0, 1}));
        const Track* funk = nullptr;
        for (const Track& t : d.timeline.audio)
            if (t.name == "Funk") funk = &t;
        CHECK(funk && funk->clips.size() == 1 && funk->clips.value(0).sourceChannels == QVector<int>({2, 3}));
    }
    // Render: the project's 5.1 by default, stereo when asked
    const QString out51 = tmp.filePath("cli.flac");
    const QJsonObject r = cli({"render", project, "--out", out51, "--format", "flac"});
    CHECK(r.value("channels").toString() == "5.1" && ffprobeAudio(out51).value("channel_layout").toString() == "5.1");
    const QString out2 = tmp.filePath("cli-stereo.flac");
    const QJsonObject r2 = cli({"render", project, "--out", out2, "--format", "flac", "--channels", "stereo"});
    CHECK(r2.value("channels").toString() == "stereo" && ffprobeAudio(out2).value("channels").toInt() == 2);
    // Loudness measures the 5.1 mix
    CHECK(cli({"loudness", project}).value("ok").toBool());
}

void testLayout()
{
    CHECK(ChannelLayout::name(2) == "stereo" && ChannelLayout::name(6) == "5.1" && ChannelLayout::name(8) == "7.1");
    CHECK(ChannelLayout::parse(" 5.1 ") == 6 && ChannelLayout::parse("7.1") == 8 && ChannelLayout::parse("Stereo") == 2);
    CHECK(ChannelLayout::parse("quad") == 0);
    CHECK(ChannelLayout::label(6, 3) == "LFE" && ChannelLayout::label(8, 6) == "Lss");
    // Weights (BS.1770): LFE not counted, surrounds 1.41
    CHECK(ChannelLayout::loudnessWeight(6, 3) == 0.0 && ChannelLayout::loudnessWeight(6, 4) == 1.41);
    CHECK(ChannelLayout::loudnessWeight(8, 7) == 1.41 && ChannelLayout::loudnessWeight(8, 2) == 1.0);
    // Downmix ITU-R BS.775
    const ChannelLayout::Matrix d = ChannelLayout::conversion(6, 2);
    CHECK(d[0][0] == 1.0 && std::abs(d[0][2] - M_SQRT1_2) < 1e-12 && std::abs(d[0][4] - M_SQRT1_2) < 1e-12);
    CHECK(d[0][3] == 0.0 && d[0][1] == 0.0 && d[1][5] > 0.7);
    const ChannelLayout::Matrix d8 = ChannelLayout::conversion(8, 2);
    CHECK(std::abs(d8[1][7] - M_SQRT1_2) < 1e-12 && std::abs(d8[1][5] - M_SQRT1_2) < 1e-12);

    // Panner: centred and in front = identity on the front pair
    auto g = ChannelLayout::surroundPan(6, 0, 0, 0, ChannelLayout::kLfeOff);
    CHECK(g.m[0][0] == 1.0 && g.m[1][1] == 1.0 && g.m[2][0] == 0.0 && g.m[4][0] == 0.0 && g.lfe == 0.0);
    // Fully back: surround pair only
    g = ChannelLayout::surroundPan(6, 0, 100, 0, ChannelLayout::kLfeOff);
    CHECK(g.m[0][0] == 0.0 && g.m[4][0] == 1.0 && g.m[5][1] == 1.0);
    // Halfway: constant power
    g = ChannelLayout::surroundPan(6, 0, 50, 0, ChannelLayout::kLfeOff);
    CHECK(std::abs(g.m[0][0] * g.m[0][0] + g.m[4][0] * g.m[4][0] - 1.0) < 1e-12);
    // Centre 100: the front pair half each into C
    g = ChannelLayout::surroundPan(6, 0, 0, 100, ChannelLayout::kLfeOff);
    CHECK(g.m[0][0] == 0.0 && g.m[2][0] == 0.5 && g.m[2][1] == 0.5);
    // Pan right: left turned down like the stereo balance
    g = ChannelLayout::surroundPan(6, 50, 0, 0, ChannelLayout::kLfeOff);
    CHECK(g.m[0][0] == 0.5 && g.m[1][1] == 1.0);
    // LFE send 0 dB = (L + R) / 2
    g = ChannelLayout::surroundPan(6, 0, 0, 0, 0);
    CHECK(g.lfe == 0.5);
    // 7.1: depth 50 = side pair, 100 = back pair
    g = ChannelLayout::surroundPan(8, 0, 50, 0, ChannelLayout::kLfeOff);
    CHECK(g.m[6][0] == 1.0 && g.m[0][0] == 0.0 && g.m[4][0] == 0.0);
    g = ChannelLayout::surroundPan(8, 0, 100, 0, ChannelLayout::kLfeOff);
    CHECK(g.m[4][0] == 1.0 && g.m[6][0] == 0.0);
}

void testRouting(const QString& quad)
{
    Clip c = audioClip(1, quad, 25);
    // Channels 3/4 -> left/right
    c.sourceChannels = {2, 3};
    auto out = renderMix(oneClip(c, 2), 4800, 4800);
    CHECK(out.size() == 2);
    CHECK(tone(out[0], kFreq[2]) > 0.19 && tone(out[0], kFreq[0]) < 0.001 && tone(out[0], kFreq[3]) < 0.001);
    CHECK(tone(out[1], kFreq[3]) > 0.19 && tone(out[1], kFreq[2]) < 0.001);
    // One channel: in the middle, -3 dB each side
    c.sourceChannels = {1};
    out = renderMix(oneClip(c, 2), 4800, 4800);
    CHECK(std::abs(tone(out[0], kFreq[1]) - 0.2 * M_SQRT1_2) < 0.005);
    CHECK(std::abs(tone(out[1], kFreq[1]) - 0.2 * M_SQRT1_2) < 0.005 && tone(out[1], kFreq[0]) < 0.001);
    // A channel the stream lacks is silent
    c.sourceChannels = {7};
    out = renderMix(oneClip(c, 2), 4800, 4800);
    CHECK(rms(out[0]) < 1e-6);
    // Routed in 5.1: a stereo voice on the front pair
    c.sourceChannels = {2, 3};
    out = renderMix(oneClip(c, 6), 4800, 4800);
    CHECK(out.size() == 6 && tone(out[0], kFreq[2]) > 0.19 && tone(out[1], kFreq[3]) > 0.19 && rms(out[2]) < 1e-6
          && rms(out[4]) < 1e-6);
    // The probe names the layout
    const QVector<MediaProbe::AudioLayout> layouts = MediaProbe::audioLayouts(quad);
    CHECK(layouts.size() == 1 && layouts.value(0).channels == 4);
    std::printf("       Layout der Vierkanal-Datei: %s\n", qPrintable(layouts.value(0).layout));
}

void testSurroundMix(const QString& stereo, const QString& five, const QString& bass)
{
    // Stereo clip in 5.1, panner at rest: front pair only
    Timeline tl = oneClip(audioClip(1, stereo, 25), 6);
    auto out = renderMix(tl, 4800, 4800);
    CHECK(out.size() == 6);
    CHECK(tone(out[0], kFreq[0]) > 0.19 && tone(out[1], kFreq[1]) > 0.19);
    for (int c = 2; c < 6; ++c) CHECK(rms(out[size_t(c)]) < 1e-7);
    // Depth 100: surround pair
    tl.audio[0].panDepth = 100;
    out = renderMix(tl, 4800, 4800);
    CHECK(rms(out[0]) < 1e-7 && tone(out[4], kFreq[0]) > 0.19 && tone(out[5], kFreq[1]) > 0.19);
    // Centre 100: both halves in C
    tl.audio[0].panDepth = 0;
    tl.audio[0].panCenter = 100;
    out = renderMix(tl, 4800, 4800);
    CHECK(rms(out[0]) < 1e-7 && std::abs(tone(out[2], kFreq[0]) - 0.1) < 0.003 && std::abs(tone(out[2], kFreq[1]) - 0.1) < 0.003);
    // The stereo downmix of that: C at -3 dB on both sides
    const auto down = renderMix(tl, 4800, 4800, 2);
    CHECK(down.size() == 2 && std::abs(tone(down[0], kFreq[0]) - 0.1 * M_SQRT1_2) < 0.003);
    // A stereo mix ignores the panner (bit-identical to a track without it)
    {
        Timeline st = oneClip(audioClip(1, stereo, 25), 2);
        const auto plain = renderMix(st, 4800, 4800);
        st.audio[0].panDepth = 100;
        st.audio[0].panCenter = 30;
        st.audio[0].lfeDb = 0;
        const auto with = renderMix(st, 4800, 4800);
        CHECK(plain == with);
    }

    // LFE send: 50 Hz passes the 120 Hz low pass, 2 kHz hardly
    Timeline lf = oneClip(audioClip(1, bass, 25), 6);
    lf.audio[0].lfeDb = 0;
    out = renderMix(lf, 9600, 4800);
    const double low = tone(out[3], 50), high = tone(out[3], 2000);
    std::printf("       LFE-Send: 50 Hz %.4f, 2 kHz %.5f\n", low, high);
    CHECK(low > 0.08 && high < low * 0.02);
    CHECK(tone(out[0], 50) > 0.09); // the front stays

    // A 5.1 file passes through channel by channel (each channel its own tone)
    Timeline fv = oneClip(audioClip(1, five, 25), 6);
    out = renderMix(fv, 4800, 4800);
    for (int c = 0; c < 6; ++c) {
        const double own = tone(out[size_t(c)], kFreq[c]);
        const double other = tone(out[size_t(c)], kFreq[(c + 1) % 6]);
        if (!CHECK(own > 0.19 && other < 0.001)) std::printf("       Kanal %d: eigen %.4f fremd %.4f\n", c, own, other);
    }
    // ... and in a stereo project it is downmixed as before (FFmpeg's normalized matrix): no channel silent
    out = renderMix(oneClip(audioClip(1, five, 25), 2), 4800, 4800);
    CHECK(out.size() == 2 && tone(out[0], kFreq[0]) > 0.05 && tone(out[0], kFreq[2]) > 0.03);
    // 5.1 file in 7.1: front, centre, LFE stay; its surround pair lands in one pair of the 7.1 (FFmpeg maps by name)
    out = renderMix(oneClip(audioClip(1, five, 25), 8), 4800, 4800);
    CHECK(out.size() == 8 && tone(out[2], kFreq[2]) > 0.19 && tone(out[4], kFreq[4]) + tone(out[6], kFreq[4]) > 0.19);
    // Monitor downmix of the 5.1 file: L = FL + .707 C + .707 Ls
    const auto mon = renderMix(fv, 4800, 4800, 2);
    CHECK(std::abs(tone(mon[0], kFreq[0]) - 0.2) < 0.003 && std::abs(tone(mon[0], kFreq[2]) - 0.2 * M_SQRT1_2) < 0.003
          && std::abs(tone(mon[0], kFreq[4]) - 0.2 * M_SQRT1_2) < 0.003 && tone(mon[0], kFreq[3]) < 0.001);
    // Track meters: L from the left speakers, R from the right ones
    {
        Audio::Mixer::Options o;
        o.rate = kRate;
        Audio::Mixer m(fv, kFmt.rate, o);
        m.setMetering(true);
        std::vector<std::vector<float>> buf(6, std::vector<float>(4800));
        float* p[6];
        for (int c = 0; c < 6; ++c) p[c] = buf[size_t(c)].data();
        m.render(4800, 4800, p);
        CHECK(m.channels() == 6 && m.outputChannels() == 6 && m.trackPeaks().size() == 2 && m.trackPeaks()[0] > 0.19);
    }
}

void testLoudness(const QString& tone1k)
{
    // The same tone on the front pair and on the surround pair of a 5.1 mix: surrounds +1.5 dB (BS.1770)
    const auto measure = [&](double depth) {
        Timeline tl = oneClip(audioClip(1, tone1k, 100), 6);
        tl.audio[0].clips[0].sourceChannels = {0}; // mono in the middle of the front pair
        tl.audio[0].panDepth = depth;
        const auto l = AudioAnalysis::timelineLoudness(kFmt, tl, 0, 100);
        return l ? l->integrated : -999.0;
    };
    const double front = measure(0), back = measure(100);
    std::printf("       Lautheit vorn %.2f LUFS, Surround %.2f LUFS\n", front, back);
    CHECK(std::abs((back - front) - 10 * std::log10(1.41)) < 0.05);
    // The stereo project measures the same tone as before (front pair = stereo)
    Timeline st = oneClip(audioClip(1, tone1k, 100), 2);
    st.audio[0].clips[0].sourceChannels = {0};
    const auto s = AudioAnalysis::timelineLoudness(kFmt, st, 0, 100);
    CHECK(s && std::abs(s->integrated - front) < 0.01);
    // True peak over every channel: the surround pair holds the peak
    Timeline b = oneClip(audioClip(1, tone1k, 100), 6);
    b.audio[0].panDepth = 100;
    const auto tp = AudioAnalysis::timelineLoudness(kFmt, b, 0, 100);
    if (!CHECK(tp && tp->truePeakDb > -17.5 && tp->truePeakDb < -16.5)) // 0.2 at -3 dB each side = -17 dBFS
        std::printf("       True Peak %.2f\n", tp ? tp->truePeakDb : -999.0);
}

void testExport(const QTemporaryDir& tmp, const QString& stereo, const QString& five)
{
    Timeline tl;
    tl.audio.resize(2);
    tl.audio[0].clips << audioClip(1, five, 25);
    tl.audio[1].clips << audioClip(2, stereo, 25);
    tl.audio[1].panDepth = 100;
    tl.audioChannels = 6;
    struct Case { const char* format; const char* ext; int channels; const char* layout; };
    const Case cases[] = {
        {"aac", "m4a", 0, "5.1"}, {"flac", "flac", 0, "5.1"}, {"wav", "wav", 0, "5.1"}, {"aac", "m4a", 8, "7.1"},
        {"flac", "flac", 2, "stereo"},
    };
    for (const Case& k : cases) {
        const QString path = tmp.filePath(QString("out-%1-%2.%3").arg(k.format).arg(k.channels).arg(k.ext));
        int written = 0;
        if (!exportTo(tl, path, k.format, k.channels, nullptr, &written)) continue;
        const QJsonObject a = ffprobeAudio(path);
        const QString layout = a.value("channel_layout").toString();
        if (!CHECK(layout == k.layout)) std::printf("       %s: Layout %s statt %s\n", k.format, qPrintable(layout), k.layout);
        CHECK(written == ChannelLayout::parse(k.layout));
    }
    // The 5.1 FLAC holds the 5.1 file's channels in place and the panned stereo clip in the surrounds
    {
        const QString path = tmp.filePath("out-flac-0.flac");
        QProcess p;
        p.start("ffmpeg", {"-v", "error", "-nostdin", "-i", path, "-f", "f32le", "-"});
        p.waitForFinished(60000);
        const QByteArray d = p.readAllStandardOutput();
        const auto* f = reinterpret_cast<const float*>(d.constData());
        const size_t frames = size_t(d.size()) / sizeof(float) / 6;
        std::vector<std::vector<float>> ch(6);
        for (size_t i = 4800; i < std::min(frames, size_t(9600)); ++i)
            for (int c = 0; c < 6; ++c) ch[size_t(c)].push_back(f[i * 6 + size_t(c)]);
        CHECK(tone(ch[2], kFreq[2]) > 0.19 && tone(ch[3], kFreq[3]) > 0.19);
        CHECK(tone(ch[4], kFreq[0]) > 0.19 && tone(ch[5], kFreq[1]) > 0.19); // stereo clip panned back
    }
    // MP3 has no 5.1: stereo downmix and a note
    {
        QString note;
        int written = 0;
        const QString path = tmp.filePath("out.mp3");
        if (exportTo(tl, path, "mp3", 0, &note, &written)) {
            CHECK(ffprobeAudio(path).value("channels").toInt() == 2 && written == 2 && !note.isEmpty());
            std::printf("       MP3-Hinweis: %s\n", qPrintable(note));
        }
    }
    // A stereo project still writes stereo by default
    {
        Timeline st = tl;
        st.audioChannels = 2;
        const QString path = tmp.filePath("stereo.m4a");
        if (exportTo(st, path, "aac", 0)) CHECK(ffprobeAudio(path).value("channel_layout").toString() == "stereo");
    }
}

void testProjectFile(const QTemporaryDir& tmp)
{
    ProjectData d;
    d.format = kFmt;
    MediaInfo m;
    m.path = tmp.filePath("cam.mov");
    m.hasAudio = true;
    m.length = 100;
    m.audioStreams = {AudioStreamInfo{4, {}, true}};
    d.media << m;
    d.timeline.audio.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = m.path;
    c.out = 49;
    d.timeline.audio[0].clips << c;
    const QString path = tmp.filePath("p.schneidi");

    // Stereo, no routing: no new fields, an older format version
    QJsonObject root = QJsonDocument::fromJson(ProjectFile::toJson(d, path)).object();
    CHECK(root.value("version").toInt() < 8);
    CHECK(!root.value("format").toObject().contains("channels"));
    // The surround panner alone (stereo project) needs no new version either
    d.timeline.audio[0].panDepth = 40;
    root = QJsonDocument::fromJson(ProjectFile::toJson(d, path)).object();
    CHECK(root.value("version").toInt() < 8);

    // Routing: format 8
    d.timeline.audio[0].clips[0].sourceChannels = {2, 3};
    root = QJsonDocument::fromJson(ProjectFile::toJson(d, path)).object();
    CHECK(root.value("version").toInt() == 8);
    d.timeline.audio[0].clips[0].sourceChannels.clear();
    // 5.1: format 8
    d.format.audioChannels = 6;
    d.timeline.audio[0].clips[0].sourceChannels = {2};
    d.timeline.audio[0].panCenter = 25;
    d.timeline.audio[0].lfeDb = -6;
    const QByteArray json = ProjectFile::toJson(d, path);
    root = QJsonDocument::fromJson(json).object();
    CHECK(root.value("version").toInt() == 8 && root.value("format").toObject().value("channels").toInt() == 6);

    // Roundtrip
    ProjectData back;
    QString error;
    CHECK(ProjectFile::fromJson(json, path, &back, &error));
    CHECK(back.format.audioChannels == 6 && back.format == d.format);
    const Track& t = back.timeline.audio.value(0);
    CHECK(t.panDepth == 40 && t.panCenter == 25 && t.lfeDb == -6);
    CHECK(t.clips.value(0).sourceChannels == QVector<int>{2});

    // An older schneidi (format 7 at most) refuses it instead of mixing it in stereo; this one refuses a newer one
    QJsonObject newer = root;
    newer["version"] = 9;
    CHECK(!ProjectFile::fromJson(QJsonDocument(newer).toJson(), path, &back, &error) && !error.isEmpty());
    // Hand-edited values are brought inside
    QJsonObject odd = root;
    QJsonObject fmt = odd.value("format").toObject();
    fmt["channels"] = 5;
    odd["format"] = fmt;
    CHECK(ProjectFile::fromJson(QJsonDocument(odd).toJson(), path, &back, &error) && back.format.audioChannels == 2);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("multichannel");
    testLayout();
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    testProjectFile(tmp);
    if (!Check::haveFfmpeg() || QStandardPaths::findExecutable("ffprobe").isEmpty())
        return Check::skip("ffmpeg/ffprobe nicht gefunden");
    // Every channel its own tone (amplitude 0.2), 4 s
    const auto tones = [&](const QString& file, int n, const QString& layout) {
        QStringList exprs;
        for (int c = 0; c < n; ++c) exprs << QString("0.2*sin(2*PI*%1*t)").arg(kFreq[c]);
        return Check::makeMedia(tmp.filePath(file), {"-f", "lavfi", "-i",
                                                     QString("aevalsrc=%1:s=48000:d=4:c=%2").arg(exprs.join('|'), layout),
                                                     "-c:a", "pcm_s16le"});
    };
    const QString quad = tones("quad.wav", 4, "4.0");
    const QString stereo = tones("stereo.wav", 2, "stereo");
    const QString five = tones("five.wav", 6, "5.1(side)");
    const QString bass = Check::makeMedia(tmp.filePath("bass.wav"),
                                          {"-f", "lavfi", "-i", "aevalsrc=0.2*sin(2*PI*50*t)+0.2*sin(2*PI*2000*t):s=48000:d=4",
                                           "-c:a", "pcm_s16le"});
    const QString tone1k = Check::makeMedia(tmp.filePath("tone.wav"),
                                            {"-f", "lavfi", "-i", "aevalsrc=0.2*sin(2*PI*1000*t):s=48000:d=4",
                                             "-c:a", "pcm_s16le"});
    if (!CHECK(!quad.isEmpty() && !stereo.isEmpty() && !five.isEmpty() && !bass.isEmpty() && !tone1k.isEmpty()))
        return Check::result();
    testRouting(quad);
    testSurroundMix(stereo, five, bass);
    testLoudness(tone1k);
    testExport(tmp, stereo, five);
    testCli(tmp, quad);
    return Check::result();
}
