#include "engine/audio/Volume.h"

#include <framework/mlt.h>

#include <cmath>

namespace Volume {
namespace {

int getAudio(mlt_frame frame, void** buffer, mlt_audio_format* format, int* frequency, int* channels, int* samples)
{
    mlt_filter filter = static_cast<mlt_filter>(mlt_frame_pop_audio(frame));
    mlt_properties props = MLT_FILTER_PROPERTIES(filter);

    double gain = 1.0;
    if (mlt_properties_get(props, "level")) {
        const mlt_position position = mlt_filter_get_position(filter, frame);
        const mlt_position length = mlt_filter_get_length2(filter, frame);
        gain = std::pow(10.0, mlt_properties_anim_get_double(props, "level", position, length) / 20.0);
    }

    *format = mlt_audio_f32le;
    const int error = mlt_frame_get_audio(frame, buffer, format, frequency, channels, samples);
    if (error || !buffer || !*buffer || *samples <= 0) return error;

    // Ramp from the gain of the frame before (only when this frame follows it: after a seek it would jump)
    mlt_service_lock(MLT_FILTER_SERVICE(filter));
    const mlt_position last = mlt_properties_get_position(props, "_last_position");
    const mlt_position current = mlt_frame_get_position(frame);
    if (!mlt_properties_get(props, "_previous_gain") || current != last + 1)
        mlt_properties_set_double(props, "_previous_gain", gain);
    const double previous = mlt_properties_get_double(props, "_previous_gain");
    mlt_properties_set_double(props, "_previous_gain", gain);
    mlt_properties_set_position(props, "_last_position", current);
    mlt_service_unlock(MLT_FILTER_SERVICE(filter));

    const double step = (gain - previous) / *samples;
    double g = previous;
    float* p = static_cast<float*>(*buffer);
    const int n = *channels;
    for (int i = 0; i < *samples; ++i, g += step)
        for (int c = 0; c < n; ++c, ++p) *p = float(*p * g);
    return 0;
}

mlt_frame process(mlt_filter filter, mlt_frame frame)
{
    mlt_frame_push_audio(frame, filter);
    mlt_frame_push_audio(frame, reinterpret_cast<void*>(getAudio));
    return frame;
}

void* create(mlt_profile, mlt_service_type, const char* id, const void*)
{
    mlt_filter f = mlt_filter_new();
    if (!f) return nullptr;
    f->process = process;
    mlt_properties_set(MLT_FILTER_PROPERTIES(f), "mlt_service", id);
    return f;
}

} // namespace

void registerService()
{
    mlt_repository repo = mlt_factory_repository();
    if (!repo) return;
    mlt_repository_register(repo, mlt_service_filter_type, "schneidi_volume", create);
    if (!mlt_properties_get_data(mlt_repository_filters(repo), "volume", nullptr))
        mlt_repository_register(repo, mlt_service_filter_type, "volume", create);
}

} // namespace Volume
