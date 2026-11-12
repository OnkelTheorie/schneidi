// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/audio/Mixer.h"

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
    if (a.audio.size() != b.audio.size() || a.masterLimiter != b.masterLimiter) return false;
    for (int i = 0; i < a.audio.size(); ++i) {
        Track x = a.audio[i], y = b.audio[i];
        for (Track* t : {&x, &y}) {
            t->volumeDb = t->pan = 0;
            t->muted = t->solo = false;
            t->eq = {};
            t->dynamics = {};
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
};

} // namespace

struct Mixer::Impl {
    Options opt;
    Clock clock;
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
    };
    struct TrackState {
        Equalizer eq;
        Compressor comp;
        std::vector<VoiceState> voices;
        size_t firstVoice = 0; // voices before it have ended
        // Gain per channel (fader, pan, mute) applied so far; a new value glides there (updateMix)
        double applied[kChannels] = {};
        bool levelSet = false;
    };
    std::vector<TrackState> trackStates;
    PeakLimiter peakLimiter;
    double masterApplied = 1.0;
    bool metering = false;
    std::vector<float> peaks; // per track L, R of the last render (metering)
    int meterBlock = 0;            // setMeterBlock
    std::vector<float> blockPeaks; // peaks per stretch of meterBlock samples of the last render
    int chunkWarm = -1;            // setChunked: >= 0 = a jump keeps filters and processors
    int64_t expected = INT64_MIN; // next render position
    int latency = 0;              // master (limiter), compensated
    std::vector<float> mix[kChannels], track[kChannels], voice[kChannels], scratch;

    Impl(const Options& o, FrameRate fps) : opt(o)
    {
        clock = {o.rate, fps};
    }

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
    auto r = Reader::open(path, 0, Reader::Options{opt.rate, kChannels});
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
        p.gain = t.volumeDb == 0.0 ? 1.0 : dbToGain(t.volumeDb);
        p.pan = t.pan / 100.0;
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
        return !a.mediaPath.isEmpty() && a.mediaPath == b.mediaPath && a.audioStream == b.audioStream && a.enabled
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
        }
        ts.firstVoice = 0;
        ts.levelSet = false;
        ts.eq = Equalizer{};
        ts.eq.setEq(tracks[i].eq);
        ts.comp = Compressor{};
        ts.comp.setParams(tracks[i].dynamics);
    }
    peakLimiter = PeakLimiter{};
    peakLimiter.setCeilingDb(limiterDb);
    peakLimiter.prepare(kChannels, opt.rate);
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

bool Mixer::Impl::readSource(Voice& v, VoiceState& st, int64_t u, int count, float* const* out)
{
    if (!st.opened) {
        st.opened = true;
        if (v.sequence) {
            auto sub = std::make_unique<Impl>(opt, clock.fps);
            sub->nested = nested;
            sub->stats = stats;
            sub->depth = depth + 1;
            sub->stack = stack;
            sub->stack.insert(v.clip.sequenceId);
            sub->plan(v.multicam ? v.angleContent : *v.sequence);
            st.sub = std::move(sub);
        } else {
            QString error;
            st.reader = Reader::open(v.path, v.stream, Reader::Options{opt.rate, kChannels}, &error);
            if (st.reader) ++stats->readersOpened;
            else ++stats->missing;
            if (st.reader && v.retimed)
                st.retimer = std::make_unique<Retimer>(std::move(st.reader), v.clip, v.fileFrames,
                                                       double(opt.rate) / clock.fps.fps());
        }
    }
    const int64_t pos = u + v.sourceOffset;
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
    for (int c = 0; c < kChannels; ++c) std::fill(out[c], out[c] + count, 0.f);
    return false;
}

