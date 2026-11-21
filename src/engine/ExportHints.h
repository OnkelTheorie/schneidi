// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// What the timeline knows about the pictures of an export that an encoder cannot find out (or only by searching):
// schneidi puts every picture together itself. Made by Exporter::encodeHints, taken by the own H.264 encoder
// (ExportWriter -> schneidi::h264::EncodeHints).

#include <QRect>
#include <QVector>

// Output frames [from, to). still: nothing moves in the timeline there (stills, freeze frames, titles), so a picture
// probably repeats the one before - the encoder compares before it believes it. background: rectangles (output
// pixels) that show only the blurred fill around fitted clips.
struct ExportHintSpan {
    int from = 0, to = 0;
    bool still = false;
    QVector<QRect> background;
};
