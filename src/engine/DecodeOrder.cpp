#include "engine/DecodeOrder.h"

#include <Mlt.h>
#include <QtGlobal>
#include <framework/mlt.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>

namespace DecodeOrder {

namespace {

// Only a safety net (see DecodeOrder.h): normally the frame before is decoded or released long before
constexpr auto kMaxWait = std::chrono::seconds(1);
constexpr const char* kTicket = "_decode_order.ticket";

struct Gate {
    std::mutex mutex;
    std::condition_variable cond;
    uint64_t next = 0;      // number of the next frame created
    std::set<uint64_t> open; // frames neither decoded nor released yet
};

// One per frame of the producer, in the order MLT creates them (one thread)
struct Ticket {
    std::shared_ptr<Gate> gate;
    uint64_t number = 0;
    bool done = false;

    void resolve()
    {
        {
            std::lock_guard<std::mutex> lock(gate->mutex);
            if (done) return;
            done = true;
            gate->open.erase(number);
        }
        gate->cond.notify_all();
    }
};

void releaseTicket(void* ticket)
{
    auto* t = static_cast<Ticket*>(ticket);
    t->resolve(); // a frame that is never decoded (track covered) must not hold up the ones after it
    delete t;
}

int getImage(mlt_frame frame, uint8_t** image, mlt_image_format* format, int* width, int* height, int writable)
{
    auto* t = static_cast<Ticket*>(mlt_properties_get_data(MLT_FRAME_PROPERTIES(frame), kTicket, nullptr));
    if (t) {
        Gate& g = *t->gate;
        std::unique_lock<std::mutex> lock(g.mutex);
        const bool inOrder = g.cond.wait_for(lock, kMaxWait, [&] { return t->done || *g.open.begin() >= t->number; });
        if (!inOrder) {
            static std::once_flag warned;
            std::call_once(warned, [] { qWarning("Decode order: a frame waited too long for the one before it"); });
        }
    }
    const int error = mlt_frame_get_image(frame, image, format, width, height, writable);
    if (t) t->resolve();
    return error;
}

mlt_frame process(mlt_filter filter, mlt_frame frame)
{
    auto gate = *static_cast<std::shared_ptr<Gate>*>(mlt_properties_get_data(MLT_FILTER_PROPERTIES(filter), "gate", nullptr));
    auto* t = new Ticket;
    t->gate = gate;
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        t->number = gate->next++;
        gate->open.insert(t->number);
    }
    mlt_properties_set_data(MLT_FRAME_PROPERTIES(frame), kTicket, t, 0, releaseTicket, nullptr);
    mlt_frame_push_get_image(frame, getImage);
    return frame;
}

void destroyGate(void* gate)
{
    delete static_cast<std::shared_ptr<Gate>*>(gate);
}

} // namespace

void attach(Mlt::Producer& producer)
{
    mlt_filter f = mlt_filter_new();
    if (!f) return;
    f->process = process;
    mlt_properties_set_data(MLT_FILTER_PROPERTIES(f), "gate", new std::shared_ptr<Gate>(std::make_shared<Gate>()), 0,
                            destroyGate, nullptr);
    Mlt::Filter filter(f); // holds its own reference
    mlt_filter_close(f);
    producer.attach(filter);
}

} // namespace DecodeOrder