// The voice's sound at unshifted samples [u, u + count): source, noise reduction, voice cleaning, curves, declick
void Mixer::Impl::renderVoice(Voice& v, VoiceState& st, int64_t u, int count, float* const* out)
{
    const bool fresh = u != st.nextU;
    st.nextU = u + count;
    if (v.denoise || v.clean) {
        // Backwards in chunks (setChunked): the processors keep what they learnt from the neighbouring chunk
        const bool have = (!v.denoise || !st.denoisers.empty()) && (!v.clean || st.cleaner);
        const bool keep = fresh && have && chunkWarm >= 0;
        if ((fresh && !keep) || !have) {
            st.denoisers.clear();
            st.cleaner.reset();
            for (int c = 0; v.denoise && c < kChannels; ++c) {
                st.denoisers.push_back(std::make_unique<Denoise::Processor>(opt.rate));
                st.denoisers.back()->setParams(v.denoiseReduction, v.denoiseFloor, v.denoiseTrack);
            }
            if (v.clean) {
                st.cleaner = std::make_unique<VoiceClean::Processor>(opt.rate);
                st.cleaner->setParams(v.cleanParams);
            }
        }
        auto process = [&](float* const* data, int n) {
            for (int c = 0; c < int(st.denoisers.size()); ++c) st.denoisers[size_t(c)]->process(data[c], n);
            if (st.cleaner) st.cleaner->process(data, n);
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
                scratch.resize(size_t(n) * kChannels);
                float* pre[kChannels];
                for (int c = 0; c < kChannels; ++c) pre[c] = scratch.data() + size_t(c) * n;
                if (p < v.uStart)
                    std::fill(scratch.begin(), scratch.begin() + ptrdiff_t(n) * kChannels, 0.f);
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
            } else { // balance: the far side is turned down linearly
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
            for (int c = 0; c < kChannels; ++c) out[c][i - u] *= g;
        }
    };
    if (v.declickIn >= 0) ramp(v.declickIn, v.declickInLen, true);
    if (v.declickOut >= 0) ramp(v.declickOut - v.declickOutLen, v.declickOutLen, false);
}

// Track and master mix of [pos, pos + count) into `mix`, before the master limiter
void Mixer::Impl::renderMix(int64_t pos, int count)
{
    for (auto& m : mix) std::fill(m.begin(), m.begin() + count, 0.f);
    const size_t stride = tracks.size() * 2;
    if (metering && meterBlock > 0) blockPeaks.assign(size_t((count + meterBlock - 1) / meterBlock) * stride, 0.f);
    const int64_t end = pos + count;
    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        TrackPlan& tp = tracks[ti];
        TrackState& ts = trackStates[ti];
        // Fader, then balance; a changed value glides (updateMix)
        const double side[kChannels] = {tp.pan < 0 ? 1.0 : std::max(0.0, 1.0 - tp.pan),
                                        tp.pan < 0 ? std::max(0.0, tp.pan + 1.0) : 1.0};
        double target[kChannels];
        for (int c = 0; c < kChannels; ++c) target[c] = tp.audible ? tp.gain * side[c] : 0.0;
        if (!ts.levelSet) std::copy(target, target + kChannels, ts.applied);
        ts.levelSet = true;
        const bool glide = !std::equal(target, target + kChannels, ts.applied);
        float* peak = metering ? peaks.data() + 2 * ti : nullptr;
        if (peak) peak[0] = peak[1] = 0.f;
        if (!tp.audible && !glide) continue;
        for (auto& t : track) std::fill(t.begin(), t.begin() + count, 0.f);
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
            float* out[kChannels];
            for (int c = 0; c < kChannels; ++c) out[c] = voice[c].data();
            renderVoice(v, ts.voices[vi], a - v.shift, int(b - a), out);
            for (int c = 0; c < kChannels; ++c)
                for (int64_t i = 0; i < b - a; ++i) track[c][size_t(a - pos + i)] += out[c][i];
            any = true;
        }
        // EQ and compressor run on silence too (their tails)
        if (!any && !tp.eq.on && !tp.dynamics.on) {
            std::copy(target, target + kChannels, ts.applied);
            continue;
        }
        if (tp.eq.on || tp.dynamics.on) {
            scratch.resize(size_t(count) * kChannels);
            float* planar = scratch.data();
            for (int c = 0; c < kChannels; ++c) std::copy(track[c].begin(), track[c].begin() + count, planar + size_t(c) * count);
            if (tp.eq.on) ts.eq.processPlanar(planar, count, kChannels, opt.rate);
            if (tp.dynamics.on) ts.comp.processPlanar(planar, count, kChannels, opt.rate);
            for (int c = 0; c < kChannels; ++c) std::copy(planar + size_t(c) * count, planar + size_t(c + 1) * count, track[c].begin());
        }
        if (!glide) { // fader, then pan (each rounds to float)
            for (int c = 0; c < kChannels; ++c)
                for (int i = 0; i < count; ++i) {
                    float x = track[c][size_t(i)];
                    if (tp.gain != 1.0) x = float(x * tp.gain);
                    if (tp.pan != 0.0) x = float(x * side[c]);
                    track[c][size_t(i)] = x;
                }
        } else {
            const int len = std::min(count, kGlide);
            for (int c = 0; c < kChannels; ++c) {
                const double from = ts.applied[c], to = target[c];
                for (int i = 0; i < count; ++i) {
                    const double g = i < len ? from + (to - from) * (i + 1) / len : to;
                    track[c][size_t(i)] = float(track[c][size_t(i)] * g);
                }
                ts.applied[c] = to;
            }
        }
        for (int c = 0; c < kChannels; ++c)
            for (int i = 0; i < count; ++i) {
                mix[c][size_t(i)] += track[c][size_t(i)];
                if (peak) peak[c] = std::max(peak[c], std::abs(track[c][size_t(i)]));
            }
        if (peak && meterBlock > 0)
            for (int b = 0, from = 0; from < count; ++b, from += meterBlock)
                for (int c = 0; c < kChannels; ++c) {
                    float& q = blockPeaks[size_t(b) * stride + 2 * ti + size_t(c)];
                    for (int i = from, to = std::min(count, from + meterBlock); i < to; ++i)
                        q = std::max(q, std::abs(track[c][size_t(i)]));
                }
    }
    if (masterGain != masterApplied) {
        const int len = std::min(count, kGlide);
        for (auto& m : mix)
            for (int i = 0; i < count; ++i)
                m[size_t(i)] = float(m[size_t(i)] * (i < len ? masterApplied + (masterGain - masterApplied) * (i + 1) / len : masterGain));
        masterApplied = masterGain;
    } else if (masterGain != 1.0) {
        for (auto& m : mix)
            for (int i = 0; i < count; ++i) m[size_t(i)] = float(m[size_t(i)] * masterGain);
    }
}

