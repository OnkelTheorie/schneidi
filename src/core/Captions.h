// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Word captions (shorts/reels style): which word of a subtitle cue is spoken when, the pop-in of a word, line breaks,
// and cues built from transcript words (optionally one word per beat). Pure data functions; the picture engine draws
// them (engine/picture/Renderer, subtitle layer), the Editor/CLI create them.

#include "core/TranscriptCut.h"
#include "core/Types.h"

#include <QJsonObject>
#include <QSize>
#include <QStringList>
#include <QVector>

#include <functional>

namespace Captions {

// The words of a cue's text (split at white space and line breaks)
QStringList words(const QString& text);

// When each word of the cue is spoken (frames from cue.start, one per word of `words(text)`): the stored times when
// they fit the text, otherwise the cue's length shared by the words' lengths (an SRT import, an edited text)
QVector<CueWord> timing(const SubtitleCue& cue);

// Index of the word spoken at timeline frame `frame` (the last one that started; it stays until the next one starts),
// -1 = none yet or outside the cue
int activeWord(const SubtitleCue& cue, int frame);

// Pop-in of a word `ageMs` after it started: grows from 60 % with an overshoot (about 108 %) and fades in during the
// first half. popMs <= 0 = no animation.
struct Pop {
    double scale = 1.0, opacity = 1.0;
};
Pop popIn(double ageMs, int popMs);

// Line breaks for words of the given widths: a new line when the next word would make the line wider than
// `maxWidth` or when `maxWords` (> 0) are on it. Returns the index of each line's first word (always starts with 0
// when there are words).
QVector<int> lineStarts(const QVector<double>& widths, double spaceWidth, double maxWidth, int maxWords);

// The cue limited to [start, end) of the timeline: words that start outside are dropped with their text (only when
// the cue has its own word times; otherwise just the times change). Word times follow the new start.
void crop(SubtitleCue& cue, int start, int end);
// true when `after` (a crop/remap of `before`) lost every word of a word caption: its text is empty now
bool lostAllWords(const SubtitleCue& before, const SubtitleCue& after);
// The deltas a cue edge may move without dropping every word of a word caption (start edge: up to the last word's
// start; end edge: down to just after the first word's start); plain cues: no limit (INT_MIN/INT_MAX)
int maxStartDelta(const SubtitleCue& cue);
int minEndDelta(const SubtitleCue& cue);
// Every word time mapped through `pos` (timeline frame -> new timeline frame, e.g. a ripple), cue start/end set to
// newStart/newEnd; words that shrink to nothing are dropped with their text
void remap(SubtitleCue& cue, const std::function<int(int)>& pos, int newStart, int newEnd);

// Text style of a new word caption track for the format: big bold letters with an outline and no box; on a portrait
// picture above the part phone apps cover (ExportCheck's safe area), otherwise near the bottom. One word at a time
// is drawn larger.
TitleStyle defaultStyle(QSize format, CaptionMode mode = CaptionMode::Karaoke);
// Width the words of a caption line may take (pixels of the project format): narrower than plain subtitles so that
// the side buttons of phone apps do not cover them
double lineWidth(QSize format);

// Mode by id ("lines", "karaoke", "word"); unknown = nullptr
const CaptionModeInfo* modeInfo(const QString& id);
const CaptionModeInfo& modeInfo(CaptionMode mode);
// Project file / CLI: only the values that differ from the default (empty object = plain lines); reading takes
// missing keys from `base`, unknown modes (newer files) are plain lines
QJsonObject toJson(const CaptionStyle& c);
CaptionStyle fromJson(const QJsonObject& o, const CaptionStyle& base = {});

// ---- Building captions from words ----

struct Options {
    int maxWords = 4;        // per cue (short phrases read better on a phone)
    int maxGapFrames = 18;   // a longer pause starts a new cue
    int minFrames = 12;      // a cue stays at least this long (up to the next one)
    QVector<int> beats;      // timeline frames: each word starts on the next beat (one word per beat), empty = as spoken
};
// Words placed on beats: each word moves to the nearest beat after the beat of the word before (a word never shares
// a beat), and lasts until the next word's beat
QVector<TranscriptCut::Word> onBeats(const QVector<TranscriptCut::Word>& words, const QVector<int>& beats);
// Cues with word times from timeline words (TranscriptCut::timelineWords): a cue ends after a sentence, before a long
// pause or when it has maxWords; a comma after half of them is a good place too. ids stay 0.
QVector<SubtitleCue> fromWords(const QVector<TranscriptCut::Word>& words, const Options& options);

} // namespace Captions
