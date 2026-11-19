// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// A timeline copied into another frame shape (new_timeline from + aspect: a 9:16 short of a 16:9 edit): scaleTimeline
// keeps every overlay at the same place relative to the frame, sizes by the short side. That leaves three things
// wrong (duel 17.11.): a shape that filled the old frame (a colour matte) is a band in the new one, a picture overlay
// (a logo) changes its size with the picture's fit while the plate behind it keeps its own, and an overlay in a
// corner lands in the phone apps' UI zones or off the frame. OverlayFit puts these right; pure, no project.

#include "core/ExportCheck.h"
#include "core/Types.h"

#include <QRectF>
#include <QSizeF>
#include <QVector>

#include <functional>

namespace OverlayFit {

// One overlay before the frame changed
struct Before {
    int clip = 0;
    QRectF block;           // what it covers in the old frame (pixels, y down)
    bool fullFrame = false; // a shape covering the whole old frame (a colour matte)
    bool picture = false;   // a media overlay (Reframe::isOverlay: a logo, a webcam picture)
};

// The overlays of `tl` in frame `from`: title clips (text, counter, shape) and still-image overlays (a logo) on tracks
// above V1 (isStill: the clip shows a still image), not animated in place or size (those are listed in *animated and
// left as they are). Video overlays (a webcam picture) keep the place and size scaleTimeline gave them.
QVector<Before> collect(const Timeline& tl, QSizeF from, const std::function<bool(const Clip&)>& isStill,
                        QVector<int>* animated = nullptr);

// Where overlays may sit in a frame: inside title safe (90 %), in a vertical frame also out of the apps' zones
QRectF safeRect(QSizeF frame, const ExportCheck::Options& o = {});

// After the frame changed (scaleTimeline ran on `tl`): full-frame shapes fill the new frame, picture overlays get
// the size the short side gives (like shapes and titles), and overlays that overlap in time and place (a logo on its
// plate) are moved together, as little as needed, into safeRect. Returns the ids it changed.
QVector<int> apply(Timeline& tl, const QVector<Before>& before, QSizeF from, QSizeF to, const ExportCheck::Options& o = {});

} // namespace OverlayFit
