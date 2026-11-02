// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Text to speech ("Sprechertext"): the pure parts - which file a text becomes (cache key), where it lies, the piper
// command line, word times when no recogniser measured them, and where the clip goes on the timeline. The program
// itself runs in engine/Tts.

#include "core/Types.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace Speech {

struct Options {
    QString voice;        // voice id ("piper-voice-de_DE-thorsten"), short name or .onnx path; empty = default voice
    double speed = 1.0;   // 0.5..2: 2 = twice as fast (piper length_scale = 1 / speed)
    double pause = 0.2;   // seconds of silence after each sentence
};
constexpr double kMinSpeed = 0.5, kMaxSpeed = 2.0, kMaxPause = 3.0;

// Whitespace (line breaks too) collapsed to single spaces, trimmed: what piper speaks (it reads one line per
// utterance; several lines would each overwrite the output file)
QString normalizedText(const QString& text);
// Same text + voice + options = same key (16 hex digits); so a second run takes the file that is there
QString cacheKey(const QString& text, const Options& options);
// "speech-hallo-das-ist-<key>.wav": the first words (ASCII, for any file system) and the key
QString fileName(const QString& text, const Options& options);
// Folder for the speech files of a project: "<project folder>/<project name>-tts" next to a saved project, else
// "tts" in the app's data folder (an unsaved project has no folder; the files must outlive any cache)
QString folder(const QString& projectPath);

// Arguments of the piper program for one file (text comes on stdin, see stdinText)
QStringList piperArguments(const QString& model, const Options& options, const QString& outFile);
QByteArray stdinText(const QString& text); // UTF-8, one line

// Language of a voice ("de_DE-thorsten", a model file "en_US-ljspeech-medium.onnx" -> "de" / "en"); empty = unknown
QString voiceLanguage(const QString& voice);

// Word times without a recogniser: the words share [speechFromMs, speechToMs) by their length in characters, a
// sentence end weighs like `pauseMs` of silence after it (piper's sentence pause). Rough, but the right order and
// pace for word captions.
QVector<TranscriptWord> estimateWords(const QString& text, qint64 speechFromMs, qint64 speechToMs, qint64 pauseMs);

// Audio track for a clip in [from, to): `preferred` if it is free there (>= 0), else the first free one, else a new
// one (= audio.size()). Locked tracks are never free.
int freeAudioTrack(const Timeline& timeline, int from, int to, int preferred = -1);

} // namespace Speech
