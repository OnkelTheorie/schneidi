// SPDX-License-Identifier: AGPL-3.0-only
// Compositing node kinds of package A (0.19): clip, media, color, gradient, merge, transform, crop, blur, grade, fx
#include "engine/picture/CompNodes.h"

namespace Picture::Comp {

void registerCoreNodes(QVector<NodeType>& out)
{
    Q_UNUSED(out);
}

} // namespace Picture::Comp
