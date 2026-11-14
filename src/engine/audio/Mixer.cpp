// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/audio/Mixer.h"

#include "core/Automation.h"
#include "core/ChannelLayout.h"
#include "core/Dynamics.h"
#include "core/EffectRegistry.h"
#include "core/Equalizer.h"
#include "core/Keyframes.h"
#include "core/Limiter.h"
#include "core/Multicam.h"
#include "core/TimelineOps.h"
#include "engine/Denoise.h"
#include "engine/VoiceClean.h"
#include "engine/audio/Reader.h"
#include "engine/audio/Retimer.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <climits>
#include <cmath>
#include <vector>

namespace Audio {

namespace {

constexpr int kMaxNesting = 16;
constexpr double kMaxGainDb = 20; // clip volumes stay below anyway
constexpr int kGlide = 480;       // a gain changed live glides over 10 ms (no zipper click)

int64_t floorDiv(int64_t a, int64_t b)
{
    const int64_t q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

double dbToGain(double db)
{
    if (db <= kMinVolumeDb) return 1e-10; // -inf: -200 dB, practically silent
    return std::pow(10.0, std::min(db, kMaxGainDb) / 20.0);
}

// Sound of the timeline apart from what the mixer panel changes (Mixer::updateMix): the clips of the audio tracks and
// the nested sequences
bool sameSound(const Timeline& a, const Timeline& b)
{
    if (a.audio.size() != b.audio.size() || a.masterLimiter != b.masterLimiter || a.audioChannels != b.audioChannels)
        return false;
    for (int i = 0; i < a.audio.size(); ++i) {
        Track x = a.audio[i], y = b.audio[i];
        for (Track* t : {&x, &y}) {
            t->volumeDb = t->pan = 0;
            t->muted = t->solo = false;
            t->eq = {};
            t->dynamics = {};
            t->panDepth = t->panCenter = 0;
            t->lfeDb = kMinVolumeDb;
            t->volumeAuto.clear(); // automation is a gain like the fader (glides when changed live)
        }
        if (!(x == y)) return false;
    }
    return TimelineOps::sameNested(a, b);
}

// Gain of a curve value (fades, transitions): below 0.001 silent (-200 dB)
double curveGain(double g) { return g > 0.001 ? g : 1e-10; }

// Frame <-> sample: frame f starts at round(f * rate / fps) with the rational frame rate (no drift at 29.97)
struct Clock {
    int rate = 48000;
    FrameRate fps;

    int64_t start(int64_t f) const
    {
        const int64_t n = f * rate * fps.den;
        return floorDiv(2 * n + fps.num, 2 * int64_t(fps.num));
    }
    int64_t frameAt(int64_t s) const
    {
        int64_t f = int64_t(std::floor(double(s) * fps.fps() / rate));
        while (start(f + 1) <= s) ++f;
        while (start(f) > s) --f;
        return f;
    }
    // Samples of a fraction of a frame (sub-frame offsets)
    int64_t fraction(double frames) const { return std::lround(frames * rate / fps.fps()); }
};

// One gain or pan curve over timeline frames [from, to): v holds the values at the frame boundaries from..to, a frame
// runs linearly from its start value to its end value.
struct Stage {
    enum Kind { Gain, Pan } kind = Gain;
    int64_t from = 0, to = 0;
    std::vector<double> v; // one entry: constant
    // Value at the start and the end of frame k
    std::pair<double, double> at(int64_t k) const
    {
        if (v.size() == 1) return {v[0], v[0]};
        const size_t i = size_t(std::clamp<int64_t>(k - from, 0, int64_t(v.size()) - 1));
        return {v[i], v[std::min(i + 1, v.size() - 1)]};
    }
};

// The sound of one clip on a track
struct Voice {
    Clip clip;
    // Source: a file stream, or the mix of a sequence (compound clip, angle of a multicam clip)
    QString path;
    int stream = 0;
    const Timeline* sequence = nullptr;
    Timeline angleContent; // multicam: the angle's sound tracks
    bool multicam = false;

    int64_t uStart = 0, uEnd = 0; // samples on the timeline before the sub-frame offset
    int64_t shift = 0;            // sub-frame offset in samples
    int64_t tEnd = 0;             // last sample (exclusive) on the timeline after the offset
    int64_t sourceOffset = 0;     // source sample = u + sourceOffset
    std::vector<Stage> stages;
    int64_t declickIn = -1, declickOut = -1; // first sample of the clip's first frame / end of its last frame
    int declickInLen = 0, declickOutLen = 0;
    bool denoise = false;
    double denoiseReduction = 0, denoiseFloor = 0;
    bool denoiseTrack = true;
    bool clean = false; // voice cleaning (engine/VoiceClean), after the noise reduction
    VoiceClean::Params cleanParams;
    bool retimed = false; // speed/reverse/ramp (Retimer)
    int fileFrames = 0;

    int64_t tStart() const { return uStart + shift; }
};

struct TrackPlan {
    std::vector<Voice> voices; // sorted by tStart
    bool audible = true;
    bool planned = false; // voices planned (a track that was silent when planned has none)
    TrackEq eq;
    TrackDynamics dynamics;
    double gain = 1.0;
    double pan = 0.0;
    double panDepth = 0.0, panCenter = 0.0, lfeDb = kMinVolumeDb; // surround panner (5.1/7.1 mix)
    // Volume automation (Track::volumeAuto): with a curve the fader is part of it (gain stays 1), applied sample by
    // sample before the pan (Impl::applyAutomation)
    AutoCurve autoCurve;
    double faderDb = 0.0;
};

// LFE send: 2nd-order Butterworth low pass at 120 Hz
struct LowPass {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    void design(int rate)
    {
        const double k = std::tan(M_PI * 120.0 / rate), q = M_SQRT1_2;
        const double norm = 1 / (1 + k / q + k * k);
        b0 = k * k * norm;
        b1 = 2 * b0;
        b2 = b0;
        a1 = 2 * (k * k - 1) * norm;
        a2 = (1 - k / q + k * k) * norm;
    }
};

// Planar conversion of `count` samples from `from` channels to `to` (ChannelLayout::conversion)
void convertChannels(const float* const* in, int from, float* const* out, int to, int count)
{
    const ChannelLayout::Matrix k = ChannelLayout::conversion(from, to);
    for (int o = 0; o < to; ++o)
        for (int i = 0; i < count; ++i) {
            double y = 0;
            for (int c = 0; c < from; ++c)
                if (const double g = k[size_t(o)][size_t(c)]; g != 0.0) y += g * in[c][i];
            out[o][i] = float(y);
        }
}

} // namespace

struct Mixer::Impl {
    Options opt;
    Clock clock;
    int nch = 2;    // channels of the mix
    int outCh = 2;  // channels render() writes (outermost mixer)
    // > 0: the master limiter runs on the mix converted to this many channels (Options::limitOutput with a downmix);
    // Impl::render then writes these channels
    int limitCh = 0;
    LowPass lfeLowPass;
    std::shared_ptr<const NestedTimelines> nested;
    Stats* stats = nullptr; // the outermost mixer's
    Stats ownStats;
    int depth = 0;
    QSet<int> stack; // sequences being planned (no loops)
    QHash<QString, int> lengths; // file lengths taken from the files (frames)

