// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// In-between pictures for slow motion (Clip::interpolate): the picture at time t (0..1) between two neighbouring
// source frames a and b, as pure RGBA functions (8 bit, straight or premultiplied alike, all four channels).
//
// Blend: (1 - t) * a + t * b per pixel.
// Flow: motion compensated. Own implementation of textbook methods: coarse-to-fine block matching on a luma pyramid
// (candidate vectors from the coarser level and the neighbours, small full search, sub-pixel by a parabola fit), both
// directions, forward/backward consistency marks unreliable blocks. The vectors are projected onto the in-between
// time (each block lands at p + t * v), every in-between block takes the candidate whose two ends match best
// (bilateral cost), and every output pixel picks again between the vectors around it and standing still - so static
// overlays (a game HUD) stay put while the picture behind them moves. Where nothing matches (occlusion) the nearer
// frame's sample is used; when most of the picture is unreliable the whole frame falls back to blending. A scene cut
// between a and b gives no in-between at all (the nearer frame, no ghosting).

#include <cstdint>
#include <memory>

#include <QString>

namespace Picture::Interp {

// Weighted average, rounded: t = 0.5 gives (a + b + 1) / 2
void blend(const uint8_t* a, const uint8_t* b, uint8_t* out, int w, int h, double t);

// a and b belong to different shots (a hard cut): their brightness histograms differ too much
bool isCut(const uint8_t* a, const uint8_t* b, int w, int h);

// Motion between a and b (expensive part, cache it per frame pair; independent of t)
struct Motion;
std::shared_ptr<const Motion> estimate(const uint8_t* a, const uint8_t* b, int w, int h);
bool cut(const Motion& m);
// Share of blocks whose forward and backward vectors disagree (0..1)
double unreliable(const Motion& m);

// The motion compensated picture at t (a, b at the size estimate() saw)
void flow(const Motion& m, const uint8_t* a, const uint8_t* b, uint8_t* out, int w, int h, double t);

// estimate() through a small cache: key names the frame pair (source, frame index, size)
std::shared_ptr<const Motion> cachedMotion(const QString& key, const uint8_t* a, const uint8_t* b, int w, int h);

} // namespace Picture::Interp
