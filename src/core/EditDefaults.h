// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

// Default lengths of new things on the timeline: a still picture added to the project (its length in the media
// pool, which a placed clip takes) and a transition added without a length. The app sets the user's choice
// (Settings › Timeline, TimelineOptions::applyEditDefaults); without that - the CLI, tests - they stay 5 s and 1 s,
// what schneidi always did, and a CLI op's own length parameters win anyway.
namespace EditDefaults {

constexpr double kStillSeconds = 5.0;
constexpr double kTransitionSeconds = 1.0;

double stillSeconds();
void setStillSeconds(double seconds);
double transitionSeconds();
void setTransitionSeconds(double seconds);
// In frames of `fps`, at least 1
int stillFrames(int fps);
int transitionFrames(int fps);

} // namespace EditDefaults
