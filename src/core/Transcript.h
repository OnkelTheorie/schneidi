// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Speech-to-text results (whisper.cpp, see engine/Transcriber): words with times, grouped into subtitle cues.
// Pure data functions; times in milliseconds from the start of the transcribed audio.

#include "core/Subtitles.h"
#include "core/Types.h"

#include <QByteArray>
#include <QHash>
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
    int dropped = 0; // words of made-up lines removed (dropHallucinations) or heard in silence
};

// whisper-cli JSON output (-oj) of a run with one word per segment (-ml 1 -sow). false = not readable.
// With the full output (-ojf) of a run with DTW token times (-dtw), a word starts at its first token's DTW time: far
// closer to the sound than whisper's own segment times (which hand a pause or a laugh before a sentence to its first
// word).
bool parseWhisperJson(const QByteArray& json, Result* out, QString* error = nullptr);
// The same, appended to out->words; `map` turns whisper's times (ms in the piece of sound it heard) into times of the
// whole sound (speechPieces). The words stay in time order (pieces in order). out->language is set by the first
// piece that names one.
// map(t, end): `end` = the time is a word's end (a cut-short pause maps to the stretch before it, a start to the one
// after it).
bool appendWhisperJson(const QByteArray& json, const std::function<qint64(qint64, bool)>& map, Result* out,
                       QString* error = nullptr);

// parakeet-cli's log with token times (-ps, on stderr), files read one after the other: per file as it names it
// ("Processing file: piece_000.wav") its words, in time order (ms from the file's start). A token that starts a word
// ("▁Hallo") begins one, the others ("ür", ",", ".") belong to the word before; a word lasts from its first token's
// start to its last token's end (10 ms steps).
QHash<QString, QVector<Word>> parseParakeetLog(const QString& log);

// A piece of sound for whisper: stretches of the whole sound (ms) one after the other, `at` = where a stretch starts
// in the piece; between two stretches the piece is silent (a long pause cut short).
struct PieceSpan {
    qint64 at = 0, from = 0, length = 0;
};
using Piece = QVector<PieceSpan>;
qint64 pieceLength(const Piece& piece);
// Time in the piece -> time in the whole sound; in a cut-short pause: the start of the stretch after it, or for an
// end (`end`) the end of the stretch before it (a word's end there would else last over the whole real pause)
qint64 pieceTime(const Piece& piece, qint64 t, bool end = false);

// The pieces whisper works on one at a time: the speech segments (ms, from voice detection), each padded by up to
// `padMs` (never past the middle of the pause to its neighbour); a pause up to `maxGapMs` stays as it is, a longer one
// shrinks to `silenceMs` of silence; a piece is filled with whole segments up to `maxMs` (never cut inside a segment:
// a word across the cut was lost). Within whisper's 30 s window: a longer sound it transcribes in several windows and
// skipped a sentence at a window's end now and then, and every piece costs a whole window however short (packing
// them: 3 min of chat in 5 pieces instead of 33). Only a single segment longer than `maxMs` is split (the voice
// detection already splits at 25 s).
QVector<Piece> speechPieces(const QVector<QPair<qint64, qint64>>& segments, qint64 lengthMs, qint64 maxGapMs = 1000,
                            qint64 maxMs = 28000, qint64 padMs = 200, qint64 silenceMs = 400);

// Words that only repeat whisper's starting text (`prompt`, e.g. "Hallo, wie geht's?" in silence): removed; returns
// how many
int dropPromptEcho(QVector<Word>& words, const QString& prompt);

// The echo of whisper's starting text as Transcriber drops it: `base` is the starting text schneidi writes (the
// punctuation sentence), `full` that with the word list after it. An echo of the whole starting text or of `base`
// goes; the word list alone never (a list with "Onkel Theo" would delete every spoken "Onkel Theo"), so without a
// base nothing is dropped. Returns how many words went.
int dropStartingTextEcho(QVector<Word>& words, const QString& base, const QString& full);

// whisper (turbo above all) now and then writes a piece's whole text twice ("A B C. A B C.", 3 of 69 German FLEURS
// pieces, measured 22.11.) or writes the piece before it once more (its text is the context of the next piece).
// words[first..] are the words of one piece, words[previous..first) those of the piece before it (previous < 0 =
// none). A copy is dropped when it has at least `minWords` words, three of them different ("ja ja ja ja" said twice
// stays), and differs from what it repeats in at most a fifth of its words. Of a piece written twice the first copy
// stays with its times, its last word lasting to the copy's end - only when the copy is squeezed into the end of the
// piece (its words start within a quarter of the first copy's time), as whisper writes it; said twice for real, it
// stays. A piece that only repeats the one before goes whole. Pieces (and pieces before) of more than 300 words are
// left as they are (speech pieces are ~100 at most). For whisper's speech pieces only: not for music (a chorus
// sung twice), not for Parakeet (it does not repeat; its blind 28 s pieces can hold a real repeat). Returns how many
// words were dropped.
int dropRepeats(QVector<Word>& words, qsizetype first, qsizetype previous = -1, int minWords = 4);