    Timeline planned; // what plan() got (updateMix)
    std::vector<TrackPlan> tracks;
    double masterGain = 1.0;
    bool limiter = false;
    double limiterDb = 0;
    int64_t endSample = 0;

    // Runtime
    struct VoiceState {
        std::unique_ptr<Reader> reader;
        std::unique_ptr<Retimer> retimer;
        std::unique_ptr<Impl> sub; // nested sequence
        bool opened = false;
        int64_t nextU = INT64_MIN; // where the voice continues (u); anything else starts it over
        std::vector<std::unique_ptr<Denoise::Processor>> denoisers;
        std::unique_ptr<VoiceClean::Processor> cleaner;
        // 5.1/7.1 source: the centre (dialogue) cleaned by its own processor; LFE and surrounds delayed by the
        // cleaner's latency (cleanDelay[c - 3]), so every channel stays aligned with the cleaned front
        std::unique_ptr<VoiceClean::Processor> centreCleaner;
        std::vector<std::vector<float>> cleanDelay;
        int channels = 0; // what the source gives (2 or the mix's), known once opened
    };
    struct TrackState {
        Equalizer eq;
        Compressor comp;
        std::vector<VoiceState> voices;
        size_t firstVoice = 0; // voices before it have ended
        // Gain per channel (fader, pan, mute) applied so far; a new value glides there (updateMix). Stereo: L, R.
        // 5.1/7.1: the panner matrix (output o from left/right at [o * 2 + i]), then the LFE send, then the gain of
        // the channels that pass through.
        std::vector<double> applied;
        bool levelSet = false;
        double lfeZ1 = 0, lfeZ2 = 0; // LFE send low pass
        // Automation: the gain of the curve at the last sample (1 = none), and whether a live change glides from it
        double autoLast = 1.0;
        bool autoGlide = false;
    };
    std::vector<TrackState> trackStates;
    PeakLimiter peakLimiter;
    double masterApplied = 1.0;
    bool metering = false;
    std::vector<float> peaks; // per track L, R of the last render (metering)
    std::vector<float> converted; // Mixer::render with outputChannels != channels: the mix before the conversion
    int meterBlock = 0;            // setMeterBlock
    std::vector<float> blockPeaks; // peaks per stretch of meterBlock samples of the last render
    std::vector<float> chPeaks;      // per track and channel, then the master's channels (Mixer::channelPeaks)
    std::vector<float> chBlockPeaks; // the same per stretch of meterBlock samples (Mixer::blockChannelPeaks)
    void meterMaster(const std::vector<std::vector<float>>& master, int count);
    int chunkWarm = -1;            // setChunked: >= 0 = a jump keeps filters and processors
    int64_t expected = INT64_MIN; // next render position
    int latency = 0;              // master (limiter), compensated
    std::vector<std::vector<float>> mix, track, voice;
    std::vector<std::vector<float>> post; // limitCh > 0: the converted mix the limiter works on
    std::vector<float> post2;             // limitCh > 0, mono output: the limited stereo downmix
    std::vector<float> scratch;
    std::vector<float> centreCopy, delayed; // voice cleaning of a 5.1/7.1 source (renderVoice)

    Impl(const Options& o, FrameRate fps, int channels) : opt(o), nch(ChannelLayout::valid(channels))
    {
        clock = {o.rate, fps};
        mix.resize(size_t(nch));
        track.resize(size_t(nch));
        voice.resize(size_t(nch));
        if (nch > 2) lfeLowPass.design(o.rate);
    }

    // The gains a track's fader, pan and mute ask for (TrackState::applied)
    std::vector<double> trackTarget(const TrackPlan& tp) const;
    // Volume automation on track[0..nch) for [pos, pos + count): gains at the frame boundaries, linear in between
    double autoGain(const TrackPlan& tp, int64_t frame) const;
    void applyAutomation(const TrackPlan& tp, TrackState& ts, int64_t pos, int count);
    void open(Voice& v, VoiceState& st);

