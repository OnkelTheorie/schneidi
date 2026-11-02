// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Text-like regions in pictures, for hiding player names (core/NameBlur). No OCR and no trained model: own edge
// heuristic tuned for game HUD fonts (bright or colored letters with an outline or on a dark plate). On a picture
// whose longer side is at most kSide: horizontal brightness steps of both signs close together (letter strokes),
// smeared along the row into text lines, cut into single lines where the rows run empty, and kept when a line has
// the height, width, fill and contrast of text. Finds words, not what they say: a template decides which count.
//
// A clip is analysed on its own picture (not fitted into the timeline's frame) with black bars cut off, so the game
// picture always has about kSide on its longer side; the result is given in the timeline's frame like the windows.
// Per clip, cached in QStandardPaths::CacheLocation/textregions (in picture coordinates; named after the file, its
// size and time, the clip's in/out, the sample step and its picture shape). Runs in any thread, cancellable.

#include "core/NameBlur.h"
#include "core/ProjectFormat.h"
#include "core/Types.h"

#include <QImage>
#include <QRectF>
#include <QString>
#include <QVector>

#include <atomic>

namespace TextRegions {

constexpr int kSide = 1920; // longer side of the analysed picture (name tags are ~8 px high there)

// Text lines in a picture, 0..1 of its size
QVector<QRectF> detect(const QImage& picture);

// Sample step in frames: about 30 samples per second (name tags move fast when the player turns)
int sampleStep(const FrameRate& rate);

struct Result {
    QVector<NameBlur::Sample> samples; // clip frames, ascending
    QString error;                     // nothing analysed (clip kind, no picture)
    bool canceled = false;
};

// The game's screen for the templates, in the frame the clip is analysed in (window geometry is in % of the whole
// frame, bars included): the picture fitted into the timeline's frame, black bars baked into the file
// (engine/BarDetect, cached per file) and the shape the clip shows the file at (a stretched 4:3 recording put right,
// Clip::pictureAspect). Blocking the first time for a file (a dozen decoded pictures).
NameBlur::Screen screenOf(const ProjectFormat& format, const Clip& c);

QString cacheDir();
// The clip's picture (without its effects and transform, as the windows see it) sampled from start to end.
// progress: 0..1000
Result analyze(const ProjectFormat& format, const Clip& c, const std::atomic<bool>* cancel = nullptr,
               std::atomic<int>* progress = nullptr);

} // namespace TextRegions
