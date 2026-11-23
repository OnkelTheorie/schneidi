// SPDX-License-Identifier: AGPL-3.0-only
#include "core/EditDefaults.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace EditDefaults {

namespace {
std::atomic<double> g_still{kStillSeconds};
std::atomic<double> g_transition{kTransitionSeconds};

int frames(double seconds, int fps) { return std::max(1, int(std::lround(seconds * std::max(1, fps)))); }
} // namespace

double stillSeconds() { return g_still; }
void setStillSeconds(double seconds) { g_still = seconds > 0 ? seconds : kStillSeconds; }
double transitionSeconds() { return g_transition; }
void setTransitionSeconds(double seconds) { g_transition = seconds > 0 ? seconds : kTransitionSeconds; }
int stillFrames(int fps) { return frames(stillSeconds(), fps); }
int transitionFrames(int fps) { return frames(transitionSeconds(), fps); }

} // namespace EditDefaults
