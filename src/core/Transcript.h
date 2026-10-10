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

// Loudness of the transcribed audio: one dB value per `frameMs` (RMS), see levelsOf
struct Levels {
    int frameMs = 10;
    QVector<float> db;
};

struct Result {
    QString language; // detected or given, e.g. "de"
    QVector<Word> words;
    Levels levels; // of the audio whisper heard (engine/Transcriber), for startAtSound
};

// whisper-cli JSON output (-oj) of a run with one word per segment (-ml 1 -sow). false = not readable.
bool parseWhisperJson(const QByteArray& json, Result* out, QString* error = nullptr);

// Words -> subtitle cues (timeline frames, `offset` added): a cue ends after a sentence, before a pause longer than
// `maxGapMs` or before it would get longer than `maxChars`; long pauses are not shown (a word lasts at most
// about its spoken length); every cue stays long enough to read, up to the next one. ids stay 0
// (Editor::importSubtitles assigns them).
QVector<SubtitleCue> toCues(const QVector<Word>& words, double fps, int maxChars = 42, int maxGapMs = 700, int offset = 0);

// End of a word without the pause after it (whisper stretches `to` to the next word)
qint64 spokenEnd(const Word& w);

Levels levelsOf(const qint16* samples, qint64 count, int sampleRate, int frameMs = 10);

// Whisper often stretches words over the pause or unrecognised sound (laughter) before them ("Das ist scheiße"
// 6.5–15.4 s, spoken from 13.9 s); subtitles then start far too early. Runs of words longer than their spoken length
// move up to the word that follows them directly (or stay after the one before); a run alone between pauses goes to
// the loudest stretch of `levels` (may be empty: then it keeps its start). Words that start together with a run
// (whisper gave them no time) go along with it.
void fitToSpeech(QVector<Word>& words, const Levels& levels);

// Cues of several speakers (one transcript per audio track) as one track: sorted; cues that overlap become one cue
// with a line per speaker (start of the first, end of the last)
QVector<SubtitleCue> mergeCues(const QVector<QVector<SubtitleCue>>& tracks);

// A cue that whisper starts in silence ("Pferd, geh …" 1.1 s before the voice) begins shortly before the sound;
// a cue without any sound is dropped (whisper makes up words in silence). Cues in timeline frames, `offset` =
// timeline frame of the audio start, as in toCues
void startAtSound(QVector<SubtitleCue>& cues, const Levels& levels, double fps, int offset = 0);

} // namespace Transcript
