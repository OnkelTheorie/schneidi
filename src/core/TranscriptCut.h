// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Text-based editing (Transcript panel, like DaVinci's transcription of timeline clips): the words of the media
// transcripts (MediaInfo::transcripts, source times) placed where their clips sit in the timeline, and the timeline
// ranges to cut out when words or pauses are removed. Pure data functions.

#include "core/Types.h"

#include <QPair>
#include <QVector>

namespace TranscriptCut {

struct Word {
    int from = 0, to = 0; // timeline frames [from, to): start of the word up to the next word (or the clip end)
    int spokenTo = 0;     // end of the spoken word (without the pause after it), <= to
    QString text;
    int clipId = 0;
    int track = 0; // audio track
    bool operator==(const Word&) const = default;
};

// Words of all audio clips (audioTrack < 0) or of one audio track, sorted by time. Only clips that play their
// source at normal speed (no speed change, ramp, freeze or reverse) and are enabled; a word counts if it starts
// inside the clip. The same word heard on two tracks (same file and stream) appears once.
QVector<Word> timelineWords(const Timeline& tl, const QVector<MediaInfo>& media, double fps, int audioTrack = -1);

// Clips on the track(s) whose source has no transcript yet: (media path, audio stream), each once
QVector<QPair<QString, int>> missingTranscripts(const Timeline& tl, const QVector<MediaInfo>& media, int audioTrack = -1);

// Timeline ranges [from, to) that remove the words `first`..`last` (indices into `words`) together with the pause
// after them (up to the next word when it follows within `maxPauseFrames`, otherwise a short tail)
QPair<int, int> cutRange(const QVector<Word>& words, int first, int last, int maxPauseFrames);

// Pauses between words longer than `minFrames`: the ranges to cut so that `keepFrames` of quiet stay (half after
// the word before, half before the next). Only between words that follow each other in the same clip.
QVector<QPair<int, int>> pauseRanges(const QVector<Word>& words, int minFrames, int keepFrames);

// Overlapping or touching ranges joined, sorted
QVector<QPair<int, int>> mergeRanges(QVector<QPair<int, int>> ranges);

} // namespace TranscriptCut
