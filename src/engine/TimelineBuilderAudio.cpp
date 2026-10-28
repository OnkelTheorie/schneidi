// SPDX-License-Identifier: AGPL-3.0-only
// TimelineBuilder: audio filters (volume, pan, meters, limiter, track EQ/compressor, sub-frame delay, mixer strips).
#include "engine/TimelineBuilderDetail.h"
#include "core/Dynamics.h"
#include "core/Equalizer.h"
#include "core/Limiter.h"
#include "core/Loudness.h"
#include "core/TruePeak.h"

#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/TimelineOps.h"
#include "engine/ColorGrade.h"
#include "engine/Denoise.h"
#include "engine/Lumas.h"
#include "engine/Profiles.h"
#include "engine/PitchLatency.h"
#include "engine/RampProducer.h"
#include "engine/Stabilizer.h"

#include <Mlt.h>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QFont>
#include <QFontMetricsF>
#include <QHash>
#include <QImage>
#include <QString>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
#include <cmath>
#include <functional>
#include <optional>
#include <utility>


namespace TimelineBuilderDetail {

// a/len: excerpt for keyframes (len = 0: static value only, e.g. mixer)
void applyVolume(Mlt::Profile& profile, Mlt::Service& clip, const Clip& c, int a, int len)
{
    const bool anim = len > 0 && Keys::animated(c, AnimParam::Volume);
    if (c.volumeDb == 0.0 && !anim && !t_liveValues) return;
    Mlt::Filter f(profile, "volume");
    if (!f.is_valid()) return;
    // -∞ is -200 dB in MLT -> sections starting/ending there not linear -> frame by frame
    const KeyTrack keys = c.keys.value(AnimParam::Volume);
    const bool bake = std::any_of(keys.begin(), keys.end(), [](const Keyframe& k) { return k.value <= kMinVolumeDb; });
    if (!anim || !animate(c, {AnimParam::Volume}, a, len, [&](int pos, double t) {
            f.anim_set("level", mltLevel(Keys::valueAt(c, AnimParam::Volume, t)), pos, len);
        }, bake))
        f.set("level", mltLevel(c.volumeDb)); // dB
    if (anim) attachTo(static_cast<Mlt::Producer&>(clip), f);
    else clip.attach(f);
}

void applyPan(Mlt::Profile& profile, Mlt::Service& clip, const Clip& c, int a, int len)
{
    const bool anim = len > 0 && Keys::animated(c, AnimParam::Pan);
    if (c.pan == 0.0 && !anim) return;
    Mlt::Filter f(profile, "panner");
    if (!f.is_valid()) return;
    f.set("channel", -1); // Balance (Stereo)
    // "start" instead of "split": split in MLT 7 acts like a fixed offset regardless of the value (tested)
    f.set("start", (c.pan + 100.0) / 200.0);
    if (anim) {
        // animated via "split" (set as a number via anim_set, not as text -> number format irrelevant)
        animate(c, {AnimParam::Pan}, a, len, [&](int pos, double t) {
            f.anim_set("split", (Keys::valueAt(c, AnimParam::Pan, t) + 100.0) / 200.0, pos, len);
        });
        attachTo(static_cast<Mlt::Producer&>(clip), f);
    } else {
        clip.attach(f);
    }
}

// Mixer: track/master fader and pan as filters on playlist resp. tractor.
// With hooks the filters are always attached (neutral at 0 dB/center) so that they can be adjusted live.

// Multiplies the samples [from, to) of every channel by gain(i)
template <typename Gain>
void scaleSamples(void* buffer, mlt_audio_format format, int channels, int n, int from, int to, Gain gain)
{
    for (int i = from; i < to; ++i) {
        const float g = gain(i);
        for (int c = 0; c < channels; ++c) {
            switch (format) {
            case mlt_audio_s16: { auto& v = static_cast<int16_t*>(buffer)[i * channels + c]; v = int16_t(v * g); break; }
            case mlt_audio_s32le: { auto& v = static_cast<int32_t*>(buffer)[i * channels + c]; v = int32_t(v * double(g)); break; }
            case mlt_audio_s32: { auto& v = static_cast<int32_t*>(buffer)[c * n + i]; v = int32_t(v * double(g)); break; }
            case mlt_audio_f32le: static_cast<float*>(buffer)[i * channels + c] *= g; break;
            case mlt_audio_float: static_cast<float*>(buffer)[c * n + i] *= g; break;
            default: return;
            }
        }
    }
}

// Seek during playback (SeekFade): every chunk is delayed by 5 ms (its end comes out with the next chunk). Frame
// position not the next one = jump: the held-back old sound fades out over those 5 ms, the new one fades in over
// 10 ms. While dragging (hold) silent, after release() the next sound fades in.
void seekFade(SeekFade& fade, int pos, void* buffer, mlt_audio_format format, int channels, int n, int frequency)
{
    const int dir = fade.direction;
    const bool restart = fade.restart.exchange(false);
    if (dir == 0 || n <= 1 || channels <= 0) { // shuttling: untouched; afterwards play() starts anew
        fade.lastPos = -1;
        return;
    }
    const auto at = [&](int i, int c) -> size_t {
        return format == mlt_audio_s32 || format == mlt_audio_float ? size_t(c) * n + i : size_t(i) * channels + c;
    };
    const auto get = [&](int i, int c) -> float {
        switch (format) {
        case mlt_audio_s16: return static_cast<const int16_t*>(buffer)[at(i, c)] / 32768.f;
        case mlt_audio_s32le:
        case mlt_audio_s32: return static_cast<const int32_t*>(buffer)[at(i, c)] / 2147483648.f;
        case mlt_audio_f32le:
        case mlt_audio_float: return static_cast<const float*>(buffer)[at(i, c)];
        default: return 0.f;
        }
    };
    const auto set = [&](int i, int c, float v) {
        switch (format) {
        case mlt_audio_s16: static_cast<int16_t*>(buffer)[at(i, c)] = int16_t(std::clamp(v * 32768.f, -32768.f, 32767.f)); break;
        case mlt_audio_s32le:
        case mlt_audio_s32:
            static_cast<int32_t*>(buffer)[at(i, c)] = int32_t(std::clamp(double(v) * 2147483648.0, -2147483648.0, 2147483647.0));
            break;
        case mlt_audio_f32le:
        case mlt_audio_float: static_cast<float*>(buffer)[at(i, c)] = v; break;
        default: break;
        }
    };
    if (format != mlt_audio_s16 && format != mlt_audio_s32le && format != mlt_audio_s32 && format != mlt_audio_f32le
        && format != mlt_audio_float)
        return;

    const int hold = std::clamp(frequency / 200, 1, n - 1); // held back: 5 ms
    if (restart || channels != fade.tailChannels || fade.tail.size() != size_t(hold) * channels) {
        fade.tail.assign(size_t(hold) * channels, 0.f);
        fade.tailChannels = channels;
        fade.silent = true;
    }
    const int seeks = fade.seeks;
    const bool jump = !restart && fade.lastPos >= 0 && (pos != fade.lastPos + dir || seeks != fade.seenSeeks);
    fade.lastPos = pos;
    fade.seenSeeks = seeks;
    const bool silentNow = fade.hold;
    const bool fadeOut = !fade.silent && (jump || silentNow);
    const bool fadeIn = !silentNow && (fade.silent || jump);
    const int inLen = std::clamp(frequency / 100, 1, n - hold); // 10 ms, ends before the next tail

    std::vector<float> next(size_t(hold) * channels);
    for (int c = 0; c < channels; ++c) {
        for (int i = 0; i < hold; ++i) next[size_t(i) * channels + c] = get(n - hold + i, c);
        // shift the chunk back by `hold` (from the end, in place), the held-back tail goes in front
        for (int i = n - 1; i >= hold; --i) {
            float v = get(i - hold, c);
            if (silentNow) v = 0.f;
            else if (fadeIn && i - hold < inLen) v *= float(i - hold) / inLen;
            set(i, c, v);
        }
        for (int i = 0; i < hold; ++i) {
            float v = fade.tail[size_t(i) * channels + c];
            if (fade.silent) v = 0.f;
            else if (fadeOut) v *= 1.f - float(i + 1) / hold;
            set(i, c, v);
        }
    }
    fade.tail = std::move(next);
    fade.silent = silentNow;
}

// Level meter (passive): remembers the peak level of the last processed audio per channel in dBFS as
// _audio_level.N (N = channel) and the true peak (BS.1770, 4x oversampled) in dBTP as _true_peak.N.
// Own filter instead of MLT "audiolevel": that converts the audio to s16 and thereby clips
// everything above 0 dBFS (preview sounded different from the export, overload was not measurable).
int meterGetAudio(mlt_frame frame, void** buffer, mlt_audio_format* format, int* frequency, int* channels, int* samples)
{
    auto filter = static_cast<mlt_filter>(mlt_frame_pop_audio(frame));
    const int error = mlt_frame_get_audio(frame, buffer, format, frequency, channels, samples);
    if (error || !*buffer || *channels <= 0 || *samples <= 0) return error;
    const int ch = std::min(*channels, 8), n = *samples;
    auto signedSample = [&](int c, int i) -> float {
        switch (*format) {
        case mlt_audio_s16: return static_cast<const int16_t*>(*buffer)[i * *channels + c] / 32768.f;
        case mlt_audio_s32le: return static_cast<const int32_t*>(*buffer)[i * *channels + c] / 2147483648.f;
        case mlt_audio_s32: return static_cast<const int32_t*>(*buffer)[c * n + i] / 2147483648.f;
        case mlt_audio_f32le: return static_cast<const float*>(*buffer)[i * *channels + c];
        case mlt_audio_float: return static_cast<const float*>(*buffer)[c * n + i];
        default: return 0.f;
        }
    };
    float peak[8] = {};
    for (int c = 0; c < ch; ++c)
        for (int i = 0; i < n; ++i) peak[c] = std::max(peak[c], std::abs(signedSample(c, i)));
    mlt_properties props = MLT_FILTER_PROPERTIES(filter);
    if (auto* fade = static_cast<SeekFade*>(mlt_properties_get_data(props, "_seek_fade", nullptr)))
        seekFade(*fade, mlt_frame_get_position(frame), *buffer, *format, *channels, n, *frequency);
    // true peak of this chunk (filter history continues across the chunks)
    auto* tp = static_cast<TruePeakMeter*>(mlt_properties_get_data(props, "_tp", nullptr));
    if (tp) {
        tp->resetPeaks();
        tp->add(n, ch, *frequency, signedSample);
    }
    // Master in the preview: also measure loudness and maximum true peak (loudness meter in the mixer)
    if (auto* live = static_cast<SharedLoudness*>(mlt_properties_get_data(props, "_loudness", nullptr));
        live && live->active) {
        std::lock_guard<std::mutex> lock(live->mutex);
        live->meter.add(n, *channels, *frequency, signedSample);
        live->truePeak.add(n, *channels, *frequency, signedSample);
    }
    for (int c = 0; c < ch; ++c) {
        char name[32];
        std::snprintf(name, sizeof name, "_audio_level.%d", c);
        mlt_properties_set_double(props, name, peak[c] > 1e-10f ? 20.0 * std::log10(peak[c]) : -200.0);
        if (tp) {
            std::snprintf(name, sizeof name, "_true_peak.%d", c);
            mlt_properties_set_double(props, name, tp->peakDb(c));
        }
    }
    return 0;
}

mlt_frame meterProcess(mlt_filter filter, mlt_frame frame)
{
    mlt_frame_push_audio(frame, filter);
    mlt_frame_push_audio(frame, reinterpret_cast<void*>(meterGetAudio));
    return frame;
}

std::shared_ptr<Mlt::Filter> makeMeter()
{
    mlt_filter f = mlt_filter_new();
    if (!f) return nullptr;
    f->process = meterProcess;
    mlt_properties_set_data(MLT_FILTER_PROPERTIES(f), "_tp", new TruePeakMeter, 0,
                            [](void* p) { delete static_cast<TruePeakMeter*>(p); }, nullptr);
    auto meter = std::make_shared<Mlt::Filter>(f); // holds its own reference
    mlt_filter_close(f);
    return meter;
}

// Master limiter: own filter around PeakLimiter (lookahead, true peak). avfilter.alimiter got integer audio in the
// export (clipped at 0 dBFS before the limiter, louder than measured) and only held the sample peak.
// The ceiling is stored in milli-dB as an integer (no number format issues, see docs/dev/mlt.md).
struct LimiterState {
    std::mutex mutex;
    PeakLimiter limiter, before; // before = state before the last frame (the same frame requested again)
    mlt_position last = -10;
};

int limiterGetAudio(mlt_frame frame, void** buffer, mlt_audio_format* format, int* frequency, int* channels,
                    int* samples)
{
    auto filter = static_cast<mlt_filter>(mlt_frame_pop_audio(frame));
    *format = mlt_audio_float;
    const int error = mlt_frame_get_audio(frame, buffer, format, frequency, channels, samples);
    if (error || !*buffer || *channels <= 0 || *samples <= 0) return error;
    if (*format != mlt_audio_float) {
        if (!frame->convert_audio || frame->convert_audio(frame, buffer, format, mlt_audio_float) || *format != mlt_audio_float)
            return 0;
    }
    mlt_properties props = MLT_FILTER_PROPERTIES(filter);
    auto* st = static_cast<LimiterState*>(mlt_properties_get_data(props, "_state", nullptr));
    if (!st) return 0;
    const std::lock_guard<std::mutex> lock(st->mutex);
    const mlt_position pos = mlt_frame_get_position(frame);
    if (pos == st->last) st->limiter = st->before;
    else if (pos != st->last + 1) st->limiter.reset(); // jump: start over
    st->before = st->limiter;
    st->limiter.setCeilingDb(mlt_properties_get_int(props, "ceiling_mdb") / 1000.0);
    st->limiter.processPlanar(static_cast<float*>(*buffer), *samples, *channels, *frequency);
    st->last = pos;
    return 0;
}

mlt_frame limiterProcess(mlt_filter filter, mlt_frame frame)
{
    mlt_frame_push_audio(frame, filter);
    mlt_frame_push_audio(frame, reinterpret_cast<void*>(limiterGetAudio));
    return frame;
}

void setLimiter(Mlt::Filter& f, bool on, double ceilingDb)
{
    f.set("disable", on ? 0 : 1);
    f.set("ceiling_mdb", int(std::lround(std::clamp(ceilingDb, -40.0, 0.0) * 1000)));
}

std::shared_ptr<Mlt::Filter> makeLimiter(Mlt::Profile& profile, bool on, double ceilingDb)
{
    mlt_filter f = mlt_filter_new();
    if (!f) return nullptr;
    mlt_service_set_profile(MLT_FILTER_SERVICE(f), profile.get_profile());
    f->process = limiterProcess;
    mlt_properties_set_data(MLT_FILTER_PROPERTIES(f), "_state", new LimiterState, 0,
                            [](void* p) { delete static_cast<LimiterState*>(p); }, nullptr);
    auto limiter = std::make_shared<Mlt::Filter>(f);
    mlt_filter_close(f);
    setLimiter(*limiter, on, ceilingDb);
    return limiter;
}

// Track EQ + compressor (Mixer "EQ"/"DYN"): one own filter per audio track, before the fader. The settings live in
// the state (not as text properties), applyMixer changes them while playing.
struct TrackDspState {
    std::mutex mutex;
    TrackEq eq;
    TrackDynamics dynamics;
    Equalizer equalizer, eqBefore; // before = state before the last frame (the same frame requested again)
    Compressor compressor, compBefore;
    mlt_position last = -10;
};

int trackDspGetAudio(mlt_frame frame, void** buffer, mlt_audio_format* format, int* frequency, int* channels,
                     int* samples)
{
    auto filter = static_cast<mlt_filter>(mlt_frame_pop_audio(frame));
    auto* st = static_cast<TrackDspState*>(mlt_properties_get_data(MLT_FILTER_PROPERTIES(filter), "_state", nullptr));
    bool active = false;
    if (st) {
        const std::lock_guard<std::mutex> lock(st->mutex);
        active = st->eq.on || st->dynamics.on;
    }
    if (!active) return mlt_frame_get_audio(frame, buffer, format, frequency, channels, samples);
    *format = mlt_audio_float;
    const int error = mlt_frame_get_audio(frame, buffer, format, frequency, channels, samples);
    if (error || !*buffer || *channels <= 0 || *samples <= 0) return error;
    if (*format != mlt_audio_float) {
        if (!frame->convert_audio || frame->convert_audio(frame, buffer, format, mlt_audio_float) || *format != mlt_audio_float)
            return 0;
    }
    const std::lock_guard<std::mutex> lock(st->mutex);
    const mlt_position pos = mlt_frame_get_position(frame);
    if (pos == st->last) {
        st->equalizer = st->eqBefore;
        st->compressor = st->compBefore;
    } else if (pos != st->last + 1) { // jump: start over
        st->equalizer.reset();
        st->compressor.reset();
    }
    st->equalizer.setEq(st->eq);
    st->compressor.setParams(st->dynamics);
    st->eqBefore = st->equalizer;
    st->compBefore = st->compressor;
    auto* pcm = static_cast<float*>(*buffer);
    st->equalizer.processPlanar(pcm, *samples, *channels, *frequency);
    st->compressor.processPlanar(pcm, *samples, *channels, *frequency);
    st->last = pos;
    return 0;
}

mlt_frame trackDspProcess(mlt_filter filter, mlt_frame frame)
{
    mlt_frame_push_audio(frame, filter);
    mlt_frame_push_audio(frame, reinterpret_cast<void*>(trackDspGetAudio));
    return frame;
}

void setTrackDsp(Mlt::Filter& f, const TrackEq& eq, const TrackDynamics& dynamics)
{
    auto* st = static_cast<TrackDspState*>(f.get_data("_state"));
    if (!st) return;
    const std::lock_guard<std::mutex> lock(st->mutex);
    st->eq = eq;
    st->dynamics = dynamics;
}

std::shared_ptr<Mlt::Filter> makeTrackDsp(Mlt::Profile& profile, const TrackEq& eq, const TrackDynamics& dynamics)
{
    mlt_filter f = mlt_filter_new();
    if (!f) return nullptr;
    mlt_service_set_profile(MLT_FILTER_SERVICE(f), profile.get_profile());
    f->process = trackDspProcess;
    mlt_properties_set_data(MLT_FILTER_PROPERTIES(f), "_state", new TrackDspState, 0,
                            [](void* p) { delete static_cast<TrackDspState*>(p); }, nullptr);
    auto dsp = std::make_shared<Mlt::Filter>(f);
    mlt_filter_close(f);
    setTrackDsp(*dsp, eq, dynamics);
    return dsp;
}

// Hard cut in the sound (clip edge without a fade, crossfade or seamless continuation): ~4 ms ramp at the clip's
// first resp. last frame. Otherwise the wave jumps there (old and new material, or material and silence), which
// clicks – Final Cut does the same at every edit. The filter sits on one excerpt (in/out = the excerpt's), so only
// its first/last frame counts.
int declickGetAudio(mlt_frame frame, void** buffer, mlt_audio_format* format, int* frequency, int* channels, int* samples)
{
    auto filter = static_cast<mlt_filter>(mlt_frame_pop_audio(frame));
    const auto pos = mlt_position(reinterpret_cast<intptr_t>(mlt_frame_pop_audio(frame))); // in the excerpt, from process()
    const int error = mlt_frame_get_audio(frame, buffer, format, frequency, channels, samples);
    if (error || !*buffer || *channels <= 0 || *samples <= 0) return error;
    mlt_properties props = MLT_FILTER_PROPERTIES(filter);
    const int n = *samples, len = std::min(n, std::max(1, *frequency / 250));
    if (pos == 0 && mlt_properties_get_int(props, "ramp_in"))
        scaleSamples(*buffer, *format, *channels, n, 0, len, [len](int i) { return float(i) / len; });
    if (pos == mlt_properties_get_int(props, "last") && mlt_properties_get_int(props, "ramp_out"))
        scaleSamples(*buffer, *format, *channels, n, n - len, n, [n, len](int i) { return float(n - 1 - i) / len; });
    return 0;
}

mlt_frame declickProcess(mlt_filter filter, mlt_frame frame)
{
    mlt_frame_push_audio(frame, reinterpret_cast<void*>(intptr_t(mlt_filter_get_position(filter, frame))));
    mlt_frame_push_audio(frame, filter);
    mlt_frame_push_audio(frame, reinterpret_cast<void*>(declickGetAudio));
    return frame;
}

void attachEdgeDeclick(Mlt::Profile& profile, Mlt::Producer& cut, bool rampIn, bool rampOut)
{
    if (!rampIn && !rampOut) return;
    mlt_filter f = mlt_filter_new();
    if (!f) return;
    mlt_service_set_profile(MLT_FILTER_SERVICE(f), profile.get_profile());
    f->process = declickProcess;
    mlt_properties props = MLT_FILTER_PROPERTIES(f);
    mlt_properties_set_int(props, "ramp_in", rampIn);
    mlt_properties_set_int(props, "ramp_out", rampOut);
    mlt_properties_set_int(props, "last", cut.get_out() - cut.get_in());
    Mlt::Filter filter(f);
    mlt_filter_close(f);
    filter.set_in_and_out(cut.get_in(), cut.get_out()); // positions count from the excerpt start
    cut.attach(filter);
}

// Sub-frame position of audio clips (Clip::subframe): delays the audio by a fraction of a frame. The missing
// samples at the frame start come from the end of the previous frame (remembered); after a jump (seeking) there is
// silence – in the export everything runs in one go. One filter per clip, on all its excerpts (source positions
// continue across the excerpts).
struct SubframeState {
    std::mutex mutex;
    mlt_position last = -10;
    int channels = 0;
    std::vector<float> tail, prevTail; // planar: channels x delay
};

int subframeGetAudio(mlt_frame frame, void** buffer, mlt_audio_format* format, int* frequency, int* channels,
                     int* samples)
{
    auto filter = static_cast<mlt_filter>(mlt_frame_pop_audio(frame));
    *format = mlt_audio_float;
    const int error = mlt_frame_get_audio(frame, buffer, format, frequency, channels, samples);
    if (error || !*buffer || *channels <= 0 || *samples <= 0) return error;
    if (*format != mlt_audio_float) {
        if (!frame->convert_audio || frame->convert_audio(frame, buffer, format, mlt_audio_float) || *format != mlt_audio_float)
            return 0; // unknown format: pass through unchanged
    }
    mlt_properties props = MLT_FILTER_PROPERTIES(filter);
    auto* st = static_cast<SubframeState*>(mlt_properties_get_data(props, "_state", nullptr));
    const double fps = mlt_profile_fps(mlt_service_profile(MLT_FILTER_SERVICE(filter)));
    if (!st || fps <= 0) return 0;
    const int ch = *channels, n = *samples;
    const int d = std::min(n, int(std::lround(mlt_properties_get_int(props, "subframe") / 100.0 * *frequency / fps)));
    if (d <= 0) return 0;

    const std::lock_guard<std::mutex> lock(st->mutex);
    const mlt_position pos = mlt_frame_get_position(frame);
    const size_t need = size_t(ch) * size_t(d);
    std::vector<float> head(need, 0.f); // start of this frame: end of the previous one
    if (st->channels == ch) {
        if (pos == st->last + 1 && st->tail.size() == need) head = st->tail;
        else if (pos == st->last && st->prevTail.size() == need) head = st->prevTail; // same frame once more
    }
    auto* pcm = static_cast<float*>(*buffer);
    std::vector<float> tail(need);
    for (int c = 0; c < ch; ++c) {
        float* plane = pcm + size_t(c) * n;
        std::copy(plane + (n - d), plane + n, tail.begin() + size_t(c) * d);
        std::memmove(plane + d, plane, sizeof(float) * size_t(n - d));
        std::copy(head.begin() + size_t(c) * d, head.begin() + size_t(c + 1) * d, plane);
    }
    if (pos != st->last) {
        st->prevTail = std::move(head);
        st->tail = std::move(tail);
    }
    st->last = pos;
    st->channels = ch;
    return 0;
}

mlt_frame subframeProcess(mlt_filter filter, mlt_frame frame)
{
    mlt_frame_push_audio(frame, filter);
    mlt_frame_push_audio(frame, reinterpret_cast<void*>(subframeGetAudio));
    return frame;
}

std::shared_ptr<Mlt::Filter> makeSubframeDelay(Mlt::Profile& profile, int subframe)
{
    mlt_filter f = mlt_filter_new();
    if (!f) return nullptr;
    mlt_service_set_profile(MLT_FILTER_SERVICE(f), profile.get_profile());
    f->process = subframeProcess;
    mlt_properties_set_int(MLT_FILTER_PROPERTIES(f), "subframe", subframe);
    mlt_properties_set_data(MLT_FILTER_PROPERTIES(f), "_state", new SubframeState, 0,
                            [](void* p) { delete static_cast<SubframeState*>(p); }, nullptr);
    auto delay = std::make_shared<Mlt::Filter>(f);
    mlt_filter_close(f);
    return delay;
}

// limiter: master only (ceiling in dBFS, nullopt = none); sits after fader/pan and before the meter.
// track: audio tracks only, its EQ and compressor sit before the fader.
void attachStrip(Mlt::Profile& profile, Mlt::Service& s, double db, double pan, MixerHooks::Strip* hook,
                 std::optional<std::pair<bool, double>> limiter, const Track* track)
{
    if (track && (hook || track->eq.on || track->dynamics.on)) { // preview: always there, switched live
        auto dsp = makeTrackDsp(profile, track->eq, track->dynamics);
        if (dsp) s.attach(*dsp);
        if (hook) hook->dsp = dsp;
    }
    if (!hook) { // export: only what is needed
        Clip c; // same conversion as for clips
        c.volumeDb = db;
        c.pan = pan;
        applyVolume(profile, s, c);
        applyPan(profile, s, c);
        if (limiter && limiter->first)
            if (auto f = makeLimiter(profile, true, limiter->second)) s.attach(*f);
        return;
    }
    hook->volume = std::make_shared<Mlt::Filter>(profile, "volume");
    hook->volume->set("level", mltLevel(db));
    s.attach(*hook->volume);
    hook->pan = std::make_shared<Mlt::Filter>(profile, "panner");
    hook->pan->set("channel", -1);
    hook->pan->set("start", mltPan(pan));
    s.attach(*hook->pan);
    if (limiter) { // Preview: always there so that it can be switched live
        hook->limiter = makeLimiter(profile, limiter->first, limiter->second);
        if (hook->limiter) s.attach(*hook->limiter);
    }
    hook->meter = makeMeter();
    s.attach(*hook->meter);
}

} // namespace TimelineBuilderDetail
