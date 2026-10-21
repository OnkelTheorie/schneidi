#include "engine/RateConverter.h"

#include <framework/mlt.h>
#include <Mlt.h>

#include <cmath>
#include <cstring>
#include <vector>

namespace RateConverter {

namespace {

struct Context {
    mlt_producer self = nullptr;
    mlt_producer timeline = nullptr; // own reference
    mlt_profile profile = nullptr;   // clone of the timeline profile, used by the nested consumer
    mlt_consumer consumer = nullptr; // pulls the timeline's frames (fully processed) one at a time
    int64_t audioCounter = 0;
    mlt_position audioPosition = -1; // timeline frame whose sound was delivered last
};

Context* context(mlt_producer self)
{
    return static_cast<Context*>(mlt_properties_get_data(MLT_PRODUCER_PROPERTIES(self), "_rate_converter", nullptr));
}

int getImage(mlt_frame frame, uint8_t** image, mlt_image_format* format, int* width, int* height, int writable)
{
    auto* cx = static_cast<Context*>(mlt_frame_pop_service(frame));
    auto nested = static_cast<mlt_frame>(mlt_frame_pop_service(frame));
    *width = cx->profile->width;
    *height = cx->profile->height;
    const int result = mlt_frame_get_image(nested, image, format, width, height, writable);
    if (result || !*image) return result ? result : 1;
    // Copy: a repeated timeline frame is shown by several output frames
    int size = mlt_image_format_size(*format, *width, *height, nullptr);
    auto* copy = static_cast<uint8_t*>(mlt_pool_alloc(size));
    std::memcpy(copy, *image, size);
    mlt_frame_set_image(frame, copy, size, mlt_pool_release);
    *image = copy;
    mlt_properties fp = MLT_FRAME_PROPERTIES(frame), np = MLT_FRAME_PROPERTIES(nested);
    mlt_properties_set(fp, "progressive", mlt_properties_get(np, "progressive"));
    mlt_properties_set_int(fp, "colorspace", mlt_properties_get_int(np, "colorspace"));
    mlt_properties_set_int(fp, "full_range", mlt_properties_get_int(np, "full_range"));
    if (uint8_t* alpha = mlt_frame_get_alpha_size(nested, &size); alpha && size > 0) {
        auto* a = static_cast<uint8_t*>(mlt_pool_alloc(size));
        std::memcpy(a, alpha, size);
        mlt_frame_set_alpha(frame, a, size, mlt_pool_release);
    }
    return 0;
}

int getAudio(mlt_frame frame, void** buffer, mlt_audio_format* format, int* frequency, int* channels, int* samples)
{
    auto* cx = static_cast<Context*>(mlt_frame_pop_audio(frame));
    auto nested = static_cast<mlt_frame>(mlt_frame_pop_audio(frame));
    if (mlt_frame_get_position(nested) == cx->audioPosition) { // repeated timeline frame: its sound is already out
        *samples = 0;
        return 0;
    }
    double fps = mlt_profile_fps(cx->profile);
    if (const double out = mlt_producer_get_fps(cx->self); out < fps) {
        fps = out;
        mlt_properties_set_double(MLT_FRAME_PROPERTIES(nested), "producer_consumer_fps", fps);
    }
    *samples = mlt_audio_calculate_frame_samples(fps, *frequency, cx->audioCounter++);
    const int result = mlt_frame_get_audio(nested, buffer, format, frequency, channels, samples);
    if (result || !*buffer) return result ? result : 1;
    const int size = mlt_audio_format_size(*format, *samples, *channels);
    void* copy = mlt_pool_alloc(size);
    std::memcpy(copy, *buffer, size);
    mlt_frame_set_audio(frame, copy, *format, size, mlt_pool_release);
    *buffer = copy;
    cx->audioPosition = mlt_frame_get_position(nested);
    return 0;
}

int getFrame(mlt_producer self, mlt_frame_ptr frame, int)
{
    Context* cx = context(self);
    *frame = mlt_frame_init(MLT_PRODUCER_SERVICE(self));
    if (*frame) {
        double position = double(mlt_producer_frame(self));
        if (const double speed = mlt_producer_get_speed(self); speed != 0) position *= speed;
        mlt_producer_seek(cx->timeline,
                          std::lrint(std::floor(position) * mlt_profile_fps(cx->profile) / mlt_producer_get_fps(self)));
        mlt_frame nested = mlt_consumer_rt_frame(cx->consumer);

        mlt_frame_push_service(*frame, nested);
        mlt_frame_push_service(*frame, cx);
        mlt_frame_push_get_image(*frame, getImage);
        mlt_frame_push_audio(*frame, nested);
        mlt_frame_push_audio(*frame, cx);
        mlt_frame_push_audio(*frame, reinterpret_cast<void*>(getAudio));
        mlt_frame_set_position(*frame, mlt_producer_position(self));

        mlt_properties fp = MLT_FRAME_PROPERTIES(*frame);
        mlt_properties_set_data(fp, "_rate_converter.frame", nested, 0, (mlt_destructor)mlt_frame_close, nullptr);
        mlt_properties_set_double(fp, "aspect_ratio", mlt_profile_sar(cx->profile));
        mlt_properties_set_int(fp, "width", cx->profile->width);
        mlt_properties_set_int(fp, "height", cx->profile->height);
        mlt_properties_set_int(fp, "meta.media.width", cx->profile->width);
        mlt_properties_set_int(fp, "meta.media.height", cx->profile->height);
        mlt_properties_set_int(fp, "progressive", cx->profile->progressive);
    }
    mlt_producer_prepare_next(self);
    return 0;
}

void close(mlt_producer self)
{
    if (Context* cx = context(self)) {
        mlt_consumer_stop(cx->consumer);
        mlt_consumer_close(cx->consumer);
        mlt_producer_close(cx->timeline);
        mlt_profile_close(cx->profile);
        delete cx;
    }
    self->close = nullptr;
    mlt_producer_close(self);
    free(self); // allocated by mlt_producer_new
}

} // namespace

std::unique_ptr<Mlt::Producer> make(Mlt::Profile& outProfile, Mlt::Profile& timelineProfile, Mlt::Producer& timeline)
{
    mlt_producer self = mlt_producer_new(outProfile.get_profile());
    if (!self) return nullptr;
    auto* cx = new Context;
    cx->self = self;
    cx->profile = mlt_profile_clone(timelineProfile.get_profile());
    cx->timeline = timeline.get_producer();
    mlt_properties_inc_ref(MLT_PRODUCER_PROPERTIES(cx->timeline));
    mlt_producer_set_speed(cx->timeline, 0); // seeked by getFrame only
    cx->consumer = mlt_consumer_new(cx->profile);
    mlt_properties_set_int(MLT_CONSUMER_PROPERTIES(cx->consumer), "real_time", 0); // fetch in the calling thread
    mlt_consumer_connect(cx->consumer, MLT_PRODUCER_SERVICE(cx->timeline));
    mlt_consumer_start(cx->consumer);
    mlt_properties_set_data(MLT_PRODUCER_PROPERTIES(self), "_rate_converter", cx, 0, nullptr, nullptr);
    self->get_frame = getFrame;
    self->close = reinterpret_cast<mlt_destructor>(close);

    auto producer = std::make_unique<Mlt::Producer>(self); // holds its own reference
    mlt_producer_close(self);
    // What MLT's loader attaches to every producer: the frames ask for image and sound in the encoder's formats
    const std::vector<std::vector<const char*>> filters = {{"avcolor_space", "imageconvert"}, {"audioconvert"}};
    for (const auto& choices : filters)
        for (const char* name : choices) {
            Mlt::Filter f(outProfile, name);
            if (!f.is_valid()) continue;
            producer->attach(f);
            break;
        }
    return producer;
}

} // namespace RateConverter
