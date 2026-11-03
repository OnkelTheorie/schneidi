// SPDX-License-Identifier: AGPL-3.0-only
// Edit ops of the 0.17 package "music": music edit (cutting a song to length, music under the edit).
// The package fills in its ops here: one OpDef per op in addMusicOps (usage/summary/details like the other
// EditOps*.cpp; every op also takes the central 'why' key, applyOp). Empty until then.

#include "cli/CommandsDetail.h"

namespace Cli::detail {

void addMusicOps(QVector<OpDef>& ops)
{
    Q_UNUSED(ops);
}

} // namespace Cli::detail
