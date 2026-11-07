// SPDX-License-Identifier: AGPL-3.0-only
// Out-of-line parts of check.h (linked into every test): keeps the engine headers out of check.h
#include "check.h"

#include "engine/Engine.h"

namespace Check {

void initEngine() { Engine::initHeadless(); }

} // namespace Check
