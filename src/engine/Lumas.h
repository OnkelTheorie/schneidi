// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Gradient wipes (TransitionType::Luma): grayscale image, dark spots switch first (as in Kdenlive/Shotcut/OpenShot,
// so their luma images work). Bundled gradients are computed by id (":/lumas/<id>", names in core/EffectFolders),
// custom images from the effects folder are read in.

#include <QImage>
#include <QString>

namespace Lumas {

// Gradient image in grayscale (w x h); empty = unknown/unreadable (error set)
QImage image(const QString& path, int w, int h, bool invert = false, QString* error = nullptr);
// Preview icon: clip A (blue) switches halfway to B (orange)
QImage preview(const QImage& luma, double progress = 0.5);

} // namespace Lumas
