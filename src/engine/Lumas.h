// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Gradient wipes (TransitionType::Luma): grayscale image, dark spots switch first (like MLT "luma",
// Kdenlive/Shotcut/OpenShot). Bundled gradients are computed by id (":/lumas/<id>", names in
// core/EffectFolders), custom images from the effects folder are read in. For MLT every image ends up as a
// grayscale PNG at project size in the cache (also inverting), so luma never reads foreign formats or paths with umlauts.

#include <QImage>
#include <QString>

namespace Lumas {

// Gradient image in grayscale (w x h); empty = unknown/unreadable (error set)
QImage image(const QString& path, int w, int h, bool invert = false, QString* error = nullptr);
// PNG for the MLT transition "luma" (cached by path, modification time, size, invert); empty = error
QString file(const QString& path, int w, int h, bool invert);
// Preview icon: clip A (blue) switches halfway to B (orange)
QImage preview(const QImage& luma, double progress = 0.5);

} // namespace Lumas