// Words -> subtitle cues (timeline frames, `offset` added): a cue ends after a sentence, before a pause longer than
// `maxGapMs` or before it would get longer than `maxChars`; long pauses are not shown (a word lasts at most
// about its spoken length); every cue stays long enough to read, up to the next one. ids stay 0
// (Editor::importSubtitles assigns them).
// `cutsMs`: cuts between clips (ms of the transcribed sound, cutsMs below): no cue spans one, none lasts past it.
QVector<SubtitleCue> toCues(const QVector<Word>& words, double fps, int maxChars = 42, int maxGapMs = 700, int offset = 0,
                            const QVector<qint64>& cutsMs = {});

// One cue per word (from its start to where it is spoken to, one CueWord; a word overlapping the next ends there),
// timeline frames with `offset` added: the input of Subtitles::readable with the word times
QVector<SubtitleCue> wordCues(const QVector<Word>& words, double fps, int offset = 0);

// Readable subtitles of the words (Subtitles::readable of wordCues with `rules`, word times kept); not for music: then like
// startAtSound per cue (a cue without any sound is dropped, a start in silence moves to the sound) - per cue, so a
// quiet word between loud ones stays
// `cutsMs` as in toCues: the words between two cuts are made readable on their own.
QVector<SubtitleCue> readableCues(const QVector<Word>& words, const Levels& levels, double fps, int offset = 0,
                                  bool music = false, const Subtitles::ReadableRules& rules = {},
                                  const QVector<qint64>& cutsMs = {});

// Cuts of a timeline range [from, to) (frames) on the audio tracks `tracks` (empty = every track not muted; then, when
// the range has clips linked to a picture, only theirs: a music bed or an effect sound ending under the speech is no
// cut of the speech): where a clip begins or ends inside the range, not where one only continues the one before
// (same file, the next source frame: a clip split in two). Sorted, each once. For transcripts of a timeline: a subtitle never starts in the clip
// before its words' clip (user report 23.11.: the next clip's line already showed at the end of the one before).
QVector<int> cutFrames(const Timeline& tl, int from, int to, const QVector<int>& tracks = {});
// cutFrames as ms of the sound transcribed from `from` on (the words' times)
QVector<qint64> cutsMs(const QVector<int>& cutFrames, int from, double fps);
// Words at cuts (ms): a word whose spoken part (spokenEnd) crosses a cut belongs to the side with more of it (whisper
// and the voice detection's padding start the first word after a cut a little early) - after it, it starts at the
// cut; before it, it ends there. Any word ends at the next cut after its start.
void fitToCuts(QVector<Word>& words, const QVector<qint64>& cutsMs);

// End of a word without the pause after it (whisper stretches `to` to the next word)
qint64 spokenEnd(const Word& w);

Levels levelsOf(const qint16* samples, qint64 count, int sampleRate, int frameMs = 10);

// No audible sound at all (every level below -60 dB; no levels = unknown, not silent): a muted track or a disabled stream, not a
// quiet voice; whisper then makes up words ("Untertitelung des ZDF")
bool isSilent(const Levels& levels);

// Whisper's well-known made-up lines from the TV subtitles it learned from ("Untertitelung des ZDF, 2020",
// "Untertitel der Amara.org-Community", "Subtitles by …") are removed wherever they are; the polite endings ("Vielen
// Dank fürs Zuschauen", "Thanks for watching") only where `levels` are quiet under them (said for real they stay).
// Returns how many words were removed.
int dropHallucinations(QVector<Word>& words, const Levels& levels);

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

// A cue that lasts on into silence (the reading minimum of toCues/readable, whisper's last word stretched over the
// pause: "Und ich hasse Mathe." shown 0.5-0.9 s after the voice stopped) ends `holdMs` after its sound: the last
// 30 ms of sound within 30 dB of the cue's own loud part (a soft ending stays). Never shorter than `minMs`, never
// longer than before; a cue whose sound runs to its end (the next speaker, laughter) keeps it. Same frames as
// startAtSound.
void endAtSound(QVector<SubtitleCue>& cues, const Levels& levels, double fps, int offset = 0, int holdMs = 200,
                int minMs = 600);

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
