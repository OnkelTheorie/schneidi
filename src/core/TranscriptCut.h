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
    QString language; // of the transcript it comes from ("de"; empty = unknown)
    bool operator==(const Word&) const = default;
};

// Words of all audio clips (audioTrack < 0) or of one audio track, sorted by time. Only clips that play their
// source at normal speed (no speed change, ramp, freeze or reverse) and are enabled; a word counts if it starts
// inside the clip. The same word heard on two tracks (same file and stream) appears once.
// retimed = also clips that play forward at another speed or with a speed ramp (word captions: the words follow
// the speed; cutting by text keeps to normal speed).
QVector<Word> timelineWords(const Timeline& tl, const QVector<MediaInfo>& media, double fps, int audioTrack = -1,
                            bool retimed = false);

// Clips on the track(s) whose source has no transcript yet: (media path, audio stream), each once
QVector<QPair<QString, int>> missingTranscripts(const Timeline& tl, const QVector<MediaInfo>& media, int audioTrack = -1);

// The parts of a (file, stream) the clips of `audioTrack` play, in ms of the file [from, to): each clip's source range
// (speed followed) padded by padMs, widened to whole gridMs steps (a cut moved a little asks for the same parts: the
// transcripts of them stay in the cache), parts less than joinMs apart joined. In file order.
QVector<QPair<qint64, qint64>> playedRangesMs(const Timeline& tl, double fps, int audioTrack, const QString& file,
                                              int stream, qint64 padMs = 1000, qint64 gridMs = 5000,
                                              qint64 joinMs = 10000);

// Timeline ranges [from, to) that remove the words `first`..`last` (indices into `words`) together with the pause
// after them (up to the next word when it follows within `maxPauseFrames`, otherwise a short tail)
QPair<int, int> cutRange(const QVector<Word>& words, int first, int last, int maxPauseFrames);

// Pauses between words longer than `minFrames`: the ranges to cut so that `keepFrames` of quiet stay (half after
// the word before, half before the next). Only between words that follow each other in the same clip.
QVector<QPair<int, int>> pauseRanges(const QVector<Word>& words, int minFrames, int keepFrames);

// Overlapping or touching ranges joined, sorted
QVector<QPair<int, int>> mergeRanges(QVector<QPair<int, int>> ranges);

} // namespace TranscriptCut
