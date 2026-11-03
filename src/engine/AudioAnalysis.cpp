// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/AudioAnalysis.h"

#include "core/Loudness.h"
#include "core/TimelineOps.h"
#include "engine/ClipFormat.h"
#include "engine/audio/Mixer.h"
#include "engine/audio/Reader.h"

#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <memory>

namespace AudioAnalysis {

namespace {

constexpr int kRate = 48000;

using Progress = std::function<bool(double)>;
// Planar float audio of one timeline frame; pcm == nullptr = the frame could not be decoded
using Sink = std::function<void(const float* pcm, int samples, int channels, int freq)>;

// Own engine: the mixer's frames [from, to) one by one into sink (stereo). false = canceled.
bool renderFrames(Audio::Mixer& mixer, int from, int to, const Progress& progress, const Sink& sink)
{
    constexpr int ch = Audio::Mixer::kChannels;
    std::vector<float> buf;
    float* out[ch];
    for (int f = from; f < to; ++f) {
        if (progress && (f - from) % 25 == 0 && !progress(double(f - from) / (to - from))) return false;
        const int64_t a = mixer.frameStart(f);
        const int n = int(mixer.frameStart(f + 1) - a);
        buf.resize(size_t(n) * ch);
        for (int c = 0; c < ch; ++c) out[c] = buf.data() + size_t(c) * size_t(n);
        mixer.render(a, n, out);
        sink(buf.data(), n, ch, mixer.rate());
    }
    if (progress) progress(1.0);
    return true;
}

// Own engine: the clip's sound alone as the timeline reads it (stream, speed, reverse, ramps), without volume, pan,
// fades, transitions, effects and faders, and without the declick ramps at its edges. Frame 0 = first clip frame.
bool decodeOwn(const ProjectFormat& format, const Clip& clip, const Progress& progress, const Sink& sink)
{
    if (clip.isTitle() || clip.freeze || clip.isNested() || !QFileInfo::exists(clip.mediaPath)) return false;
    const auto reader = Audio::Reader::open(clip.mediaPath, clip.audioStream,
                                            Audio::Reader::Options{kRate, Audio::Mixer::kChannels});
    if (!reader) return false; // no such stream
    const int fileFrames = int(std::ceil(double(reader->info().end) * format.rate.fps() / kRate));
    Clip c = clip;
    c.start = 0;
    c.subframe = 0;
    c.volumeDb = 0;
    c.pan = 0;
    c.keys.remove(AnimParam::Volume);
    c.keys.remove(AnimParam::Pan);
    c.fadeIn = c.fadeOut = 0;
    c.transIn = c.transOut = 0;
    c.effects.clear();
    c.enabled = true;
    // Up to the end of the material
    if (const int len = c.retimedLength(fileFrames); len > 0) c.out = std::min(c.out, len - 1);
    if (c.out < c.in) return false;
    Timeline tl;
    tl.audio.resize(1);
    tl.audio[0].clips << c;
    tl.masterLimiter = false;
    Audio::Mixer::Options o;
    o.rate = kRate;
    o.declick = false;
    o.fileLength = [fileFrames](const QString&) { return fileFrames; };
    Audio::Mixer mixer(tl, format.rate, o);
    return renderFrames(mixer, 0, c.length(), progress, sink) && mixer.stats().missing == 0;
}

bool decode(const ProjectFormat& format, const Clip& clip, const Progress& progress, const Sink& sink)
{
    return decodeOwn(format, clip, progress, sink);
}

// The mix of timeline frames [from, to) as it is exported, frame by frame. false = could not mix or canceled.
bool mix(const ProjectFormat& format, const Timeline& timeline, int from, int to, const Progress& progress,
         const Sink& sink)
{
    Audio::Mixer::Options o;
    o.rate = kRate;
    Audio::Mixer mixer(timeline, format.rate, o);
    return renderFrames(mixer, from, to, progress, sink);
}

float framePeak(const float* pcm, int samples, int channels)
{
    float peak = 0.f;
    if (pcm)
        for (int i = 0, n = samples * channels; i < n; ++i) peak = std::max(peak, std::abs(pcm[i]));
    return peak;
}

double toDb(float peak) { return peak > 1e-10f ? 20.0 * std::log10(double(peak)) : -200.0; }

// Loudness of planar float audio blocks (meters fed by measure())
struct Measure {
    LoudnessMeter meter;
    TruePeakMeter truePeak;
    float peak = 0.f;
    double maxMomentary = LoudnessMeter::kSilence, maxShortTerm = LoudnessMeter::kSilence;
    void add(const float* pcm, int samples, int channels, int freq)
    {
        if (!pcm) return;
        meter.addPlanar(pcm, samples, channels, freq);
        truePeak.addPlanar(pcm, samples, channels, freq);
        peak = std::max(peak, framePeak(pcm, samples, channels));
        maxMomentary = std::max(maxMomentary, meter.momentary());
        maxShortTerm = std::max(maxShortTerm, meter.shortTerm());
    }
    Loudness result() const
    {
        Loudness l;
        l.integrated = meter.integrated();
        l.range = meter.range();
        l.peakDb = toDb(peak);
        l.truePeakDb = truePeak.maxDb();
        l.maxMomentary = maxMomentary;
        l.maxShortTerm = maxShortTerm;
        l.blocks = meter.blocks();
        return l;
    }
};

} // namespace

std::optional<double> clipPeakDb(const ProjectFormat& format, const Clip& clip,
                                 const std::function<bool(double)>& progress)
{
    float peak = 0.f;
    const bool ok = decode(format, clip, progress, [&](const float* pcm, int samples, int channels, int) {
        peak = std::max(peak, framePeak(pcm, samples, channels));
    });
    if (!ok) return std::nullopt;
    return toDb(peak);
}

std::optional<ClipLevels> measureClips(const ProjectFormat& format, const Timeline& timeline, const QVector<int>& ids,
                                       bool loudness, const std::function<bool(double)>& progress)
{
    ClipLevels out;
    std::vector<double> blocks; // 400 ms blocks of all clips -> their loudness together
    for (int i = 0; i < ids.size(); ++i) {
        const Clip* c = TimelineOps::findClip(timeline, ids[i]);
        if (!c) continue;
        bool go = true;
        const auto step = [&](double f) {
            if (progress && !progress((i + f) / ids.size())) go = false;
            return go;
        };
        if (loudness) {
            if (const auto l = clipLoudness(format, *c, step)) {
                out.levels[c->id] = l->integrated;
                out.truePeaks[c->id] = l->truePeakDb;
                blocks.insert(blocks.end(), l->blocks.begin(), l->blocks.end());
            }
        } else if (const auto peak = clipPeakDb(format, *c, step)) {
            out.levels[c->id] = *peak;
        }
        if (!go) return std::nullopt;
    }
    if (loudness) out.shared = LoudnessMeter::integratedOf(blocks);
    return out;
}

std::optional<Loudness> clipLoudness(const ProjectFormat& format, const Clip& clip,
                                     const std::function<bool(double)>& progress)
{
    Measure m;
    const bool ok = decode(format, clip, progress, [&](const float* pcm, int samples, int channels, int freq) {
        m.add(pcm, samples, channels, freq);
    });
    if (!ok) return std::nullopt;
    return m.result();
}

std::optional<Loudness> timelineLoudness(const ProjectFormat& format, const Timeline& timeline, int from, int to,
                                         const std::function<bool(double)>& progress, std::vector<float>* framePeaks)
{
    return MixMeter(format).measure(timeline, from, to, progress, framePeaks);
}

MixMeter::MixMeter(const ProjectFormat& format) : m_format(format) {}

MixMeter::~MixMeter() = default;

std::optional<Loudness> MixMeter::measure(const Timeline& timeline, int from, int to,
                                          const std::function<bool(double)>& progress, std::vector<float>* framePeaks)
{
    if (to <= from) return std::nullopt;
    Measure m;
    if (framePeaks) {
        framePeaks->clear();
        framePeaks->reserve(size_t(to - from));
    }
    const Sink sink = [&](const float* pcm, int samples, int channels, int freq) {
        m.add(pcm, samples, channels, freq);
        if (framePeaks) framePeaks->push_back(framePeak(pcm, samples, channels));
    };
    if (!mix(m_format, timeline, from, to, progress, sink)) return std::nullopt;
    return m.result();
}

std::optional<std::vector<float>> clipFramePeaks(const ProjectFormat& format, const Clip& clip,
                                                 const std::function<bool(double)>& progress)
{
    std::vector<float> peaks;
    const bool ok = decode(format, clip, progress, [&](const float* pcm, int samples, int channels, int) {
        peaks.push_back(framePeak(pcm, samples, channels));
    });
    if (!ok) return std::nullopt;
    return peaks;
}

std::optional<std::vector<float>> timelineFramePeaks(const ProjectFormat& format, const Timeline& timeline, int from,
                                                     int to, const std::function<bool(double)>& progress)
{
    if (to <= from) return std::nullopt;
    std::vector<float> peaks;
    peaks.reserve(size_t(to - from));
    const bool ok = mix(format, timeline, from, to, progress, [&](const float* pcm, int samples, int channels, int) {
        peaks.push_back(framePeak(pcm, samples, channels));
    });
    if (!ok) return std::nullopt;
    return peaks;
}

bool clipSound(const ProjectFormat& format, const Clip& clip, const SoundSink& sink,
               const std::function<bool(double)>& progress)
{
    return decode(format, clip, progress, sink);
}

bool timelineSound(const ProjectFormat& format, const Timeline& timeline, int from, int to, const SoundSink& sink,
                   const std::function<bool(double)>& progress)
{
    return to > from && mix(format, timeline, from, to, progress, sink);
}

std::optional<Ducking::Ranges> soundRanges(const ProjectFormat& format, const Timeline& timeline, const QVector<int>& under,
                                           const Ducking::Params& params, const std::function<bool(double)>& progress)
{
    Timeline tl = timeline;
    bool any = false;
    for (int i = 0; i < tl.audio.size(); ++i) {
        tl.audio[i].muted = !under.contains(i);
        tl.audio[i].solo = false;
        any = any || (!tl.audio[i].muted && !tl.audio[i].clips.isEmpty());
    }
    const int end = TimelineOps::endFrame(tl);
    if (!any || end <= 0) return Ducking::Ranges{};
    const auto peaks = timelineFramePeaks(format, tl, 0, end, progress);
    if (!peaks) return std::nullopt;
    return Ducking::soundRanges(*peaks, 0, params);
}

std::optional<std::vector<double>> fileBlockLevels(const QString& path, int stream, double from, double to,
                                                   double blockSeconds)
{
    const auto reader = Audio::Reader::open(path, stream, Audio::Reader::Options{kRate, 0});
    if (!reader) return std::nullopt;
    const int ch = std::max(1, reader->channels());
    const int block = std::max(1, int(std::lround(blockSeconds * kRate)));
    const int64_t start = int64_t(std::llround(std::max(0.0, from) * kRate));
    const int64_t end = int64_t(std::llround(std::max(from, to) * kRate));
    std::vector<float> buf(size_t(block) * ch);
    std::vector<float*> out(ch);
    for (int c = 0; c < ch; ++c) out[c] = buf.data() + size_t(c) * block;
    std::vector<double> levels;
    for (int64_t pos = start; pos + block <= end; pos += block) {
        reader->read(pos, block, out.data());
        double sum = 0;
        for (float v : buf) sum += double(v) * v;
        const double ms = sum / (double(block) * ch);
        levels.push_back(ms > 1e-20 ? 10 * std::log10(ms) : -200.0);
    }
    return levels;
}

} // namespace AudioAnalysis
