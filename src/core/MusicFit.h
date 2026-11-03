// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// A song cut to a length (edit op `fit_music`): whole phrases (if need be bars) come out of the middle, the start and
// the real end (the last phrase and what rings out after it) stay, the parts join at bar ones whose sound is alike
// (level and bass of the bars around the cut, core/Beats) with a short equal-power crossfade. Pure computation on the
// result of the beat analysis; the caller decodes and places the clips.
//
// The beat grid is fixed, so every cut between two bar ones keeps the rhythm and only the start is free to move by
// less than a bar: what remains of the length below a bar is taken off the start before the first bar one (the
// silence or the pick-up there), or the song starts that much before a later bar one (intro bars dropped). Only a
// song without either starts inside its first bar (with a short fade-in, the answer says so).

#include "core/Beats.h"
#include "core/Types.h"

#include <QString>
#include <QVector>

#include <vector>

namespace MusicFit {

enum class End { Natural, Fade };

struct Options {
    double length = 0;      // target length (seconds)
    End end = End::Natural; // Fade: the song plays from its start and fades out at the length (no cut)
    bool keepDrop = false;  // the strongest drop must stay (otherwise only preferred)
    double climaxAt = -1;   // >= 0: the strongest drop lands here (seconds into the result)
    double crossfade = -1;  // seconds per cut; < 0 = automatic (2 beats where the sound changes, else ~40 ms)
};

// A part of the song in source seconds [from, to)
struct Segment {
    double from = 0, to = 0;
};

// A cut between two parts: where it lies in the result and in the song
struct Cut {
    double at = 0;       // seconds into the result
    double from = 0;     // song time where the part before it stops
    double to = 0;       // song time where the part after it starts
    int fromBar = 0, toBar = 0; // bar numbers (0 = the first bar one) of both
    bool phrase = false; // both are phrase starts (whole phrases removed)
    double levelDb = 0;  // how far the sound around the cut differs (dB, 0 = alike)
    double crossfade = 0; // seconds
};

struct Plan {
    bool ok = false;
    QString error;           // !ok: why (English, for the CLI)
    QString errorCode;       // TOO_SHORT, NO_FIT
    std::vector<Segment> segments;
    std::vector<Cut> cuts;
    double startTrim = 0;    // song seconds dropped before the first part (= segments.front().from)
    bool startInBar = false; // the start falls inside a bar (no room before a bar one): fadeIn is set
    double fadeIn = 0, fadeOut = 0; // seconds
    double dropAt = -1;      // the strongest drop in the result (seconds), -1 = none / cut out
    double dropSong = -1;    // ... and in the song
    double length = 0;       // of the result (seconds)
};

// songLength: the whole file (seconds); r: Beats::analyze of the whole file (times from its start)
Plan plan(const Beats::Result& r, double songLength, const Options& o);

// The parts as audio clips from timeline frame `at`: whole frames for the cuts, the 1/100 frame position
// (Clip::subframe) keeps every part on the beat grid of the one before; crossfades as transitions on both sides of
// each cut, fades as fade handles. `like` gives media, stream, volume …; ids are left 0. fileLength in frames.
QVector<Clip> clips(const Plan& p, const Clip& like, double fps, int at, int fileLength);

} // namespace MusicFit
