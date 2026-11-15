// SPDX-License-Identifier: AGPL-3.0-only
// Multichannel sound (0.23, docs/plan-0.23.md Block 1), with synthetic files only (ffmpeg lavfi):
// - ChannelLayout: names, panner gains, ITU downmix, loudness weights
// - routing: a clip that plays channels 3/4 (or one channel) of a four-channel file
// - the mixer in 5.1: a stereo clip placed by the surround panner (front, surround, centre, LFE send low-passed),
//   a 5.1 file passing through channel by channel, the stereo downmix for the monitor, 7.1
// - loudness over all channels (surround +1.5 dB, LFE not counted)
// - export in 5.1/7.1 (AAC, FLAC, WAV: ffprobe channel_layout), MP3 falls back to stereo with a note
// - per-speaker meters (Mixer::channelPeaks, Player::takeLevels speakers, Engine::audioSpeakerLevels)
// - AC-3 for the MP4 formats (RenderSettings::audioFormat): 5.1(side), 7.1 as 5.1, CLI audio_codec, Deliver page
// - the project file: format 8 only when used (stereo projects unchanged), roundtrip, older versions refuse it
// - review fixes: voice cleaning of a 5.1 source (centre cleaned, LFE/surrounds aligned), a loud 5.1 mix exported as
//   stereo stays under the limiter (limiter after the downmix; loudness/check measure it with channels 'stereo'),
//   ALAC 7.1 downmixed with a note, routing dropped on a stream change, channels as JSON numbers, Project Settings
//   and the first clip's format keep the channels, the waveform of a routed clip shows its channels only
// - routed waveforms: peaks of exactly the routed channels (a 4-channel file, a stream of a multi-stream file) within
//   one step of the mixer's sound, all routings of a stream from one decode (also from disk), unrouted unchanged,
//   old/damaged per-channel cache files rebuilt. WAVE_BENCH=<file>: waveform build times of a file, then exit

#include "check.h"

