// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Internal helpers of Editor, shared by its implementation files (not part of the API).
#include "core/TimelineOps.h"
#include "core/Types.h"

#include <QSet>

namespace EditorDetail {

// Set the stored edge lengths to the effective ones (what does not fit is shortened or removed)
void fitTransitions(Timeline& tl, const QSet<int>& clipIds, const TimelineOps::SourceLength& len);

// What was set on a compound or multicam clip `outer` goes onto `x`, a piece of its content that now takes its place
// (decompose, flatten): transform (if set), effects (same effect: outer's), volume (added), pan and fine position (if
// set), outer's keyframes (frames of the sequence -> x's source frames: + `sourceShift`), and at outer's edges
// (atStart/atEnd) its transitions and fades, if it has any there (else x keeps its own). outerShift = frames from
// outer's first frame to x's (sequence frames): outer's comp graph keeps its picture in place (Comp::followHead)
void takeOuter(Clip& x, const Clip& outer, int sourceShift, bool atStart, bool atEnd, int outerShift = 0,
               double fps = 0);

} // namespace EditorDetail