    int fileLength(const QString& path);
    void plan(const Timeline& tl);
    void applyMixValues(const Timeline& tl);
    void planTrack(const Track& track, TrackPlan& out);
    void renderMix(int64_t pos, int count);
    void renderVoice(Voice& v, VoiceState& st, int64_t u, int count, float* const* out);
    bool readSource(Voice& v, VoiceState& st, int64_t u, int count, float* const* out);
    void restart();
    void continueChunk();
    void render(int64_t pos, int count, float* const* out);
};

int Mixer::Impl::fileLength(const QString& path)
{
    if (opt.fileLength)
        if (const int n = opt.fileLength(path); n > 0) return n;
    if (const auto it = lengths.constFind(path); it != lengths.cend()) return *it;
    auto r = Reader::open(path, 0, Reader::Options{opt.rate, 2});
    const int n = r ? int(std::ceil(double(r->info().end) * clock.fps.fps() / opt.rate)) : 0;
    lengths.insert(path, n);
    return n;
}

void Mixer::Impl::plan(const Timeline& tl)
{
    planned = tl;
    tracks.resize(size_t(tl.audio.size()));
    applyMixValues(tl);
    for (int i = 0; i < tl.audio.size(); ++i) {
        TrackPlan& p = tracks[size_t(i)];
        if (p.audible) planTrack(tl.audio[i], p);
        p.planned = p.audible;
    }
    limiter = tl.masterLimiter;
    masterApplied = masterGain;
    endSample = clock.start(TimelineOps::endFrame(tl));
}

// What the mixer panel changes: fader, pan, mute/solo, EQ, compressor, master fader, limiter ceiling
void Mixer::Impl::applyMixValues(const Timeline& tl)
{
    const bool anySolo = std::any_of(tl.audio.begin(), tl.audio.end(), [](const Track& t) { return t.solo; });
    for (int i = 0; i < tl.audio.size(); ++i) {
        const Track& t = tl.audio[i];
        TrackPlan& p = tracks[size_t(i)];
        p.audible = !t.muted && (!anySolo || t.solo);
        p.eq = t.eq;
        p.dynamics = t.dynamics;
        p.gain = t.volumeDb == 0.0 || !t.volumeAuto.isEmpty() ? 1.0 : dbToGain(t.volumeDb);
        p.autoCurve = t.volumeAuto;
        p.faderDb = t.volumeDb;
        p.pan = t.pan / 100.0;
        p.panDepth = t.panDepth;
        p.panCenter = t.panCenter;
        p.lfeDb = t.lfeDb;
    }
    masterGain = tl.masterVolumeDb == 0.0 ? 1.0 : dbToGain(tl.masterVolumeDb);
    limiterDb = std::clamp(tl.masterLimiterDb, -40.0, 0.0);
}

void Mixer::Impl::planTrack(const Track& track, TrackPlan& out)
{
    const TimelineOps::SourceLength srcLen = [&](const Clip& c) {
        if (c.isNested()) {
            if (!nested) return 0;
            const auto it = nested->constFind(c.sequenceId);
            return it == nested->cend() ? 0 : TimelineOps::endFrame(*it);
        }
        return c.mediaPath.isEmpty() ? 0 : c.retimedLength(fileLength(c.mediaPath));
    };
    const QVector<TimelineOps::TransitionSpan> spans = TimelineOps::transitions(track, srcLen);

    // Hard cuts get a short ramp against the click: clip edges without a fade
    // that do not continue seamlessly (split clip)
    auto continues = [](const Clip& a, const Clip& b) {
        return !a.mediaPath.isEmpty() && a.mediaPath == b.mediaPath && a.audioStream == b.audioStream
            && a.sourceChannels == b.sourceChannels && a.enabled
            && b.enabled && b.start == a.end() && b.in == a.out + 1 && a.speed == b.speed && !a.reverse && !b.reverse
            && a.ramp.isEmpty() && b.ramp.isEmpty();
    };

    for (int ci = 0; ci < track.clips.size(); ++ci) {
        const Clip& c = track.clips[ci];
        if (!c.enabled || c.isTitle() || c.freeze) continue; // a freeze frame is silent (like DaVinci)
        Voice v;
        v.clip = c;
        if (c.isNested()) {
            if (!nested || depth + 1 >= kMaxNesting || stack.contains(c.sequenceId)) continue;
            const auto it = nested->constFind(c.sequenceId);
            if (it == nested->cend()) continue;
            if (c.isMulticam()) {
                const int angle = std::max(0, c.angle);
                if (angle >= it->angles.size()) continue;
                v.angleContent = Multicam::angleTimeline(*it, it->angles[angle], TrackKind::Audio);
                if (v.angleContent.audio.isEmpty()) continue;
                v.multicam = true;
            }
            v.sequence = &*it;
        } else {
            if (c.mediaPath.isEmpty()) continue;
            v.path = c.mediaPath;
            v.stream = c.audioStream;
            v.retimed = c.isRetimed();
            if (v.retimed) v.fileFrames = fileLength(c.mediaPath);
        }

        // Excerpts of the clip: transition in, body, transition out
        struct Excerpt { int from, to; const TimelineOps::TransitionSpan* span; };
        QVector<Excerpt> excerpts;
        int bodyStart = c.start, bodyEnd = c.end();
        for (const auto& s : spans) {
            if (s.rightId == c.id) bodyStart = std::max(bodyStart, s.end);
            if (s.leftId == c.id) bodyEnd = std::min(bodyEnd, s.start);
        }
        for (const auto& s : spans)
            if (s.rightId == c.id) excerpts.push_back({s.start, s.end, &s});
        if (bodyEnd > bodyStart) excerpts.push_back({bodyStart, bodyEnd, nullptr});
        for (const auto& s : spans)
            if (s.leftId == c.id) excerpts.push_back({s.start, s.end, &s});
        std::sort(excerpts.begin(), excerpts.end(), [](const Excerpt& a, const Excerpt& b) { return a.from < b.from; });
        // Material before the source start does not exist (handles of a transition that reach before frame 0)
        while (!excerpts.isEmpty() && c.in + (excerpts.first().from - c.start) < 0) excerpts.removeFirst();
        if (excerpts.isEmpty()) continue;

        const int first = excerpts.first().from, last = excerpts.last().to;
        v.uStart = clock.start(first);
        v.uEnd = clock.start(last);
        v.sourceOffset = clock.start(c.in + (first - c.start)) - v.uStart;
        v.shift = c.subframe > 0 ? clock.fraction(c.subframe / 100.0) : 0;
        v.tEnd = v.uEnd + v.shift;

        const int length = c.length();
        const int fi = std::min(c.fadeIn, length), fo = std::min(c.fadeOut, length - fi);
        const bool volumeKeys = Keys::animated(c, AnimParam::Volume), panKeys = Keys::animated(c, AnimParam::Pan);
        auto addCurve = [&](Stage::Kind kind, int from, int to, const auto& value) {
            Stage s;
            s.kind = kind;
            s.from = from;
            s.to = to;
            // values at the boundaries from..to
            const int n = to - from + 1;
            s.v.resize(size_t(n));
            for (int i = 0; i < n; ++i) s.v[size_t(i)] = value(from + i);
            v.stages.push_back(std::move(s));
        };
        auto addConstant = [&](Stage::Kind kind, int from, int to, double value) {
            v.stages.push_back(Stage{kind, from, to, {value}});
        };
        // Fade handles: 0 at the clip edge, 1 at the end of the fade
        auto fadeGain = [&](int k) { // k: frame boundary on the timeline
            const int t = k - c.start;
            double g = 1.0;
            if (fi > 0 && t < fi) g = std::min(g, double(t) / fi);
            if (fo > 0 && t > length - fo) g = std::min(g, double(length - t) / fo);
            return curveGain(std::sin(std::clamp(g, 0.0, 1.0) * M_PI / 2));
        };
        auto transitionGain = [&](const TimelineOps::TransitionSpan& s, int k) {
            const int len = std::max(1, s.length());
            const double i = k - s.start;
            const double t = std::clamp(c.id == s.rightId ? i / len : (len - i) / len, 0.0, 1.0);
            switch (s.style.audio) {
            case AudioCurve::Zero: return curveGain(t);
            case AudioCurve::Minus3dB: return curveGain(t * std::sqrt(t));
            default: return curveGain(std::sin(t * M_PI / 2));
            }
        };
        for (const Excerpt& e : excerpts) {
            if (volumeKeys)
                addCurve(Stage::Gain, e.from, e.to, [&](int k) {
                    const double db = Keys::valueAt(c, AnimParam::Volume, k - c.start);
                    return dbToGain(db);
                });
            else if (c.volumeDb != 0.0)
                addConstant(Stage::Gain, e.from, e.to, dbToGain(c.volumeDb));
            if (panKeys)
                addCurve(Stage::Pan, e.from, e.to, [&](int k) { return Keys::valueAt(c, AnimParam::Pan, k - c.start) / 100.0; });
            else if (c.pan != 0.0)
                addConstant(Stage::Pan, e.from, e.to, c.pan / 100.0);
            const int a = e.from - c.start, b = e.to - c.start;
            if (!((fi <= 0 || a >= fi) && (fo <= 0 || b <= length - fo))) addCurve(Stage::Gain, e.from, e.to, fadeGain);
            if (e.span) addCurve(Stage::Gain, e.from, e.to, [&](int k) { return transitionGain(*e.span, k); });
        }
        // Declick: at a clip edge where no transition takes over
        const int64_t ramp = std::max(1, opt.rate / 250); // ~4 ms
        auto edge = [&](bool atStart) -> const Excerpt* {
            const Excerpt& e = atStart ? excerpts.first() : excerpts.last();
            return !e.span && (atStart ? e.from == c.start : e.to == c.end()) ? &e : nullptr;
        };
        if (opt.declick && c.fadeIn <= 0 && (ci == 0 || !continues(track.clips[ci - 1], c)))
            if (edge(true)) {
                v.declickIn = clock.start(c.start);
                v.declickInLen = int(std::min(ramp, clock.start(c.start + 1) - v.declickIn));
            }
        if (opt.declick && c.fadeOut <= 0 && (ci + 1 == track.clips.size() || !continues(c, track.clips[ci + 1])))
            if (edge(false)) {
                v.declickOut = clock.start(c.end());
                v.declickOutLen = int(std::min(ramp, v.declickOut - clock.start(c.end() - 1)));
            }
        if (const EffectInstance* e = EffectRegistry::instance(c, Denoise::EffectId); e && e->enabled) {
            // in hundredths (as the project values are rounded)
            auto hundredths = [&](const char* key) {
                return std::lround(EffectRegistry::value(c, Denoise::EffectId, key).toDouble() * 100.0) / 100.0;
            };
            v.denoise = true;
            v.denoiseReduction = hundredths("reduction");
            v.denoiseFloor = hundredths("floor");
            v.denoiseTrack = EffectRegistry::value(c, Denoise::EffectId, "track").toBool();
        }
        if (const EffectInstance* e = EffectRegistry::instance(c, VoiceClean::EffectId); e && e->enabled) {
            auto value = [&](const char* key) { return EffectRegistry::value(c, VoiceClean::EffectId, key); };
            v.clean = true;
            v.cleanParams.amount = std::lround(value("amount").toDouble() * 100.0) / 10000.0; // percent, hundredths
            v.cleanParams.gate = value("gate").toBool();
            v.cleanParams.clicks = value("clicks").toBool();
            v.cleanParams.shape = value("shape").toBool();
            v.cleanParams.roomDb = std::lround(value("room").toDouble() * 10.0) / 10.0;
        }
        ++stats->voices;
        out.voices.push_back(std::move(v));
    }
    std::stable_sort(out.voices.begin(), out.voices.end(),
                     [](const Voice& a, const Voice& b) { return a.tStart() < b.tStart(); });
}

// After a jump: filters start empty; open sources stay (they read any position), every voice starts over
void Mixer::Impl::restart()
{
    trackStates.resize(tracks.size());
    for (size_t i = 0; i < tracks.size(); ++i) {
        TrackState& ts = trackStates[i];
        ts.voices.resize(tracks[i].voices.size());
        for (VoiceState& v : ts.voices) {
            v.nextU = INT64_MIN;
            v.denoisers.clear();
            v.cleaner.reset();
            v.centreCleaner.reset();
            v.cleanDelay.clear();
        }
        ts.firstVoice = 0;
        ts.levelSet = false;
        ts.autoLast = 1.0;
        ts.autoGlide = false;
        ts.lfeZ1 = ts.lfeZ2 = 0;
        ts.eq = Equalizer{};
        ts.eq.setEq(tracks[i].eq);
        ts.comp = Compressor{};
        ts.comp.setParams(tracks[i].dynamics);
    }
    peakLimiter = PeakLimiter{};
    peakLimiter.setCeilingDb(limiterDb);
    peakLimiter.prepare(limitCh > 0 ? limitCh : nch, opt.rate);
    latency = limiter ? peakLimiter.latency() : 0;
    masterApplied = masterGain;
}

// Backwards (setChunked): the next chunk lies before the one mixed last. Every voice starts over at its new
// position, but the track EQ and compressor, the limiter and the voices' noise reduction and voice cleaning go on with
// what they have: the level of the neighbouring chunk instead of a reset every chunk.
void Mixer::Impl::continueChunk()
{
    for (TrackState& ts : trackStates) {
        for (VoiceState& v : ts.voices) v.nextU = INT64_MIN;
        ts.firstVoice = 0; // earlier voices are ahead again
    }
}

void Mixer::Impl::open(Voice& v, VoiceState& st)
{
    if (st.opened) return;
    st.opened = true;
    st.channels = nch;
    if (v.sequence) {
        auto sub = std::make_unique<Impl>(opt, clock.fps, nch); // a nested sequence mixes in the same format
        sub->outCh = nch;
        sub->nested = nested;
        sub->stats = stats;
        sub->depth = depth + 1;
        sub->stack = stack;
        sub->stack.insert(v.clip.sequenceId);
        sub->plan(v.multicam ? v.angleContent : *v.sequence);
        st.sub = std::move(sub);
    } else {
        QString error;
        // Stereo: as always. 5.1/7.1: a source of up to two channels stays stereo (the track's panner places it),
        // a wider one is read in the mix's format. Routed clips read the picked channels as stereo.
        Reader::Options ro{opt.rate, nch};
        ro.stereoBelow = nch > 2;
        ro.pick.assign(v.clip.sourceChannels.begin(), v.clip.sourceChannels.end());
        st.reader = Reader::open(v.path, v.stream, ro, &error);
        if (st.reader) ++stats->readersOpened;
        else ++stats->missing;
        if (st.reader) st.channels = std::min(nch, st.reader->channels());
        if (st.reader && v.retimed)
            st.retimer = std::make_unique<Retimer>(std::move(st.reader), v.clip, v.fileFrames,
                                                   double(opt.rate) / clock.fps.fps());
    }
}

bool Mixer::Impl::readSource(Voice& v, VoiceState& st, int64_t u, int count, float* const* out)
{
    open(v, st);
    const int64_t pos = u + v.sourceOffset;
    // Channels the source does not give (a stereo source in a 5.1 mix) are silent
    for (int c = st.channels; c < nch; ++c) std::fill(out[c], out[c] + count, 0.f);
    if (st.sub) {
        st.sub->chunkWarm = chunkWarm;
        st.sub->render(pos, count, out);
        return true;
    }
    if (st.retimer) {
        st.retimer->read(pos, count, out);
        return true;
    }
    if (st.reader) return st.reader->read(pos, count, out);
    for (int c = 0; c < nch; ++c) std::fill(out[c], out[c] + count, 0.f);
    return false;
}

// The voice's sound at unshifted samples [u, u + count): source, noise reduction, voice cleaning, curves, declick
void Mixer::Impl::renderVoice(Voice& v, VoiceState& st, int64_t u, int count, float* const* out)
{
    const bool fresh = u != st.nextU;
    st.nextU = u + count;
    open(v, st);
    const int vch = st.channels; // channels the voice has (the rest of the mix's stay silent)
    if (v.denoise || v.clean) {
        // Backwards in chunks (setChunked): the processors keep what they learnt from the neighbouring chunk
        const bool surround = vch > ChannelLayout::FC; // a source with a centre channel
        const bool have = (!v.denoise || !st.denoisers.empty()) && (!v.clean || (st.cleaner && (!surround || st.centreCleaner)));
        const bool keep = fresh && have && chunkWarm >= 0;
        if ((fresh && !keep) || !have) {
            st.denoisers.clear();
            st.cleaner.reset();
            st.centreCleaner.reset();
            st.cleanDelay.clear();
            for (int c = 0; v.denoise && c < vch; ++c) {
                st.denoisers.push_back(std::make_unique<Denoise::Processor>(opt.rate));
                st.denoisers.back()->setParams(v.denoiseReduction, v.denoiseFloor, v.denoiseTrack);
            }
            if (v.clean) {
                st.cleaner = std::make_unique<VoiceClean::Processor>(opt.rate);
                st.cleaner->setParams(v.cleanParams);
                if (surround) {
                    st.centreCleaner = std::make_unique<VoiceClean::Processor>(opt.rate);
                    st.centreCleaner->setParams(v.cleanParams);
                    st.cleanDelay.assign(size_t(vch - ChannelLayout::LFE), std::vector<float>(size_t(st.cleaner->latency()), 0.f));
                }
            }
        }
        auto process = [&](float* const* data, int n) {
            for (int c = 0; c < int(st.denoisers.size()); ++c) st.denoisers[size_t(c)]->process(data[c], n);
            if (st.cleaner) st.cleaner->process(data, n);
            if (st.centreCleaner) {
                // The centre on its own (a copy as the processor's second channel: linked, it sees one sound)
                centreCopy.assign(data[ChannelLayout::FC], data[ChannelLayout::FC] + n);
                float* centre[2] = {data[ChannelLayout::FC], centreCopy.data()};
                st.centreCleaner->process(centre, n);
                // The other channels come that much later, as the cleaned ones
                for (size_t k = 0; k < st.cleanDelay.size(); ++k) {
                    std::vector<float>& held = st.cleanDelay[k];
                    float* x = data[size_t(ChannelLayout::LFE) + k];
                    const size_t len = held.size();
                    delayed.resize(len + size_t(n));
                    std::copy(held.begin(), held.end(), delayed.begin());
                    std::copy(x, x + n, delayed.begin() + ptrdiff_t(len));
                    std::copy(delayed.begin(), delayed.begin() + n, x);
                    std::copy(delayed.begin() + n, delayed.end(), held.begin());
                }
            }
        };
        // The processors run `lead` samples ahead, their output lines up with the timeline. Starting inside the
        // clip, the voice cleaning first listens to up to half a second before u (its noise and voice level settle),
        // so playback from there sounds like the export that ran through from the clip's start.
        const int lead = (v.denoise ? st.denoisers[0]->latency() : 0) + (st.cleaner ? st.cleaner->latency() : 0);
        // Kept processors only need their buffers filled anew with the sound before u (silence before the clip).
        if (fresh) {
            const int64_t warm = keep ? chunkWarm : v.clean ? std::clamp<int64_t>(u - v.uStart, 0, opt.rate / 2) : 0;
            stats->warmed += warm + lead;
            for (int64_t p = u - warm, end = u + lead; p < end;) {
                const int n = int(std::min<int64_t>(end - p, p < v.uStart ? v.uStart - p : 4096));
                scratch.resize(size_t(n) * size_t(nch));
                float* pre[kMaxChannels];
                for (int c = 0; c < nch; ++c) pre[c] = scratch.data() + size_t(c) * n;
                if (p < v.uStart)
                    std::fill(scratch.begin(), scratch.begin() + ptrdiff_t(n) * nch, 0.f);
                else
                    readSource(v, st, p, n, pre);
                process(pre, n);
                p += n;
            }
        }
        readSource(v, st, u + lead, count, out);
        process(out, count);
    } else {
        readSource(v, st, u, count, out);
    }

    // Curves frame by frame: a frame's value runs linearly over its samples
    for (int64_t k = clock.frameAt(u), end = u + count; clock.start(k) < end; ++k) {
        const int64_t f0 = clock.start(k), f1 = clock.start(k + 1), n = f1 - f0;
        const int64_t a = std::max(f0, u), b = std::min(f1, end);
        for (const Stage& s : v.stages) {
            if (k < s.from || k >= s.to) continue;
            const auto [g0, g1] = s.at(k);
            const double step = (g1 - g0) / double(n);
            float* l = out[0] + (a - u);
            float* r = out[1] + (a - u);
            if (s.kind == Stage::Gain) {
                for (int64_t i = a; i < b; ++i, ++l, ++r) {
                    const double g = g0 + step * double(i - f0);
                    *l = float(*l * g);
                    *r = float(*r * g);
                }
                for (int c = 2; c < vch; ++c) { // the further channels of a 5.1/7.1 source
                    float* x = out[c] + (a - u);
                    for (int64_t i = a; i < b; ++i, ++x) *x = float(*x * (g0 + step * double(i - f0)));
                }
            } else { // balance of the front pair: the far side is turned down linearly
                for (int64_t i = a; i < b; ++i, ++l, ++r) {
                    const double w = g0 + step * double(i - f0);
                    *l = float(*l * (w < 0 ? 1.0 : std::max(0.0, 1.0 - w)));
                    *r = float(*r * (w < 0 ? std::max(0.0, w + 1.0) : 1.0));
                }
            }
        }
    }
    // Declick at hard cuts: the first/last ~4 ms ramp from/to silence
    auto ramp = [&](int64_t from, int len, bool in) {
        for (int64_t i = std::max(from, u); i < std::min(from + len, u + count); ++i) {
            const float g = in ? float(i - from) / len : float(from + len - 1 - i) / len;
            for (int c = 0; c < vch; ++c) out[c][i - u] *= g;
        }
    };
    if (v.declickIn >= 0) ramp(v.declickIn, v.declickInLen, true);
    if (v.declickOut >= 0) ramp(v.declickOut - v.declickOutLen, v.declickOutLen, false);
}

std::vector<double> Mixer::Impl::trackTarget(const TrackPlan& tp) const
{
    if (nch == 2) { // fader, then balance
        const double side[2] = {tp.pan < 0 ? 1.0 : std::max(0.0, 1.0 - tp.pan),
                                tp.pan < 0 ? std::max(0.0, tp.pan + 1.0) : 1.0};
        return {tp.audible ? tp.gain * side[0] : 0.0, tp.audible ? tp.gain * side[1] : 0.0};
    }
    const ChannelLayout::PanGains g = ChannelLayout::surroundPan(nch, tp.pan * 100.0, tp.panDepth, tp.panCenter, tp.lfeDb);
    const double gain = tp.audible ? tp.gain : 0.0;
    std::vector<double> t(size_t(nch) * 2 + 2);
    for (int o = 0; o < nch; ++o)
        for (int i = 0; i < 2; ++i) t[size_t(o) * 2 + size_t(i)] = gain * g.m[size_t(o)][size_t(i)];
    t[size_t(nch) * 2] = gain * g.lfe;
    t[size_t(nch) * 2 + 1] = gain; // channels 2.. of the track pass through
    return t;
}

double Mixer::Impl::autoGain(const TrackPlan& tp, int64_t frame) const
{
    return dbToGain(std::clamp(tp.faderDb + Automation::valueAt(tp.autoCurve, double(frame)), kMinVolumeDb, kMaxVolumeDb));
}

void Mixer::Impl::applyAutomation(const TrackPlan& tp, TrackState& ts, int64_t pos, int count)
{
    // One gain per frame boundary like the clip curves, a frame runs linearly from its start value to its end value:
    // the curve is continuous, so there is no step anywhere. Without a curve (just removed) the gain is 1.
    const bool on = !tp.autoCurve.isEmpty();
    int64_t f = clock.frameAt(pos);
    int64_t s0 = clock.start(f), s1 = clock.start(f + 1);
    double g0 = on ? autoGain(tp, f) : 1.0, g1 = on ? autoGain(tp, f + 1) : 1.0;
    // A live change (new curve, fader moved) glides from what played last, like the fader
    const int len = ts.autoGlide ? std::min(count, kGlide) : 0;
    const double from = ts.autoLast;
    double g = from;
    for (int i = 0; i < count; ++i) {
        const int64_t s = pos + i;
        while (s >= s1) {
            ++f;
            s0 = s1;
            s1 = clock.start(f + 1);
            g0 = g1;
            g1 = on ? autoGain(tp, f + 1) : 1.0;
        }
        g = s1 > s0 ? g0 + (g1 - g0) * double(s - s0) / double(s1 - s0) : g0;
        if (i < len) g = from + (g - from) * (i + 1) / len;
        for (int c = 0; c < nch; ++c) track[size_t(c)][size_t(i)] = float(track[size_t(c)][size_t(i)] * g);
    }
    ts.autoLast = g;
    ts.autoGlide = false;
}

// Track and master mix of [pos, pos + count) into `mix`, before the master limiter
void Mixer::Impl::renderMix(int64_t pos, int count)
{
    for (int c = 0; c < nch; ++c) std::fill(mix[size_t(c)].begin(), mix[size_t(c)].begin() + count, 0.f);
    const size_t stride = tracks.size() * 2;
    const size_t chStride = (tracks.size() + 1) * size_t(nch);
    if (metering && meterBlock > 0) {
        const size_t blocks = size_t((count + meterBlock - 1) / meterBlock);
        blockPeaks.assign(blocks * stride, 0.f);
        chBlockPeaks.assign(blocks * chStride, 0.f);
    }
    const int64_t end = pos + count;
    // Meters: the left speakers to L, the right ones to R, centre and LFE to both
    const auto meterSide = [this](int c) { return nch == 2 ? c : c == ChannelLayout::FC || c == ChannelLayout::LFE ? -1 : c % 2; };
    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        TrackPlan& tp = tracks[ti];
        TrackState& ts = trackStates[ti];
        // Fader, then balance (5.1/7.1: the surround panner); a changed value glides (updateMix)
        const std::vector<double> target = trackTarget(tp);
        if (!ts.levelSet) ts.applied = target;
        ts.levelSet = true;
        const bool glide = target != ts.applied;
        float* peak = metering ? peaks.data() + 2 * ti : nullptr;
        float* chPeak = metering ? chPeaks.data() + size_t(nch) * ti : nullptr;
        if (peak) {
            peak[0] = peak[1] = 0.f;
            std::fill(chPeak, chPeak + nch, 0.f);
        }
        if (!tp.audible && !glide) continue;
        for (int c = 0; c < nch; ++c) std::fill(track[size_t(c)].begin(), track[size_t(c)].begin() + count, 0.f);
        while (ts.firstVoice < tp.voices.size() && tp.voices[ts.firstVoice].tEnd <= pos) {
            ts.voices[ts.firstVoice] = {}; // done: close its reader
            ++ts.firstVoice;
        }
        bool any = false;
        for (size_t vi = ts.firstVoice; vi < tp.voices.size(); ++vi) {
            Voice& v = tp.voices[vi];
            if (v.tStart() >= end) break;
            const int64_t a = std::max(pos, v.tStart()), b = std::min(end, v.tEnd);
            if (b <= a) continue;
            float* out[kMaxChannels];
            for (int c = 0; c < nch; ++c) out[c] = voice[size_t(c)].data();
            renderVoice(v, ts.voices[vi], a - v.shift, int(b - a), out);
            for (int c = 0; c < nch; ++c)
                for (int64_t i = 0; i < b - a; ++i) track[size_t(c)][size_t(a - pos + i)] += out[c][i];
            any = true;
        }
        // EQ and compressor run on silence too (their tails), so does the LFE send's low pass
        if (!any && !tp.eq.on && !tp.dynamics.on && (nch == 2 || (ts.lfeZ1 == 0 && ts.lfeZ2 == 0))) {
            ts.applied = target;
            ts.autoLast = tp.autoCurve.isEmpty() ? 1.0 : autoGain(tp, clock.frameAt(end));
            ts.autoGlide = false;
            continue;
        }
        if (tp.eq.on || tp.dynamics.on) {
            scratch.resize(size_t(count) * size_t(nch));
            float* planar = scratch.data();
            for (int c = 0; c < nch; ++c)
                std::copy(track[size_t(c)].begin(), track[size_t(c)].begin() + count, planar + size_t(c) * count);
            if (tp.eq.on) ts.eq.processPlanar(planar, count, nch, opt.rate);
            if (tp.dynamics.on) ts.comp.processPlanar(planar, count, nch, opt.rate);
            for (int c = 0; c < nch; ++c)
                std::copy(planar + size_t(c) * count, planar + size_t(c + 1) * count, track[size_t(c)].begin());
        }
        // Automation (fader + curve) where the fader would be: after EQ and compressor, before the pan
        if (!tp.autoCurve.isEmpty() || ts.autoGlide) applyAutomation(tp, ts, pos, count);
        if (nch == 2) {
            const double side[2] = {tp.pan < 0 ? 1.0 : std::max(0.0, 1.0 - tp.pan),
                                    tp.pan < 0 ? std::max(0.0, tp.pan + 1.0) : 1.0};
            if (!glide) { // fader, then pan (each rounds to float)
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < count; ++i) {
                        float x = track[size_t(c)][size_t(i)];
                        if (tp.gain != 1.0) x = float(x * tp.gain);
                        if (tp.pan != 0.0) x = float(x * side[c]);
                        track[size_t(c)][size_t(i)] = x;
                    }
            } else {
                const int len = std::min(count, kGlide);
                for (int c = 0; c < 2; ++c) {
                    const double from = ts.applied[size_t(c)], to = target[size_t(c)];
                    for (int i = 0; i < count; ++i) {
                        const double g = i < len ? from + (to - from) * (i + 1) / len : to;
                        track[size_t(c)][size_t(i)] = float(track[size_t(c)][size_t(i)] * g);
                    }
                    ts.applied[size_t(c)] = to;
                }
            }
        } else {
            // Surround panner: the front pair into the speakers, the LFE send through its low pass, channels 2.. of
            // the track (a 5.1/7.1 source's centre, LFE, surrounds) with the fader only
            const int len = glide ? std::min(count, kGlide) : 0;
            const size_t n = target.size(), lfeAt = size_t(nch) * 2, passAt = lfeAt + 1;
            std::vector<double> g = target;
            const LowPass& lp = lfeLowPass;
            for (int i = 0; i < count; ++i) {
                if (i < len)
                    for (size_t k = 0; k < n; ++k) g[k] = ts.applied[k] + (target[k] - ts.applied[k]) * (i + 1) / len;
                else if (i == len && len > 0)
                    g = target;
                const double l = track[0][size_t(i)], r = track[1][size_t(i)];
                double y[kMaxChannels];
                for (int o = 0; o < nch; ++o) {
                    y[o] = g[size_t(o) * 2] * l + g[size_t(o) * 2 + 1] * r;
                    if (o >= 2) y[o] += g[passAt] * track[size_t(o)][size_t(i)];
                }
                // LFE send (direct form II transposed)
                const double x = g[lfeAt] * (l + r); // surroundPan's lfe holds the 1/2 of (L + R) / 2
                const double lfe = lp.b0 * x + ts.lfeZ1;
                ts.lfeZ1 = lp.b1 * x - lp.a1 * lfe + ts.lfeZ2;
                ts.lfeZ2 = lp.b2 * x - lp.a2 * lfe;
                if (std::abs(ts.lfeZ1) < 1e-25 && std::abs(ts.lfeZ2) < 1e-25) ts.lfeZ1 = ts.lfeZ2 = 0; // no denormals
                y[ChannelLayout::LFE] += lfe;
                for (int o = 0; o < nch; ++o) track[size_t(o)][size_t(i)] = float(y[o]);
            }
            ts.applied = target;
        }
        for (int c = 0; c < nch; ++c) {
            const int side = meterSide(c);
            for (int i = 0; i < count; ++i) {
                mix[size_t(c)][size_t(i)] += track[size_t(c)][size_t(i)];
                if (peak) {
                    const float x = std::abs(track[size_t(c)][size_t(i)]);
                    if (side != 1) peak[0] = std::max(peak[0], x);
                    if (side != 0) peak[1] = std::max(peak[1], x);
                    chPeak[c] = std::max(chPeak[c], x);
                }
            }
        }
        if (peak && meterBlock > 0)
            for (int b = 0, from = 0; from < count; ++b, from += meterBlock)
                for (int c = 0; c < nch; ++c) {
                    float q = 0.f;
                    for (int i = from, to = std::min(count, from + meterBlock); i < to; ++i)
                        q = std::max(q, std::abs(track[size_t(c)][size_t(i)]));
                    const int side = meterSide(c);
                    float* bp = blockPeaks.data() + size_t(b) * stride + 2 * ti;
                    if (side != 1) bp[0] = std::max(bp[0], q);
                    if (side != 0) bp[1] = std::max(bp[1], q);
                    chBlockPeaks[size_t(b) * chStride + size_t(nch) * ti + size_t(c)] = q;
                }
    }
    if (masterGain != masterApplied) {
        const int len = std::min(count, kGlide);
        for (int c = 0; c < nch; ++c) {
            std::vector<float>& m = mix[size_t(c)];
            for (int i = 0; i < count; ++i)
                m[size_t(i)] = float(m[size_t(i)] * (i < len ? masterApplied + (masterGain - masterApplied) * (i + 1) / len : masterGain));
        }
        masterApplied = masterGain;
    } else if (masterGain != 1.0) {
        for (int c = 0; c < nch; ++c) {
            std::vector<float>& m = mix[size_t(c)];
            for (int i = 0; i < count; ++i) m[size_t(i)] = float(m[size_t(i)] * masterGain);
        }
    }
}