#include "app/MainWindow.h"
#include "app/ProjectSettingsDialog.h"
#include "core/ChannelLayout.h"
#include "core/Project.h"
#include "core/Loudness.h"
#include "core/ProjectFile.h"
#include "core/RenderJob.h"
#include "engine/AudioAnalysis.h"
#include "engine/Engine.h"
#include "engine/MediaCache.h"
#include "engine/Exporter.h"
#include "engine/MediaProbe.h"
#include "engine/RenderQueue.h"
#include "engine/HardwareEncoders.h"
#include "engine/audio/Mixer.h"
#include "engine/audio/Reader.h"
#include "engine/audio/Player.h"
#include "ui/DeliverPanel.h"
#include "ui/RenderQueuePanel.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QDataStream>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <functional>
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
              int* written = nullptr, const QString& audioFormat = {})
{
    RenderJob job;
    job.id = 1;
    job.path = path;
    job.settings.format = format;
    job.settings.audioChannels = channels;
    job.settings.audioFormat = audioFormat;
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

// Per-speaker meters: a 5.1 file with channel c at amplitude 0.1 * (c + 1), a stereo clip panned into the surrounds
void testSpeakerMeters(const QString& levels51, const QString& stereo)
{
    const Timeline tl = oneClip(audioClip(1, levels51, 25), 6);
    for (int outCh : {6, 2}) { // the export's format and the monitor's downmix: the speakers of the mix either way
        Audio::Mixer::Options o;
        o.rate = kRate;
        o.outputChannels = outCh;
        Audio::Mixer m(tl, kFmt.rate, o);
        m.setMetering(true);
        m.setMeterBlock(outCh == 2 ? 512 : 0);
        std::vector<std::vector<float>> buf(size_t(outCh), std::vector<float>(4800));
        float* p[8];
        for (int c = 0; c < outCh; ++c) p[c] = buf[size_t(c)].data();
        m.render(0, 4800, p); // the start, then on
        m.render(4800, 4800, p);
        const std::vector<float>& peaks = m.channelPeaks();
        if (!CHECK_EQ(int(peaks.size()), 12)) continue;
        for (int c = 0; c < 6; ++c) {
            const float want = 0.1f * float(c + 1);
            if (!CHECK(std::abs(peaks[size_t(c)] - want) < 0.01f && std::abs(peaks[size_t(6 + c)] - want) < 0.01f))
                std::printf("       Ausgang %d, Kanal %d: Spur %.3f Master %.3f statt %.2f\n", outCh, c, peaks[size_t(c)],
                            peaks[size_t(6 + c)], want);
        }
        // the stereo pair as before: L the loudest of FL, C, LFE, Ls; R of FR, C, LFE, Rs
        CHECK(m.trackPeaks().size() == 2 && std::abs(m.trackPeaks()[0] - 0.5f) < 0.01f
              && std::abs(m.trackPeaks()[1] - 0.6f) < 0.01f);
        if (outCh == 2) { // per block (backwards playback): each block of 512 samples (> 1 period of 300 Hz)
            const std::vector<float>& bp = m.blockChannelPeaks();
            if (CHECK_EQ(int(bp.size()), 10 * 12))
                for (int b : {0, 5, 9})
                    for (int c = 0; c < 6; ++c)
                        CHECK(std::abs(bp[size_t(b * 12 + c)] - 0.1f * float(c + 1)) < 0.02f
                              && std::abs(bp[size_t(b * 12 + 6 + c)] - 0.1f * float(c + 1)) < 0.02f);
        }
    }
    // A stereo clip at depth 100: only the surround speakers move
    {
        Timeline st = oneClip(audioClip(1, stereo, 25), 6);
        st.audio[0].panDepth = 100;
        Audio::Mixer::Options o;
        o.rate = kRate;
        Audio::Mixer m(st, kFmt.rate, o);
        m.setMetering(true);
        std::vector<std::vector<float>> buf(6, std::vector<float>(4800));
        float* p[6];
        for (int c = 0; c < 6; ++c) p[c] = buf[size_t(c)].data();
        m.render(4800, 4800, p);
        const std::vector<float>& peaks = m.channelPeaks();
        if (CHECK_EQ(int(peaks.size()), 12))
            CHECK(peaks[0] < 1e-6f && peaks[1] < 1e-6f && peaks[2] < 1e-6f && peaks[3] < 1e-6f && peaks[4] > 0.19f
                  && peaks[5] > 0.19f && peaks[10] > 0.19f && peaks[11] > 0.19f && peaks[6] < 1e-6f);
    }
    // Stereo: L, R as the track meters
    {
        Audio::Mixer::Options o;
        o.rate = kRate;
        Audio::Mixer m(oneClip(audioClip(1, stereo, 25), 2), kFmt.rate, o);
        m.setMetering(true);
        std::vector<std::vector<float>> buf(2, std::vector<float>(4800));
        float* p[2] = {buf[0].data(), buf[1].data()};
        m.render(4800, 4800, p);
        CHECK(m.channelPeaks().size() == 4 && m.channelPeaks()[0] == m.trackPeaks()[0]
              && m.channelPeaks()[1] == m.trackPeaks()[1]);
    }
    // The player hands them on (null device): forward and backwards; nothing extra in stereo
    qputenv("SCHNEIDI_AUDIO_DEVICE", "null");
    for (const double speed : {1.0, -1.0}) {
        Audio::Player player;
        player.setTimeline(tl, kFmt.rate);
        player.play(speed > 0 ? 0.0 : 3.5 * kRate, speed);
        player.release();
        QElapsedTimer t;
        t.start();
        std::vector<float> speakers, levels;
        while (t.elapsed() < 3000 && (speakers.empty() || speakers[11] < 0.5f)) {
            QThread::msleep(30);
            std::vector<float> sp;
            const std::vector<float> lv = player.takeLevels(&sp);
            if (!lv.empty()) levels = lv;
            if (!sp.empty()) speakers = sp;
        }
        player.pause();
        if (!CHECK_EQ(int(speakers.size()), 12)) continue;
        CHECK_EQ(int(levels.size()), 4); // track L, R + master L, R as before
        for (int c = 0; c < 6; ++c)
            if (!CHECK(std::abs(speakers[size_t(6 + c)] - 0.1f * float(c + 1)) < 0.02f))
                std::printf("       Player %+.0f: Master-Kanal %d %.3f\n", speed, c, speakers[size_t(6 + c)]);
    }
    {
        Audio::Player player;
        player.setTimeline(oneClip(audioClip(1, stereo, 25), 2), kFmt.rate);
        player.play(0, 1.0);
        player.release();
        QThread::msleep(300);
        std::vector<float> sp{1.f};
        const std::vector<float> lv = player.takeLevels(&sp);
        CHECK(lv.size() == 4 && sp.empty());
        player.pause();
    }
}

// AC-3 for 5.1 in the MP4 formats, when this FFmpeg has the encoder (the bundled one since 0.23, Debian's always)
void testAc3(const QTemporaryDir& tmp, const QString& five)
{
    RenderSettings s;
    s.audioFormat = "ac3";
    CHECK(s.audioCodec() == "ac3" && !s.hasAudioBitrate() && s.summary(QSize(320, 180)).contains("AC-3"));
    CHECK(RenderSettings::fromJson(s.toJson()) == s && RenderSettings::fromJson(s.toJson()).audioFormat == "ac3");
    RenderSettings plain;
    CHECK(plain != s && !plain.toJson().contains("audioFormat"));
    s.format = "vp9"; // WebM: Opus, AC-3 not offered
    CHECK(s.audioCodec() == "libopus" && s.audioFormatChoices().isEmpty());
    s.format = "prores";
    CHECK(s.audioCodec().startsWith("pcm_"));
    if (!HardwareEncoders::builtIn("ac3")) {
        std::printf("       ÜBERSPRUNGEN: dieses FFmpeg hat keinen AC-3-Encoder\n");
        return;
    }
    Timeline tl = oneClip(audioClip(1, five, 25), 6);
    const QString path = tmp.filePath("ac3.mp4");
    int written = 0;
    if (exportTo(tl, path, "h264", 0, nullptr, &written, "ac3")) {
        const QJsonObject a = ffprobeAudio(path);
        if (!CHECK(a.value("codec_name").toString() == "ac3" && a.value("channel_layout").toString() == "5.1(side)"
                   && a.value("channels").toInt() == 6 && written == 6))
            std::printf("       AC-3: %s\n", QJsonDocument(a).toJson(QJsonDocument::Compact).constData());
        CHECK(std::abs(a.value("bit_rate").toString().toInt() - 640000) < 1000);
    }
    // 7.1 as 5.1 with a note (not stereo)
    {
        Timeline t8 = oneClip(audioClip(1, five, 25), 8);
        QString note;
        const QString p8 = tmp.filePath("ac3-71.mkv");
        if (exportTo(t8, p8, "h264", 0, &note, &written, "ac3")) {
            const QJsonObject a = ffprobeAudio(p8);
            CHECK(a.value("codec_name").toString() == "ac3" && a.value("channels").toInt() == 6 && written == 6
                  && note.contains("5.1"));
            std::printf("       AC-3 7.1: %s\n", qPrintable(note));
        }
    }
    // CLI: audio_codec ac3; refused for WebM and when the FFmpeg lacks the encoder
    const QString project = tmp.filePath("ac3.schneidi");
    CHECK(cli({"new", project, "--template", "neutral", "--media", QString("[\"%1\"]").arg(five)}).value("ok").toBool());
    CHECK(ops(project, {QJsonObject{{"op", "append"}, {"media", five}}, QJsonObject{{"op", "format"}, {"channels", "5.1"}}})
              .value("ok")
              .toBool());
    const QJsonObject r = cli({"render", project, "--out", tmp.filePath("cli-ac3.mp4"), "--audio_codec", "ac3", "--to", "1s"});
    CHECK(r.value("audio_codec").toString() == "ac3" && r.value("channels").toString() == "5.1"
          && ffprobeAudio(tmp.filePath("cli-ac3.mp4")).value("codec_name").toString() == "ac3");
    QProcess p;
    p.start(Check::cliPath(), {"render", project, "--out", tmp.filePath("cli-ac3.webm"), "--format", "vp9", "--audio_codec", "ac3"});
    p.waitForFinished(60000);
    CHECK(QJsonDocument::fromJson(p.readAllStandardOutput()).object().value("error").toObject().value("code").toString()
          == "BAD_ARGUMENT");
    QProcess q;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("SCHNEIDI_IGNORE_BUILTIN", "ac3");
    q.setProcessEnvironment(env);
    q.start(Check::cliPath(), {"render", project, "--out", tmp.filePath("cli-no-ac3.mp4"), "--audio_codec", "ac3"});
    q.waitForFinished(60000);
    const QJsonObject no = QJsonDocument::fromJson(q.readAllStandardOutput()).object();
    CHECK(!no.value("ok").toBool() && no.value("error").toObject().value("message").toString().contains("AC-3"));
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


// Planar float samples of an audio file (ffmpeg), channel by channel
std::vector<std::vector<float>> decodeFile(const QString& path, int channels)
{
    QProcess p;
    p.start("ffmpeg", {"-v", "error", "-nostdin", "-i", path, "-f", "f32le", "-"});
    p.waitForFinished(60000);
    const QByteArray d = p.readAllStandardOutput();
    const auto* f = reinterpret_cast<const float*>(d.constData());
    const size_t frames = size_t(d.size()) / sizeof(float) / size_t(channels);
    std::vector<std::vector<float>> ch(size_t(channels), std::vector<float>(frames, 0.f));
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c) ch[size_t(c)][i] = f[i * size_t(channels) + size_t(c)];
    return ch;
}

float peakOf(const std::vector<float>& x)
{
    float p = 0;
    for (float v : x) p = std::max(p, std::abs(v));
    return p;
}

// Voice cleaning of a 5.1 source: the centre (dialogue) is cleaned like the front pair, LFE and surrounds come as
// late as the cleaned channels (they were read ~57 ms early: an echo against the front)
void testVoiceClean51(const QString& voice51)
{
    Clip c = audioClip(1, voice51, 100);
    c.effects << EffectInstance{"voiceclean", {}, true};
    const auto out = renderMix(oneClip(c, 6), 0, 3 * kRate);
    if (!CHECK(out.size() == 6)) return;
    // The click on LFE and surrounds at 2 s stays at 2 s
    for (int ch : {3, 4, 5}) {
        size_t at = 0;
        for (size_t i = 0; i < out[size_t(ch)].size(); ++i)
            if (std::abs(out[size_t(ch)][i]) > std::abs(out[size_t(ch)][at])) at = i;
        if (!CHECK(std::abs(int64_t(at) - 2 * kRate) <= 1)) std::printf("       Kanal %d: Klick bei %zu statt %d\n", ch, at, 2 * kRate);
    }
    // Centre = front left (the same sound through an equal processor): cleaned and aligned
    double diff = 0, ref = 0;
    for (size_t i = 0; i < out[0].size(); ++i) {
        diff += double(out[2][i] - out[0][i]) * (out[2][i] - out[0][i]);
        ref += double(out[0][i]) * out[0][i];
    }
    if (!CHECK(ref > 1e-3 && diff < 1e-6 * ref)) std::printf("       Mitte gegen links: %.3g / %.3g\n", diff, ref);
}

// A loud 5.1 mix exported as stereo: the ITU downmix adds centre and surrounds to the front pair; the limiter runs
// after the downmix, so the file stays under its ceiling. loudness/check measure that with channels 'stereo'.
void testDownmixLimit(const QTemporaryDir& tmp, const QString& loud51)
{
    Timeline tl = oneClip(audioClip(1, loud51, 50), 6);
    tl.masterLimiter = true;
    tl.masterLimiterDb = -1.0;
    const double ceiling = std::pow(10.0, -1.0 / 20.0);
    const QString path = tmp.filePath("loud-stereo.flac");
    int written = 0;
    if (exportTo(tl, path, "flac", 2, nullptr, &written)) {
        CHECK(written == 2);
        const auto ch = decodeFile(path, 2);
        const float peak = std::max(peakOf(ch[0]), peakOf(ch[1]));
        if (!CHECK(peak > 0.5 && peak <= ceiling + 1e-3)) std::printf("       Spitze %.3f (Grenze %.3f)\n", peak, ceiling);
    }
    // The 5.1 export keeps every channel under the ceiling as before
    const QString path51 = tmp.filePath("loud-51.flac");
    if (exportTo(tl, path51, "flac", 0)) {
        const auto ch = decodeFile(path51, 6);
        float peak = 0;
        for (const auto& x : ch) peak = std::max(peak, peakOf(x));
        CHECK(peak > 0.5 && peak <= ceiling + 1e-3);
    }
    // Measured as written
    const auto stereo = AudioAnalysis::timelineLoudness(kFmt, tl, 0, 50, {}, nullptr, 2);
    if (!CHECK(stereo && stereo->truePeakDb < -0.5)) std::printf("       True Peak Stereo %.2f\n", stereo ? stereo->truePeakDb : 0.0);
    // Monitoring (no limitOutput) is unchanged: the limiter on the mix, the downmix after it goes over
    const auto monitor = renderMix(tl, kRate / 2, kRate / 2, 2);
    CHECK(peakOf(monitor[0]) > 1.2f);
}

// ALAC has no 7.1 with side surrounds (only "7.1(wide)": a front-centre pair): 5.1 downmix with a note (since the AC-3
// work: an encoder with 5.1 keeps the surrounds instead of the stereo downmix); 5.1 stays
void testAlac(const QTemporaryDir& tmp, const QString& five)
{
    Timeline tl = oneClip(audioClip(1, five, 25), 8);
    QString note;
    int written = 0;
    const QString path = tmp.filePath("alac71.m4a");
    if (exportTo(tl, path, "alac", 0, &note, &written)) {
        CHECK(written == 6 && ffprobeAudio(path).value("channels").toInt() == 6 && note.contains("5.1"));
        std::printf("       ALAC-7.1-Hinweis: %s\n", qPrintable(note));
    }
    tl.audioChannels = 6;
    const QString path51 = tmp.filePath("alac51.m4a");
    if (exportTo(tl, path51, "alac", 0, &note, &written))
        CHECK(written == 6 && ffprobeAudio(path51).value("channels").toInt() == 6);
}

// Routing a new stream lacks is dropped (none left = all channels); unknown streams keep it
void testFitRouting()
{
    MediaInfo m;
    m.path = "/x/cam.mov";
    m.hasAudio = true;
    m.audioStreams = {AudioStreamInfo{4, {}, true}, AudioStreamInfo{2, {}, true}};
    Clip c;
    c.mediaPath = m.path;
    c.sourceChannels = {2, 3};
    fitSourceChannels(c, m);
    CHECK(c.sourceChannels == QVector<int>({2, 3}));
    c.audioStream = 1;
    fitSourceChannels(c, m);
    CHECK(c.sourceChannels.isEmpty());
    c.sourceChannels = {1, 2};
    fitSourceChannels(c, m);
    CHECK(c.sourceChannels == QVector<int>({1}));
    c.audioStream = 5; // not described: kept
    c.sourceChannels = {2, 3};
    fitSourceChannels(c, m);
    CHECK(c.sourceChannels == QVector<int>({2, 3}));
}

// CLI: op audio_stream drops routing the new stream lacks; channels as JSON numbers; loudness/check channels
void testCliFixes(const QTemporaryDir& tmp, const QString& twoStreams, const QString& loud51)
{
    if (Check::cliPath().isEmpty()) return;
    const QString project = tmp.filePath("fix.schneidi");
    CHECK(cli({"new", project, "--template", "neutral"}).value("ok").toBool());
    CHECK(ops(project, {QJsonObject{{"op", "append"}, {"media", twoStreams}}}).value("ok").toBool());
    QJsonObject info = cli({"info", project});
    int id = 0;
    for (const QJsonValue& t : info.value("timeline").toObject().value("tracks").toArray())
        for (const QJsonValue& c : t.toObject().value("clips").toArray())
            if (!id) id = c.toObject().value("id").toInt();
    CHECK(id > 0);
    CHECK(ops(project, {QJsonObject{{"op", "route"}, {"clips", QJsonArray{id}}, {"channels", "3-4"}}}).value("ok").toBool());
    CHECK(ops(project, {QJsonObject{{"op", "audio_stream"}, {"clips", QJsonArray{id}}, {"stream", 1}}}).value("ok").toBool());
    ProjectData d;
    QString error;
    if (CHECK(ProjectFile::load(project, &d, &error) && !d.timeline.audio.isEmpty()))
        CHECK(d.timeline.audio[0].clips.value(0).audioStream == 1 && d.timeline.audio[0].clips.value(0).sourceChannels.isEmpty());

    // channels as numbers: format op and render
    CHECK(ops(project, {QJsonObject{{"op", "format"}, {"channels", 8}}}).value("ok").toBool());
    CHECK(cli({"info", project}).value("format").toObject().value("channels").toString() == "7.1");
    CHECK(ops(project, {QJsonObject{{"op", "format"}, {"channels", 6}}}).value("ok").toBool());
    CHECK(cli({"info", project}).value("format").toObject().value("channels").toString() == "5.1");
    CHECK(!ops(project, {QJsonObject{{"op", "format"}, {"channels", 4}}}).value("ok").toBool());
    const QString out = tmp.filePath("fix-num.flac");
    const QJsonObject r = cli({"render", project, "--out", out, "--format", "flac", "--channels", "2"});
    CHECK(r.value("channels").toString() == "stereo" && ffprobeAudio(out).value("channels").toInt() == 2);

    // loudness: the 5.1 mix with a note, the stereo downmix when asked; check the same
    const QString loud = tmp.filePath("loud.schneidi");
    CHECK(cli({"new", loud, "--template", "neutral"}).value("ok").toBool());
    CHECK(ops(loud, {QJsonObject{{"op", "format"}, {"channels", "5.1"}}, QJsonObject{{"op", "append"}, {"media", loud51}},
                     QJsonObject{{"op", "mixer"}, {"track", "master"}, {"limiter", true}, {"limiter_db", -1}}})
              .value("ok").toBool());
    const QJsonObject all = cli({"loudness", loud});
    CHECK(all.value("channels").toString() == "5.1" && !all.value("note").toString().isEmpty());
    const QJsonObject st = cli({"loudness", loud, "--channels", "stereo"});
    CHECK(st.value("channels").toString() == "stereo" && st.value("note").toString().isEmpty());
    const double tp = st.value("loudness").toObject().value("true_peak_db").toDouble(99);
    if (!CHECK(tp < -0.5)) std::printf("       loudness stereo true peak %.2f\n", tp);
    const QJsonObject chk = cli({"check", loud, "--channels", "stereo"});
    CHECK(chk.value("sound").toObject().value("channels").toString() == "stereo");
    CHECK(!cli({"loudness", loud, "--channels", "quad"}).value("ok").toBool());
}

// App: Project Settings shows and keeps the channels; the first clip's format keeps them too
void testApp(const QTemporaryDir& tmp, const QString& video)
{
    ProjectFormat f51 = kFmt;
    f51.audioChannels = 6;
    {
        ProjectSettingsDialog dlg(f51, false);
        CHECK(dlg.format().audioChannels == 6 && dlg.format() == f51); // OK without changes = the same format
        auto* box = dlg.findChild<QComboBox*>("audio_channels");
        if (CHECK(box && box->count() == 3)) {
            box->setCurrentIndex(box->findData(8));
            CHECK(dlg.format().audioChannels == 8);
            box->setCurrentIndex(box->findData(2));
            CHECK(dlg.format().audioChannels == 2);
        }
    }
    if (video.isEmpty()) return;
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return;
    MainWindow w(&engine);
    w.disableAutosave();
    Project* p = w.liveProject();
    p->setFormat(f51);
    CHECK(p->format().audioChannels == 6);
    w.importFiles({video}, true, true); // 640x360 at 30 fps: the project takes its format
    QApplication::processEvents();
    CHECK(p->format().width == 640 && p->format().rate == FrameRate({30, 1}));
    if (!CHECK(p->format().audioChannels == 6)) std::printf("       Kanäle nach dem ersten Clip: %d\n", p->format().audioChannels);
    (void)tmp;
    // Deliver page: "Audio-Codec" (AAC / AC-3) for H.264 in a 5.1 project when the FFmpeg has AC-3; not in stereo
    auto* deliver = w.findChild<DeliverPanel*>();
    auto* codec = deliver ? deliver->findChild<QComboBox*>("renderAudioFormat") : nullptr;
    if (!CHECK(codec)) return;
    RenderSettings h264 = deliver->settings();
    h264.format = "h264";
    deliver->setSettings(h264);
    const bool ac3 = HardwareEncoders::builtIn("ac3");
    CHECK_EQ(codec->isVisibleTo(deliver), ac3);
    if (ac3) {
        codec->setCurrentIndex(codec->findData(QStringLiteral("ac3")));
        CHECK(deliver->settings().audioFormat == "ac3" && deliver->settings().audioCodec() == "ac3");
        h264.format = "vp9"; // WebM: not offered, the choice is not used
        deliver->setSettings(h264);
        CHECK(!codec->isVisibleTo(deliver) && deliver->settings().audioFormat.isEmpty());
        h264.format = "h264";
        h264.audioFormat.clear();
        deliver->setSettings(h264);
        CHECK(codec->isVisibleTo(deliver) && deliver->settings().audioFormat.isEmpty());
        qputenv("SCHNEIDI_IGNORE_BUILTIN", "ac3"); // an FFmpeg without the encoder: the row goes
        deliver->setSettings(h264);
        CHECK(!codec->isVisibleTo(deliver));
        qunsetenv("SCHNEIDI_IGNORE_BUILTIN");
    }
    ProjectFormat st = p->format();
    st.audioChannels = 2;
    p->setFormat(st);
    QApplication::processEvents();
    CHECK(!codec->isVisibleTo(deliver));
}

// Waveform of a routed clip: only its channels
void testWaveform(const QString& frontOnly)
{
    QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/peaks").removeRecursively();
    MediaCache cache;
    cache.setFormat(kFmt);
    auto get = [&](const QVector<int>& channels) {
        QElapsedTimer t;
        t.start();
        std::shared_ptr<const Waveform> w = cache.waveform(frontOnly, 0, channels);
        while ((!w || !w->complete) && t.elapsed() < 60000) {
            QEventLoop loop;
            QObject::connect(&cache, &MediaCache::updated, &loop, &QEventLoop::quit);
            QTimer::singleShot(50, &loop, &QEventLoop::quit);
            loop.exec();
            w = cache.waveform(frontOnly, 0, channels);
        }
        int peak = 0;
        if (w && w->complete && !w->levels.isEmpty())
            for (quint8 v : w->levels[0]) peak = std::max(peak, int(v));
        return w && w->complete ? peak : -1;
    };
    const int all = get({}), routed = get({2, 3}), front = get({0});
    if (!CHECK(all > 50 && routed == 0 && front > 50)) std::printf("       Wellenform alle %d, 3/4 %d, 1 %d\n", all, routed, front);
}

// Routed waveforms (docs/plan-0.23.md "Wellenform je Strom/Kanal"): peaks of exactly what the clip plays, per-channel
// peaks decoded once per stream and shared by its routings, old/damaged cache files rebuilt
std::shared_ptr<const Waveform> waitWave(MediaCache& cache, const QString& path, int stream, const QVector<int>& pick)
{
    QElapsedTimer t;
    t.start();
    std::shared_ptr<const Waveform> w = cache.waveform(path, stream, pick);
    while ((!w || !w->complete) && t.elapsed() < 60000) {
        QEventLoop loop;
        QObject::connect(&cache, &MediaCache::updated, &loop, &QEventLoop::quit);
        QTimer::singleShot(20, &loop, &QEventLoop::quit);
        loop.exec();
        w = cache.waveform(path, stream, pick);
    }
    return w && w->complete ? w : nullptr;
}

// Reference: the bucket peaks of what the mixer reads for the clip (Audio::Reader with the routing, stereo)
QVector<quint8> refPeaks(const QString& path, int stream, const QVector<int>& pick)
{
    constexpr int N = Waveform::kBucketsPerFrame;
    Audio::Reader::Options ro{kRate, 2};
    for (int c : pick) ro.pick.push_back(c);
    const auto reader = Audio::Reader::open(path, stream, ro);
    if (!reader) return {};
    const FrameRate fps = kFmt.rate;
    const auto frameStart = [fps](int64_t f) { return (2 * f * kRate * fps.den + fps.num) / (2 * int64_t(fps.num)); };
    const int length = int(std::ceil(double(reader->info().end) * fps.num / (double(kRate) * fps.den)));
    const int64_t total = frameStart(length);
    std::vector<float> l(static_cast<size_t>(total)), r(static_cast<size_t>(total));
    float* out[2] = {l.data(), r.data()};
    reader->read(0, int(total), out);
    QVector<quint8> peaks(length * N, 0);
    for (int i = 0; i < length; ++i) {
        const int64_t a = frameStart(i);
        const int samples = int(frameStart(i + 1) - a);
        for (int k = 0; k < N; ++k) {
            float peak = 0.f;
            for (int64_t s = a + samples * k / N; s < a + samples * (k + 1) / N; ++s)
                peak = std::max({peak, std::abs(l[size_t(s)]), std::abs(r[size_t(s)])});
            peaks[i * N + k] = quint8(std::min(255, int(std::min(peak, 1.f) * 32767.f) >> 7));
        }
    }
    return peaks;
}

// Largest difference of a waveform's finest level to the reference (-1 = sizes differ / no waveform)
int waveDiff(const std::shared_ptr<const Waveform>& w, const QVector<quint8>& ref)
{
    if (!w || w->levels.isEmpty() || w->levels[0].size() != ref.size() || ref.isEmpty()) return -1;
    int d = 0;
    for (qsizetype i = 0; i < ref.size(); ++i) d = std::max(d, std::abs(int(w->levels[0][i]) - int(ref[i])));
    return d;
}

void testRoutedWaveforms(const QTemporaryDir& tmp)
{
    // Four channels, each its own level and slow envelope (peaks change over time and between channels)
    QStringList exprs;
    const double amp[4] = {0.1, 0.25, 0.5, 0.9};
    for (int c = 0; c < 4; ++c)
        exprs << QString("%1*sin(2*PI*%2*t)*(0.55+0.45*sin(2*PI*%3*t))").arg(amp[c]).arg(kFreq[c]).arg(0.7 * (c + 1));
    const QString quad = Check::makeMedia(
        tmp.filePath("wave-quad.wav"),
        {"-f", "lavfi", "-i", QString("aevalsrc=%1:s=48000:d=3:c=4.0").arg(exprs.join('|')).replace(",", "\\,"),
         "-c:a", "pcm_s16le"});
    // A recorder-like file: stream 0 a stereo mix, stream 1 the four channels (a camera's radio mic on 3/4)
    const QString multi = Check::makeMedia(
        tmp.filePath("wave-multi.mkv"),
        {"-f", "lavfi", "-i", "aevalsrc=0.05*sin(2*PI*440*t)|0.07*sin(2*PI*660*t):s=48000:d=3:c=stereo", "-i", quad,
         "-map", "0:a", "-map", "1:a", "-c:a", "pcm_s16le"});
    if (!CHECK(!quad.isEmpty() && !multi.isEmpty())) return;
    QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/peaks").removeRecursively();

    struct Case {
        QString path;
        int stream;
        QVector<int> pick;
    };
    const QList<Case> cases{{quad, 0, {2, 3}}, {quad, 0, {0}}, {quad, 0, {3, 1}}, {multi, 1, {2}}, {multi, 1, {0, 1}}};
    {
        MediaCache cache;
        cache.setFormat(kFmt);
        for (const Case& c : cases) {
            const int d = waveDiff(waitWave(cache, c.path, c.stream, c.pick), refPeaks(c.path, c.stream, c.pick));
            if (!CHECK(d >= 0 && d <= 1)) std::printf("       Wellenform Strom %d Kanäle %d…: Abstand %d\n", c.stream, c.pick.value(0), d);
        }
        // Three routings of the quad file and two of the multi file's stream 1: each stream decoded once
        if (!CHECK_EQ(cache.waveDecodes(), 2)) std::printf("       Dekodierungen %d\n", cache.waveDecodes());
        // The distinct channel levels really show: channel 4 (0.9) far above channel 1 (0.1)
        const auto loud = waitWave(cache, quad, 0, {3}), quiet = waitWave(cache, quad, 0, {0});
        CHECK(loud && quiet && *std::max_element(loud->levels[0].begin(), loud->levels[0].end())
                                   > 4 * *std::max_element(quiet->levels[0].begin(), quiet->levels[0].end()));
        // Unrouted streams keep their own stereo path (exact, its own decode) and the other stream is not mixed in
        CHECK_EQ(waveDiff(waitWave(cache, multi, 0, {}), refPeaks(multi, 0, {})), 0);
        CHECK_EQ(waveDiff(waitWave(cache, multi, 1, {}), refPeaks(multi, 1, {})), 0);
        CHECK_EQ(cache.waveDecodes(), 4);
        // A channel the stream lacks is silent, as in the mixer
        const auto none = waitWave(cache, quad, 0, {7});
        CHECK(none && *std::max_element(none->levels[0].begin(), none->levels[0].end()) == 0);
        CHECK_EQ(cache.waveDecodes(), 4);
    }
    {
        // A new session: the per-channel peaks come from disk, a routing never asked for before decodes nothing
        MediaCache cache;
        cache.setFormat(kFmt);
        const int d = waveDiff(waitWave(cache, quad, 0, {1}), refPeaks(quad, 0, {1}));
        CHECK(d >= 0 && d <= 1);
        CHECK_EQ(cache.waveDecodes(), 0);
    }
    // Old or damaged files where the per-channel peaks live are rebuilt, never misread
    const QString file = MediaCache::channelPeaksFile(quad, 0, kFmt.rate);
    CHECK(QFileInfo::exists(file));
    const int frames = 3 * 25;
    const auto writeFile = [&](const std::function<void(QDataStream&)>& fill) {
        QFile f(file);
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QDataStream out(&f);
        fill(out);
    };
    const QByteArray full(frames * Waveform::kBucketsPerFrame, char(255));
    const QList<std::function<void(QDataStream&)>> broken{
        [&](QDataStream& o) { o << quint32(0x53574631) << qint32(frames) << full; }, // an unrouted (SWF1) file
        [&](QDataStream& o) { o << quint32(0x53574632) << qint32(2) << qint32(4) << qint32(frames) << full.repeated(4); }, // newer version
        [&](QDataStream& o) { o << quint32(0x53574632) << qint32(1) << qint32(4) << qint32(frames) << full; }, // too short
        [&](QDataStream& o) { o << quint32(0x53574632) << qint32(1) << qint32(4); }, // cut off
    };
    for (const auto& fill : broken) {
        writeFile(fill);
        MediaCache cache;
        cache.setFormat(kFmt);
        const int d = waveDiff(waitWave(cache, quad, 0, {2, 3}), refPeaks(quad, 0, {2, 3}));
        if (!CHECK(d >= 0 && d <= 1 && cache.waveDecodes() == 1))
            std::printf("       altes Format: Abstand %d, Dekodierungen %d\n", d, cache.waveDecodes());
    }
    // The rebuilt peaks were saved again
    {
        MediaCache cache;
        cache.setFormat(kFmt);
        waitWave(cache, quad, 0, {2, 3});
        CHECK_EQ(cache.waveDecodes(), 0); // rebuilt and saved by the last round
    }
}

// Room tone of a routed take (op room_tone, AudioAnalysis::fileBlockLevels pick): measured on its channels only
void testRoutedBlockLevels(const QTemporaryDir& tmp)
{
    // Channels 1/2 loud (0.5), channels 3/4 quiet (0.01): the radio mic's floor is far below the camera's
    const QString file = Check::makeMedia(
        tmp.filePath("levels-quad.wav"),
        {"-f", "lavfi", "-i",
         "aevalsrc=0.5*sin(2*PI*300*t)|0.5*sin(2*PI*500*t)|0.01*sin(2*PI*700*t)|0.01*sin(2*PI*900*t):s=48000:d=2:c=4.0",
         "-c:a", "pcm_s16le"});
    if (!CHECK(!file.isEmpty())) return;
    const auto mean = [&](const QVector<int>& pick) {
        const auto l = AudioAnalysis::fileBlockLevels(file, 0, 0.2, 1.8, 0.1, nullptr, 0, pick);
        if (!l || l->empty()) return 1000.0;
        double sum = 0;
        for (double v : *l) sum += v;
        return sum / double(l->size());
    };
    const double all = mean({}), mic = mean({2, 3}), one = mean({2});
    // all four: power mean of 0.5 and 0.01 sines ~ -11 dB; 3/4 alone: 0.01 sine = -43 dBFS (RMS -40 - 3); one channel in
    // the middle: -3 dB each side, the same per-channel power mean
    if (!CHECK(std::abs(all - -12.0) < 1.5 && std::abs(mic - -43.0) < 0.5 && std::abs(one - -46.0) < 0.5))
        std::printf("       Blockpegel alle %.1f, 3/4 %.1f, 3 %.1f\n", all, mic, one);
}

// WAVE_BENCH=<file>: build time of its waveforms (stream 0 unrouted, then two routings of stream 0), disk cache emptied
// before each build, best of 3; prints and ends the test
int waveBench(const QString& file)
{
    const QString peaks = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/peaks";
    const auto build = [&](const QVector<QVector<int>>& routings) {
        qint64 best = -1;
        for (int run = 0; run < 3; ++run) {
            QDir(peaks).removeRecursively();
            MediaCache cache;
            cache.setFormat(ProjectFormat{1920, 1080, {60, 1}});
            QElapsedTimer t;
            t.start();
            for (const QVector<int>& r : routings) {
                std::shared_ptr<const Waveform> w = cache.waveform(file, 0, r);
                while ((!w || !w->complete) && t.elapsed() < 120000) {
                    QEventLoop loop;
                    QObject::connect(&cache, &MediaCache::updated, &loop, &QEventLoop::quit);
                    QTimer::singleShot(5, &loop, &QEventLoop::quit);
                    loop.exec();
                    w = cache.waveform(file, 0, r);
                }
            }
            const qint64 ms = t.elapsed();
            best = best < 0 ? ms : std::min(best, ms);
        }
        return best;
    };
    std::printf("waveform unrouted: %lld ms\n", (long long)build({{}}));
    std::printf("waveform routed {0} + {1}: %lld ms\n", (long long)build({{0}, {1}}));
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("multichannel");
    if (const QString b = qEnvironmentVariable("WAVE_BENCH"); !b.isEmpty()) return waveBench(b);
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
    {
        QStringList exprs;
        for (int c = 0; c < 6; ++c) exprs << QString("%1*sin(2*PI*%2*t)").arg(0.1 * (c + 1)).arg(kFreq[c]);
        const QString levels51 = Check::makeMedia(
            tmp.filePath("levels51.wav"),
            {"-f", "lavfi", "-i", QString("aevalsrc=%1:s=48000:d=4:c=5.1(side)").arg(exprs.join('|')), "-c:a", "pcm_f32le"});
        if (CHECK(!levels51.isEmpty())) testSpeakerMeters(levels51, stereo);
    }
    testExport(tmp, stereo, five);
    testAc3(tmp, five);
    testCli(tmp, quad);

    // Review fixes
    testFitRouting();
    // 5.1: front pair and centre the same "voice" (a 300 Hz tone with a slow tremolo from 0.5 s), a click on LFE and
    // the surrounds at 2 s
    const QString voice = "0.3*sin(2*PI*300*t)*(0.6+0.4*sin(2*PI*3*t))*gt(t,0.5)";
    const QString click = "if(eq(n,96000),0.8,0)";
    const QString voice51 = Check::makeMedia(
        tmp.filePath("voice51.wav"),
        {"-f", "lavfi", "-i",
         QString("aevalsrc=%1|%1|%1|%2|%2|%2:s=48000:d=4:c=5.1(side)").arg(voice, click).replace(",", "\\,"), // (filter commas)
         "-c:a", "pcm_s16le"});
    const QString loud51 = Check::makeMedia(
        tmp.filePath("loud51.wav"),
        {"-f", "lavfi", "-i", "aevalsrc=0.9*sin(2*PI*200*t)|0.9*sin(2*PI*200*t)|0.9*sin(2*PI*200*t)|0|0.9*sin(2*PI*200*t)|0.9*sin(2*PI*200*t):s=48000:d=2:c=5.1(side)",
         "-c:a", "pcm_f32le"});
    const QString frontOnly = Check::makeMedia(
        tmp.filePath("front.wav"),
        {"-f", "lavfi", "-i", "aevalsrc=0.5*sin(2*PI*300*t)|0.5*sin(2*PI*500*t)|0|0:s=48000:d=2:c=4.0", "-c:a", "pcm_s16le"});
    const QString twoStreams = Check::makeMedia(
        tmp.filePath("two.mkv"), {"-i", quad, "-i", stereo, "-map", "0:a", "-map", "1:a", "-c:a", "pcm_s16le"});
    const QString video = Check::makeMedia(
        tmp.filePath("v30.mp4"), {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30:duration=1", "-f", "lavfi", "-i",
                                  "sine=d=1", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
    if (CHECK(!voice51.isEmpty())) testVoiceClean51(voice51);
    if (CHECK(!loud51.isEmpty())) testDownmixLimit(tmp, loud51);
    testAlac(tmp, five);
    if (CHECK(!frontOnly.isEmpty())) testWaveform(frontOnly);
    testRoutedWaveforms(tmp);
    testRoutedBlockLevels(tmp);
    if (CHECK(!twoStreams.isEmpty() && !loud51.isEmpty())) testCliFixes(tmp, twoStreams, loud51);
    testApp(tmp, video);
    return Check::result();
}
