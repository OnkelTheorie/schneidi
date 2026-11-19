// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Filling a template's clip slots by itself (`templates fill auto:true`, the recipe "template"): for each slot the
// best shot of the material index (engine/MediaIndex) - long enough, sharp, well exposed, a face, and what the slot's
// label and hint ask for (core/IndexSearch). Each shot is used once. A slot no shot is long enough for takes the
// longest one left; without index data (shots of unknown quality) the material goes in in its order. Pure functions.

#include "core/IndexSearch.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace SlotPick {

struct Slot {
    QString id;
    QString label, hint;
    double minS = 0; // the shortest shot that fills it (seconds; 0 = any): the longest of its instances
};

struct Shot {
    IndexSearch::Shot shot; // file, n, from/to (seconds), what the search sees
    double sharp = -1;      // 0..1, < 0 = not measured
    double bright = -1;     // mean luma 0..1, < 0 = not measured
    int faces = -1;         // faces found (YuNet), -1 = not measured
    bool video = true;      // has a picture (a clip slot needs one)
    double length() const { return shot.to - shot.from; }
};

// Why a shot was taken: reason key (core/Direction's table: "slot_match") and what made it
struct Pick {
    QString slot;
    int shot = -1;          // index into the shots; -1 = nothing left for this slot
    double score = 0;
    QStringList matched;    // the words of label/hint the shot matched ("laden", "laughter")
    QStringList quality;    // "sharp", "face", "well exposed" … (what counted for it)
    bool shortShot = false; // shorter than minS (nothing long enough was left)
    bool inOrder = false;   // no index data: the next shot of the material
    QString why;            // English: "matches 'laden' (desc); sharp, face"
};

// Quality points of a shot (sharpness, exposure, a face; blurry/dark/overexposed count against it); 0 when unmeasured
double quality(const Shot& s, QStringList* what = nullptr);

// One pick per slot, in the slots' order; no shot twice. First the slots whose label/hint something matches (the
// best match first), then the others in their order by quality
QVector<Pick> pick(const QVector<Slot>& slotList, const QVector<Shot>& shots);

} // namespace SlotPick
