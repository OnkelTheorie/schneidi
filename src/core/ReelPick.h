// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Which moments go into a highlight video (`direct` recipe highlights): the best ones up to a target length (or all
// of them), not the same scene three times (a minimum spacing per recording), in the order best first or as they
// happened, and with the cuts between them on the beat of the music when its grid is known (each moment's end moved
// by at most `snapS` so the cut lands on the nearest beat of the result timeline). When the moments found fall short of
// the target, each grows (more build-up and reaction, up to growBeforeS/growAfterS). Pure function on seconds.

#include "core/Beats.h"
#include "core/ShortPick.h"

#include <QVector>

#include <optional>

namespace ReelPick {

using Moment = ShortPick::Moment;

enum class Order { Best, Time };

struct Options {
    double targetS = 60;     // length of the video; 0 = every moment (after spacing)
    Order order = Order::Best;
    double minMomentS = 4;   // a shorter moment grows to this (build-up and reaction)
    double joinGapS = 1.5;   // moments of one recording closer than this become one
    double spacingS = 8;     // two moments of one recording closer than this (start to start): only the better one
    // Fewer moments than the target length: each picked one grows by up to this much build-up before and reaction
    // after (all of them by the same share of their room, never past a recording's ends or into a neighbour) - more
    // of the real scene instead of filler. 0 = never
    double growBeforeS = 6;
    double growAfterS = 4;
    std::optional<Beats::Result> beats; // the music's grid on the result timeline (0 = its start)
    double snapS = 0.25;     // farthest a cut moves onto the beat
};

// candidates: any order; durations: seconds of each recording. Result: in the result's order, no overlaps.
QVector<Moment> pick(const QVector<Moment>& candidates, const QVector<double>& durations, const Options& o = {});

} // namespace ReelPick
