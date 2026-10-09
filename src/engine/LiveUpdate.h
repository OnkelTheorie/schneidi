#pragma once
// Inspector changes without restarting the preview: when an edit only changes values (transform, effect parameters,
// keyframes, clip volume/pan, fades), the Engine still builds the new MLT tractor (cheap), but instead of swapping it
// in (consumer stop/start, decoders seek again: 3–10x the cost of one frame) it copies the new values into the
// running tractor's filters and transitions. Anything else, or a graph that does not match node for node, falls
// back to the full swap.
#include "core/Types.h"

namespace Mlt {
class Service;
}

namespace LiveUpdate {

// before -> after changes only values that transfer() can carry (incl. the nested sequences of compound clips)
bool valuesOnly(const Timeline& before, const Timeline& after);

// Walk both graphs in parallel. Same structure (services, playlist entries, in/out, producer properties) -> copy the
// properties of every filter/transition/cut of `fresh` into its counterpart in `live` and return true. Otherwise
// change nothing and return false. Safe while the consumer renders `live` (MLT properties lock themselves).
bool transfer(Mlt::Service& live, Mlt::Service& fresh);

} // namespace LiveUpdate
