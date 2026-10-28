// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/MixedSound.h"

#include <Mlt.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace {

struct State {
    std::mutex mutex;
    std::unique_ptr<Audio::Mixer> mixer;
    FrameRate timelineFps;
    MixedSound::Output out;
    std::vector<float> left, right;
};

int64_t floorDiv(int64_t a, int64_t b)
{
    const int64_t q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

template <typename T>
T clampRound(double v, double lo, double hi)
{
    return T(std::clamp(std::nearbyint(v), lo, hi));
}

// Writes count samples of the mix (planar L/R) in the wanted format and channel count
void convert(const float* L, const float* R, int count, int channels, mlt_audio_format format, int bits, void* dst)
{
    const bool planar = format == mlt_audio_s32 || format == mlt_audio_float;
    auto value = [&](int i, int c) -> double {
        if (channels == 1) return 0.5 * (double(L[i]) + double(R[i]));
        return c == 0 ? L[i] : c == 1 ? R[i] : 0.0;
    };
    auto index = [&](int i, int c) {
        return planar ? size_t(c) * size_t(count) + size_t(i) : size_t(i) * size_t(channels) + size_t(c);
    };
    const int intBits = format == mlt_audio_s16 ? 16 : std::clamp(bits > 0 ? bits : 32, 8, 32);
    for (int i = 0; i < count; ++i)
        for (int c = 0; c < channels; ++c) {
            const double v = value(i, c);
            switch (format) {
            case mlt_audio_s16:
                static_cast<int16_t*>(dst)[index(i, c)] = clampRound<int16_t>(v * 32768.0, -32768.0, 32767.0);
                break;
            case mlt_audio_s32:
            case mlt_audio_s32le: {
                // Rounded to the file's bit depth, then placed in the top bits (the encoder keeps those)
                const double scale = std::ldexp(1.0, intBits - 1);
                const auto q = clampRound<int64_t>(v * scale, -scale, scale - 1);
                static_cast<int32_t*>(dst)[index(i, c)] = int32_t(q * (int64_t(1) << (32 - intBits)));
                break;
            }
            case mlt_audio_u8:
                static_cast<uint8_t*>(dst)[index(i, c)] = clampRound<uint8_t>(v * 128.0 + 128.0, 0.0, 255.0);
                break;
            default: // mlt_audio_float, mlt_audio_f32le
                static_cast<float*>(dst)[index(i, c)] = float(v);
                break;
            }
        }
}

// Samples [first, second) of output frame n in the mix
std::pair<int64_t, int64_t> span(const Audio::Mixer& mixer, FrameRate timelineFps, const MixedSound::Output& out, int64_t n)
{
    if (out.fps == timelineFps || out.fps.num <= 0 || out.fps.den <= 0)
        return {mixer.frameStart(out.firstFrame + n), mixer.frameStart(out.firstFrame + n + 1)};
    // Other frame rate: the same rounding as the mixer's frame grid, on the output's frame rate
    const int64_t base = mixer.frameStart(out.firstFrame);
    auto start = [&](int64_t k) {
        const int64_t v = k * int64_t(mixer.rate()) * out.fps.den;
        return base + floorDiv(2 * v + out.fps.num, 2 * int64_t(out.fps.num));
    };
    return {start(n), start(n + 1)};
}

int getAudio(mlt_frame frame, void** buffer, mlt_audio_format* format, int* frequency, int* channels, int* samples)
{
    auto filter = static_cast<mlt_filter>(mlt_frame_pop_audio(frame));
    // The rest of the stack (filters of the graph, e.g. a master fader) must not run: the consumer fetches the sound
    // a second time, and every callback left on the stack would then work on this mix again
    while (mlt_frame_pop_audio(frame)) {}
    auto* st = static_cast<State*>(mlt_properties_get_data(MLT_FILTER_PROPERTIES(filter), "_state", nullptr));
    if (!st) return 1;
    const std::lock_guard<std::mutex> lock(st->mutex);
    const int64_t n = mlt_frame_get_position(frame) - st->out.firstPosition;
    const auto [a, b] = span(*st->mixer, st->timelineFps, st->out, n);
    const int count = int(std::max<int64_t>(0, b - a));
    st->left.resize(size_t(std::max(1, count)));
    st->right.resize(size_t(std::max(1, count)));
    float* planes[] = {st->left.data(), st->right.data()};
    if (count > 0) st->mixer->render(a, count, planes);

    mlt_audio_format fmt = *format;
    switch (fmt) {
    case mlt_audio_s16: case mlt_audio_s32: case mlt_audio_s32le: case mlt_audio_float: case mlt_audio_f32le:
    case mlt_audio_u8: break;
    default: fmt = mlt_audio_f32le; // none or unknown: float, the consumer converts
    }
    const int ch = *channels > 0 ? *channels : st->out.channels;
    const int size = mlt_audio_format_size(fmt, std::max(1, count), ch);
    void* data = mlt_pool_alloc(size);
    if (!data) return 1;
    if (count > 0) convert(st->left.data(), st->right.data(), count, ch, fmt, st->out.bits, data);
    else std::fill_n(static_cast<char*>(data), size, char(0));
    mlt_frame_set_audio(frame, data, fmt, size, mlt_pool_release);
    *buffer = data;
    *format = fmt;
    *frequency = st->out.rate;
    *channels = ch;
    *samples = count;
    return 0;
}

mlt_frame process(mlt_filter filter, mlt_frame frame)
{
    // Not passed on: the graph below has no sound, and this replaces whatever it would give. A graph without sound
    // marks its frames "test_audio" (mlt_frame_get_audio then skips every callback and gives silence)
    mlt_properties_set_int(MLT_FRAME_PROPERTIES(frame), "test_audio", 0);
    mlt_frame_push_audio(frame, filter);
    mlt_frame_push_audio(frame, reinterpret_cast<void*>(getAudio));
    return frame;
}

} // namespace

namespace MixedSound {

std::unique_ptr<Mlt::Filter> make(Mlt::Profile& profile, const Timeline& timeline, FrameRate timelineFps,
                                  const Audio::Mixer::Options& options, const Output& output)
{
    mlt_filter f = mlt_filter_new();
    if (!f) return nullptr;
    mlt_service_set_profile(MLT_FILTER_SERVICE(f), profile.get_profile());
    f->process = process;
    auto* st = new State;
    Audio::Mixer::Options o = options;
    o.rate = output.rate;
    st->mixer = std::make_unique<Audio::Mixer>(timeline, timelineFps, o);
    st->timelineFps = timelineFps;
    st->out = output;
    mlt_properties_set_data(MLT_FILTER_PROPERTIES(f), "_state", st, 0, [](void* s) { delete static_cast<State*>(s); },
                            nullptr);
    auto filter = std::make_unique<Mlt::Filter>(f); // holds its own reference
    mlt_filter_close(f);
    return filter;
}

} // namespace MixedSound
