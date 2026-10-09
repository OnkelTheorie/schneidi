#pragma once
// Single frames without a preview (schneidi-cli: lets an AI look at the material). Own profile and producers,
// blocking; transparent areas are black like in the viewer.

#include "core/ProjectFormat.h"
#include "core/Types.h"

#include <QImage>
#include <QString>

namespace Snapshot {

// Frame `frame` (in the rate of `format`) of a media file, fitted into the project aspect ratio
QImage media(const ProjectFormat& format, const QString& path, int frame, int maxEdge = 0);
// Frame of a timeline (use Project::renderTimeline so compound clips render)
QImage timeline(const ProjectFormat& format, const Timeline& tl, int frame, int maxEdge = 0);

} // namespace Snapshot
