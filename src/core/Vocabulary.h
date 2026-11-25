// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The word list for speech recognition: names, slang and game terms whisper should spell as the user does. Given to
// whisper as part of its starting text (the prompt): it biases the spelling, it does not make whisper hear a word
// that is not said. The user's list lives in the settings (File > Settings, QSettings "transcribe/vocabulary"); a
// call can add words, and a known game adds a few of its terms (short/direct captions).

#include <QString>
#include <QStringList>

namespace Vocabulary {

// Words of a text: one per line or separated by commas (a phrase keeps its spaces), trimmed, each only once (case
// ignored), empty ones left out
QStringList parse(const QString& text);
// `a` then the words of `b` not in it yet (case ignored)
QStringList combine(const QStringList& a, const QStringList& b);

// The user's list (settings); empty = none
QStringList stored();
void setStored(const QStringList& words);

// A few terms of a game as players say and write them ("cs2", "valorant"); empty for others
QStringList gameTerms(const QString& game);

// whisper reads at most this many prompt tokens (half its text context of 448); more are cut by whisper itself
constexpr int kMaxPromptTokens = 224;
// A cautious count of whisper's tokens for a text (about 3 bytes of UTF-8 per token, every word at least one)
int estimateTokens(const QString& text);
// The starting text: `base` (the punctuation/verbatim prompt, may be empty) and the words after it ("…, AWP,
// Headshot."), as many words as fit in maxTokens in their order; *dropped = the words left out
QString prompt(const QString& base, const QStringList& words, int maxTokens = kMaxPromptTokens,
               QStringList* dropped = nullptr);
// For a cache key: empty for no words (old keys stay valid), else a short hash of the list
QString cacheId(const QStringList& words);

} // namespace Vocabulary