Mixer::Mixer(const Timeline& timeline, FrameRate fps, const Options& options)
    : d(std::make_unique<Impl>(options, fps, options.channels > 0 ? options.channels : timeline.audioChannels))
{
    d->outCh = options.outputChannels > 0 ? std::clamp(options.outputChannels, 1, kMaxChannels) : d->nch;
    if (d->outCh != 1) d->outCh = ChannelLayout::valid(d->outCh);
    if (options.limitOutput)
        if (const int lc = d->outCh == 1 ? 2 : d->outCh; lc < d->nch) d->limitCh = lc; // a downmix only
    d->nested = timeline.nested;
    d->stats = &d->ownStats;
    d->plan(timeline);
}

Mixer::~Mixer() = default;

int Mixer::rate() const { return d->opt.rate; }
int Mixer::channels() const { return d->nch; }
int Mixer::outputChannels() const { return d->outCh; }
const std::vector<float>& Mixer::lastMix() const { return d->converted; }
int64_t Mixer::length() const { return d->endSample; }
int64_t Mixer::frameStart(int64_t frame) const { return d->clock.start(frame); }
int64_t Mixer::frameAt(int64_t sample) const { return d->clock.frameAt(sample); }
const Mixer::Stats& Mixer::stats() const { return *d->stats; }

