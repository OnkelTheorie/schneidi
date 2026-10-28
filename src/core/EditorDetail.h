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
// (atStart/atEnd) its transitions and fades, if it has any there (else x keeps its own)
void takeOuter(Clip& x, const Clip& outer, int sourceShift, bool atStart, bool atEnd);

} // namespace EditorDetail
