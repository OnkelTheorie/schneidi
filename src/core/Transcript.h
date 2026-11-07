// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Speech-to-text results (whisper.cpp, see engine/Transcriber): words with times, grouped into subtitle cues.
// Pure data functions; times in milliseconds from the start of the transcribed audio.

#include "core/Types.h"

#include <QByteArray>
#include <QString>
#include <QVector>

#include <functional>

namespace Transcript {

using Word = TranscriptWord;

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

// One cue per word (from its start to where it is spoken to, one CueWord; a word overlapping the next ends there),
// timeline frames with `offset` added: the input of Subtitles::readable with the word times
QVector<SubtitleCue> wordCues(const QVector<Word>& words, double fps, int offset = 0);

// Readable subtitles of the words (Subtitles::readable of wordCues, word times kept); not for music: then like
// startAtSound per cue (a cue without any sound is dropped, a start in silence moves to the sound) - per cue, so a
// quiet word between loud ones stays
QVector<SubtitleCue> readableCues(const QVector<Word>& words, const Levels& levels, double fps, int offset = 0,
                                  bool music = false);

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

// Which sound to transcribe when the user chose none. Game recordings (OBS, Clipi) keep game, voice chat, music and
// the microphone in streams of their own; whisper on the mix writes song lyrics into the captions.
// A stream or track name that says "own voice": microphone, voice, narration … ("Mikrofon", "Mic/Aux", "Voice");
// not a mix with it ("System + Mikrofon", "Mix (Mic)")
bool isVoiceName(const QString& name);
// Audio streams of a media file to transcribe: its voice-named streams (mixed if several), else {0} (the mix or the
// only stream)
QVector<int> defaultStreams(const MediaInfo& info);
// Audio tracks of a timeline to transcribe: the audible tracks with clips whose track name or audio stream names
// (`streamName` of a clip, may be empty) say voice, but only when that leaves other audible tracks out; empty = as
// heard (no voice track, or nothing else there)
QVector<int> voiceTracks(const Timeline& tl, const std::function<QString(const Clip&)>& streamName);

} // namespace Transcript
