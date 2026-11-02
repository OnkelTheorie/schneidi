// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Hide player names automatically: from text-like regions found on sampled frames of a clip (engine/TextRegions) to
// masks over time, which become windows of the clip's grade (core/GradeWindows) with a blur or pixelate fill.
// Without OCR the text itself is unknown: a template decides which parts of the picture count (a game's kill feed,
// chat, name tags in the 3D view) and which never do (ammo, health, timer, crosshair). Static zones (kill feed,
// chat) get one window each that grows and shrinks with the text in them; text in a moving zone (name tags above
// heads) is followed from sample to sample, every track becomes a window path. Pure functions.

#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cmath>

namespace NameBlur {

enum class Template { Cs2, Valorant, All };
const char* templateId(Template t);
bool templateFromId(const QString& id, Template* t);
QStringList templateIds();

// Where the game's picture lies in the clip's picture and what shape the game drew its HUD for. The templates are
// measured on a 16:9 screen; a HUD keeps its size with the screen height and sticks to the left edge, the middle or
// the right edge, so on a 4:3 screen (stretched to 16:9 by the recorder, or between black bars) every zone keeps its
// distance to its edge in heights.
struct Screen {
    double aspect = 16.0 / 9.0;     // width / height of the game's screen
    QRectF content{0, 0, 1, 1};     // the game picture in the clip's picture (0..1; less with black bars)
    bool isDefault() const { return std::abs(aspect - 16.0 / 9.0) < 0.01 && content == QRectF(0, 0, 1, 1); }
};

// A part of the picture (0..1 of width/height, origin top left)
struct Zone {
    const char* name; // id for notes ("killfeed", "chat", "view")
    QRectF area;
    bool moving;      // text there moves (name tags above heads): tracked; else one window for the zone
    bool always = false; // hidden over the whole clip, found or not (a name the detection misses on its plate)
    // Narrower text (0..1 of the width) in this zone never counts, in no other zone either: weapon icons and
    // short labels (CS2 weapon list), while a long line there may end in a name ("… von <name>")
    double minWidth = 0;
};
struct Layout {
    QVector<Zone> include;   // text whose center lies in one of these counts (first match)
    QVector<QRectF> exclude; // text whose center lies here never counts (checked first)
};
// The template's zones on `screen` (0..1 of the clip's picture)
Layout layout(Template t, const Screen& screen = {});

// Text regions found on one sampled frame
struct Sample {
    int frame = 0;          // clip frame (0 = first frame of the clip)
    QVector<QRectF> boxes;  // 0..1 of the picture
};

// A mask over time: rectangles at clip frames, linear in between; `on` false = nothing hidden there
struct Key {
    int frame = 0;
    bool on = false;
    QRectF rect; // 0..1 of the picture, padded; empty when off
};
struct Mask {
    QString zone;      // the zone it comes from
    bool moving = false;
    QVector<Key> keys; // sorted by frame
};

struct Options {
    int maxMasks = 8;       // windows available on the clip
    double padding = 0.4;   // added around a text box on every side, in box heights
    double samplesPerSecond = 30; // of the samples given to plan()
    // HUD text stands still on the screen while the world behind it moves: text in a static zone counts when it is
    // at the same place on most samples of the half second around it (the floor behind the chat does not)
    double steadyShare = 0.6;
    // Moving text (name tags) must be followed this long, found on most samples of its time and keep its size
    double minTrackSeconds = 0.5;
    Screen screen;
};

// Masks for a clip of `length` frames from its samples (sorted by frame). dropped = moving tracks left out because
// no window was free.
QVector<Mask> plan(const QVector<Sample>& samples, Template t, int length, const Options& o = {}, int* dropped = nullptr);

// Rectangle of a mask at clip frame t (interpolated like linear keyframes); empty = off
QRectF rectAt(const Mask& m, int t);

} // namespace NameBlur