std::vector<std::pair<int64_t, int64_t>> Mixer::soundSpans() const
{
    std::vector<std::pair<int64_t, int64_t>> spans;
    for (const TrackPlan& t : d->tracks)
        if (t.audible)
            for (const Voice& v : t.voices) spans.emplace_back(v.tStart(), v.tEnd);
    std::sort(spans.begin(), spans.end());
    std::vector<std::pair<int64_t, int64_t>> merged;
    for (const auto& s : spans) {
        if (s.second <= s.first) continue;
        if (!merged.empty() && s.first <= merged.back().second) merged.back().second = std::max(merged.back().second, s.second);
        else merged.push_back(s);
    }
    return merged;
}

void Mixer::render(int64_t pos, int count, float* const* out)
{
    Impl& m = *d;
    if (m.outCh == m.nch) {
        m.render(pos, count, out);
        return;
    }
    if (m.limitCh > 0) { // export of a downmix: converted and limited inside (Options::limitOutput)
        if (m.outCh == m.limitCh) {
            m.render(pos, count, out);
            return;
        }
        m.post2.resize(size_t(std::max(count, 0)) * 2); // mono: the mean of the limited stereo downmix
        float* lr[2] = {m.post2.data(), m.post2.data() + std::max(count, 0)};
        m.render(pos, count, lr);
        for (int i = 0; i < count; ++i) out[0][i] = float(0.5 * (double(lr[0][i]) + lr[1][i]));
        return;
    }
    // Another format than the mix (monitor downmix, export): mixed in the mix's format, then converted
    m.converted.resize(size_t(std::max(count, 0)) * size_t(m.nch));
    float* in[kMaxChannels];
    for (int c = 0; c < m.nch; ++c) in[c] = m.converted.data() + size_t(c) * size_t(count);
    m.render(pos, count, in);
    if (count <= 0) return;
    if (m.outCh == 1) { // mono: the mean of the stereo downmix
        const ChannelLayout::Matrix k = ChannelLayout::conversion(m.nch, 2);
        for (int i = 0; i < count; ++i) {
            double sum = 0;
            for (int o = 0; o < 2; ++o)
                for (int c = 0; c < m.nch; ++c) sum += k[size_t(o)][size_t(c)] * in[c][i];
            out[0][i] = float(0.5 * sum);
        }
        return;
    }
    const float* from[kMaxChannels];
    for (int c = 0; c < m.nch; ++c) from[c] = in[c];
    convertChannels(from, m.nch, out, m.outCh, count);
}

