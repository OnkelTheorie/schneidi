// SPDX-License-Identifier: AGPL-3.0-only
// Edit ops of the 0.17 package "story": story sections and timeline notes (Timeline::sections / notes).
// The package fills in its ops here: one OpDef per op in addStoryOps (usage/summary/details like the other
// EditOps*.cpp; every op also takes the central 'why' key, applyOp). Empty until then.

#include "cli/CommandsDetail.h"

namespace Cli::detail {

void addStoryOps(QVector<OpDef>& ops)
{
    Q_UNUSED(ops);
}

} // namespace Cli::detail
