#pragma once
// Internal helpers of Editor, shared by its implementation files (not part of the API).
#include "core/TimelineOps.h"
#include "core/Types.h"

#include <QSet>

namespace EditorDetail {

// Set the stored edge lengths to the effective ones (what does not fit is shortened or removed)
void fitTransitions(Timeline& tl, const QSet<int>& clipIds, const TimelineOps::SourceLength& len);

} // namespace EditorDetail
