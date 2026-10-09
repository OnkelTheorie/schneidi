#pragma once
// Speech-to-text results (whisper.cpp, see engine/Transcriber): words with times, grouped into subtitle cues.
// Pure data functions; times in milliseconds from the start of the transcribed audio.

#include "core/Types.h"

#include <QByteArray>
#include <QString>
#include <QVector>

namespace Transcript {

struct Word {
    qint64 from = 0, to = 0; // ms; `to` reaches up to the next word (whisper counts pauses to the word before)
    QString text;            // without leading space, punctuation attached ("Americans,")
    bool operator==(const Word&) const = default;
};

struct Result {
    QString language; // detected or given, e.g. "de"
    QVector<Word> words;
};

// whisper-cli JSON output (-oj) of a run with one word per segment (-ml 1 -sow). false = not readable.
bool parseWhisperJson(const QByteArray& json, Result* out, QString* error = nullptr);

// Words -> subtitle cues (timeline frames, `offset` added): a cue ends after a sentence, before a pause longer than
// `maxGapMs` or before it would get longer than `maxChars`; long pauses are not shown (a word lasts at most
// about its spoken length). ids stay 0 (Editor::importSubtitles assigns them).
QVector<SubtitleCue> toCues(const QVector<Word>& words, double fps, int maxChars = 42, int maxGapMs = 700, int offset = 0);

// End of a word without the pause after it (whisper stretches `to` to the next word)
qint64 spokenEnd(const Word& w);

} // namespace Transcript