void Mixer::setMetering(bool on)
{
    d->metering = on;
    d->peaks.assign(on ? d->tracks.size() * 2 : 0, 0.f);
    d->chPeaks.assign(on ? (d->tracks.size() + 1) * size_t(d->nch) : 0, 0.f);
    if (!on) d->chBlockPeaks.clear();
}

// The master's channels of the last render() into the channel meters (after renderMix filled the tracks' part)
void Mixer::Impl::meterMaster(const std::vector<std::vector<float>>& master, int count)
{
    const size_t at = tracks.size() * size_t(nch), chStride = at + size_t(nch);
    if (chPeaks.size() != chStride) return;
    for (int c = 0; c < nch; ++c) {
        const float* x = master[size_t(c)].data();
        float q = 0.f;
        for (int i = 0; i < count; ++i) q = std::max(q, std::abs(x[i]));
        chPeaks[at + size_t(c)] = q;
    }
    if (meterBlock <= 0) return;
    for (int b = 0, from = 0; from < count && (size_t(b) + 1) * chStride <= chBlockPeaks.size(); ++b, from += meterBlock)
        for (int c = 0; c < nch; ++c) {
            const float* x = master[size_t(c)].data();
            float q = 0.f;
            for (int i = from, to = std::min(count, from + meterBlock); i < to; ++i) q = std::max(q, std::abs(x[i]));
            chBlockPeaks[size_t(b) * chStride + at + size_t(c)] = q;
        }
}