Mixer::Mixer(const Timeline& timeline, FrameRate fps, const Options& options)
    : d(std::make_unique<Impl>(options, fps))
{
    d->nested = timeline.nested;
    d->stats = &d->ownStats;
    d->plan(timeline);
}

Mixer::~Mixer() = default;

int Mixer::rate() const { return d->opt.rate; }
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

void Mixer::render(int64_t pos, int count, float* const* out) { d->render(pos, count, out); }

void Mixer::setMetering(bool on)
{
    d->metering = on;
    d->peaks.assign(on ? d->tracks.size() * 2 : 0, 0.f);
}

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
    for (int c = 0; c < kChannels; ++c) {
        for (auto* v : {&m.mix[c], &m.track[c], &m.voice[c]})
            if (v->size() < need) v->resize(need);
    }
    auto limit = [&](int n) {
        if (!m.limiter) return;
        m.scratch.resize(size_t(n) * kChannels);
        for (int c = 0; c < kChannels; ++c) std::copy(m.mix[c].begin(), m.mix[c].begin() + n, m.scratch.begin() + ptrdiff_t(c) * n);
        m.peakLimiter.processPlanar(m.scratch.data(), n, kChannels, m.opt.rate);
        for (int c = 0; c < kChannels; ++c)
            std::copy(m.scratch.begin() + ptrdiff_t(c) * n, m.scratch.begin() + ptrdiff_t(c + 1) * n, m.mix[c].begin());
    };
    // The limiter's delay is compensated by mixing that far ahead (primed on a fresh start)
    if (restarted && m.latency > 0) {
        m.renderMix(pos, m.latency);
        limit(m.latency);
    }
    m.renderMix(pos + m.latency, count);
    limit(count);
    for (int c = 0; c < kChannels; ++c) std::copy(m.mix[c].begin(), m.mix[c].begin() + count, out[c]);
}

} // namespace Audio
