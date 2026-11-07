// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// A clip's compositing graph (effect "comp", engine/picture/CompEval) on the graphics card (0.19 package F).
// The CPU evaluation stays the reference and the fallback; this runs the common nodes as shaders
// (src/engine/picture/shaders: merge, mask, key, blur; transform/crop/grade through layer.frag).
//
// Split between CPU and card (decided per frame by planComp, on the CPU side):
// - nodes the card does: merge (all 8 modes, opacity, mask), mask (rect, ellipse, linear, radial; invert, combine;
//   not following a tracker), key (chroma with a coloured key, luma), blur (radius up to 64 box pixels, mask),
//   transform (not following a tracker, not shrunk below 1/2: no mip levels on node pictures), crop (hard edge),
//   grade (without LUT file, mask), color
// - every other node (clip, media, gradient, text, polygon/bezier masks, fx, shadow, glitch, tracker, …) is computed
//   by CompEval on the CPU and uploaded - but only when nothing it reads comes from the card: a CPU node fed by a
//   card node makes the whole graph go to the CPU (no readback in the middle of a frame: the card works on one
//   queue, waiting for it there would cost more than the CPU node itself). Sources are leaves, so the usual graphs
//   (picture/media/text -> key/mask -> merge chain -> blur/grade) run on the card with only their sources uploaded.
// - a graph with no card node at all stays on the CPU (nothing to win).
// Node pictures on the card are RGBA16F premultiplied (Context::compTarget); the shaders convert to the CPU's 8-bit
// straight values and back where the CPU's integer steps matter (merge, key), so card and CPU agree within 1-2 code
// values.

#include "engine/picture/CompEval.h"
#include "engine/picture/gpu/Gpu.h"

#include <QString>

#include <memory>
#include <vector>

struct EffectInstance;

namespace Picture::Gpu {

struct CompPlan;
using CompPlanPtr = std::shared_ptr<const CompPlan>;

// CPU side: the graph of `comp` as passes for the card at env.size, the CPU nodes evaluated here (env's services,
// cache as CompEval). nullptr = the card cannot do this graph (then Comp::evaluate); *why says which node
CompPlanPtr planComp(const EffectInstance& comp, const Comp::Env& env, Comp::Cache* cache, QString* why = nullptr);
// GPU thread, inside begin()/finish(): the graph's picture (compTarget of the plan's size, premultiplied); nullptr =
// the card refused. Textures to keep until finish() go to *keep
TexturePtr runComp(Context& g, const CompPlan& plan, std::vector<TexturePtr>* keep);
QSize planSize(const CompPlan& plan);

} // namespace Picture::Gpu
