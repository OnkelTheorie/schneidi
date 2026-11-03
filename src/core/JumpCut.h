// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Jump cuts (0.17 package "hide jumps"): two clips of one track that touch, play the same file and stream at normal
// speed, and skip a part of it between them (a pause or a word cut out of one take) - the picture jumps. Found here
// for the edit op hide_jumps (alternating zoom, so the jump looks like a camera change) and for `check` (jump_cut).
// Pure function on the model, no Qt beyond containers.

#include "core/Types.h"

#include <QVector>

namespace JumpCut {

struct Jump {
    int left = 0, right = 0; // clip ids
    int at = 0;              // timeline frame of the cut
    int skipped = 0;         // source frames left out between them (< 0: back in the file)
};

// The jumps on `track`, in time order: |skipped| >= minSkip frames (and at least one). A cut to another file or
// stream, a speed change, a title or a gap between the clips is no jump.
QVector<Jump> find(const Track& track, int minSkip);

// Whether the two clips look the same where they meet: same zoom, position and rotation (a jump shows)
bool sameFraming(const Clip& a, const Clip& b);

} // namespace JumpCut