const std::vector<float>& Mixer::channelPeaks() const { return d->chPeaks; }

const std::vector<float>& Mixer::blockChannelPeaks() const { return d->chBlockPeaks; }

const std::vector<float>& Mixer::trackPeaks() const { return d->peaks; }

void Mixer::setMeterBlock(int block) { d->meterBlock = std::max(0, block); }

const std::vector<float>& Mixer::blockPeaks() const { return d->blockPeaks; }

void Mixer::setChunked(int warm) { d->chunkWarm = warm < 0 ? -1 : warm; }

bool Mixer::updateMix(const Timeline& timeline)
{
    Impl& m = *d;
    if (!sameSound(m.planned, timeline)) return false;
    const bool anySolo = std::any_of(timeline.audio.begin(), timeline.audio.end(), [](const Track& t) { return t.solo; });
    for (int i = 0; i < timeline.audio.size(); ++i) {
        const Track& t = timeline.audio[i];
        if (!t.muted && (!anySolo || t.solo) && !m.tracks[size_t(i)].planned) return false; // silent when planned
    }
    // Automation changed (or the fader of an automated track): glide there from what played last
    for (int i = 0; i < timeline.audio.size() && size_t(i) < m.trackStates.size(); ++i) {
        const Track& was = m.planned.audio[i];
        const Track& now = timeline.audio[i];
        if (was.volumeAuto != now.volumeAuto || (!now.volumeAuto.isEmpty() && was.volumeDb != now.volumeDb))
            m.trackStates[size_t(i)].autoGlide = m.trackStates[size_t(i)].levelSet;
    }
    m.applyMixValues(timeline);
    m.planned = timeline;
    for (size_t i = 0; i < m.trackStates.size(); ++i) {
        m.trackStates[i].eq.setEq(m.tracks[i].eq);
        m.trackStates[i].comp.setParams(m.tracks[i].dynamics);
    }
    m.peakLimiter.setCeilingDb(m.limiterDb);
    return true;
}

