// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Smart render (plan 0.11): where the export shows a file as it is (one clip on top, no effect, no transform, no
// transition, no title or subtitle over it), the writer copies the file's compressed pictures instead of encoding
// them again: lossless and many times faster. This part only reads the timeline; whether a file really fits
// (codec, size, frame rate, color, key frames) the writer decides (engine/ExportWriter), encoding what does not.

#include "core/Types.h"

#include <QSize>
#include <QString>
#include <QVector>
#include <cstdint>

namespace SmartRender {

// Output frames [from, to) show the frames sourceFrame, sourceFrame + 1 … of file, unchanged
struct Span {
    int from = 0, to = 0;
    QString file;
    int64_t sourceFrame = 0;
    bool operator==(const Span&) const = default;
};

// Spans of the export range [first, first + length) (timeline frames) that could be copied, sorted, output frames
// counted from first. Hidden tracks and disabled clips count as not there; burnSubtitles: frames with a subtitle stay
// encoded. Only for exports in the timeline's own size and frame rate (the caller checks).
QVector<Span> spans(const Timeline& tl, int first, int length, bool burnSubtitles);

// Color standard (MLT number, 601 or 709) of most frames among the spans whose files have the encoder's codec and the
// output size; 0 = none such. Opens the files (slow, not in the GUI thread).
int colorspace(const QVector<Span>& spans, const QString& videoCodec, QSize size);

// Wanted? SCHNEIDI_SMART_RENDER=off switches it off for tests and comparisons
bool enabled();

} // namespace SmartRender
