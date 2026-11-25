// SPDX-License-Identifier: AGPL-3.0-only
#include "core/NestedLoop.h"

#include <QThread>

namespace NestedLoop {

namespace {
int s_depth = 0; // (GUI thread only)
}

Guard::Guard() { ++s_depth; }
Guard::~Guard() { --s_depth; }

bool guarded() { return s_depth > 0; }

bool running()
{
    // loopLevel(): 1 = the main loop (app.exec); 0 = none yet (tests call rebuildUi directly)
    return s_depth > 0 || QThread::currentThread()->loopLevel() > 1;
}

} // namespace NestedLoop