void Mixer::Impl::render(int64_t pos, int count, float* const* out)
{
    if (count <= 0) return;
    Impl& m = *this;
    const bool restarted = pos != m.expected;
    if (restarted) {
        if (m.expected != INT64_MIN) ++m.stats->restarts;
        if (m.chunkWarm >= 0 && m.expected != INT64_MIN) m.continueChunk();
        else m.restart();
    }
    m.expected = pos + count;
    const size_t need = size_t(std::max(count, m.latency));
    const int nch = m.nch;
    for (int c = 0; c < nch; ++c) {
        for (auto* v : {&m.mix[size_t(c)], &m.track[size_t(c)], &m.voice[size_t(c)]})
            if (v->size() < need) v->resize(need);
    }
    // Options::limitOutput with a downmix: the mix converted first, the limiter on what is written
    const int lch = m.limitCh > 0 ? m.limitCh : nch;
    std::vector<std::vector<float>>& limited = m.limitCh > 0 ? m.post : m.mix;
    if (m.limitCh > 0) {
        m.post.resize(size_t(lch));
        for (auto& v : m.post)
            if (v.size() < need) v.resize(need);
    }
    auto limit = [&](int n) {
        if (m.limitCh > 0) {
            const float* in[kMaxChannels];
            float* to[kMaxChannels];
            for (int c = 0; c < nch; ++c) in[c] = m.mix[size_t(c)].data();
            for (int c = 0; c < lch; ++c) to[c] = m.post[size_t(c)].data();
            convertChannels(in, nch, to, lch, n);
        }
        if (!m.limiter) return;
        m.scratch.resize(size_t(n) * size_t(lch));
        for (int c = 0; c < lch; ++c)
            std::copy(limited[size_t(c)].begin(), limited[size_t(c)].begin() + n, m.scratch.begin() + ptrdiff_t(c) * n);
        m.peakLimiter.processPlanar(m.scratch.data(), n, lch, m.opt.rate);
        for (int c = 0; c < lch; ++c)
            std::copy(m.scratch.begin() + ptrdiff_t(c) * n, m.scratch.begin() + ptrdiff_t(c + 1) * n, limited[size_t(c)].begin());
    };
    // The limiter's delay is compensated by mixing that far ahead (primed on a fresh start)
    if (restarted && m.latency > 0) {
        m.renderMix(pos, m.latency);
        limit(m.latency);
    }
    m.renderMix(pos + m.latency, count);
    limit(count);
    if (m.metering) m.meterMaster(lch == nch ? limited : m.mix, count); // a downmix limited inside: the mix before it
    for (int c = 0; c < lch; ++c) std::copy(limited[size_t(c)].begin(), limited[size_t(c)].begin() + count, out[c]);
}

} // namespace Audio
