// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Name search of the Media Pool and the Media Storage: every word of the query must be found (in any of the texts
// given: the name, a clip color, a format word …), case and accents do not count, separators are all the same (space,
// underscore, dash, dot … : "cs2 11-12" finds "cs2_clip_2026-11-12_21-30.mkv"), and a word of at least 4 characters
// with a letter in it may be off by one typo (one letter more, less or another: "clps" and "clipz" find "clips"). Words
// of digits only and words with separators in them ("11-12", "a.mp4") must match exactly (a date or a number one off
// is another one, "a.mp4" is not "b.mp4").
// Pure functions, thread-safe.

#include <QString>
#include <QStringList>
#include <QVector>

namespace NameMatch {

struct Range {
    int start = 0; // in the original text
    int length = 0;
    bool operator==(const Range&) const = default;
};

// Lower case, accents dropped (ä -> a, é -> e, ß -> ss), every run of characters that are neither letters nor digits
// one space, no space at the ends. `origin` (optional): for every character of the result its index in `text`
QString normalize(const QString& text, QVector<int>* origin = nullptr);

// The query's words (split at white space only, so "11-12" stays one word: its parts next to each other), each
// normalized; empty words dropped
QStringList queryWords(const QString& query);

// Where `word` (normalized) is in `hay` (normalized): exactly, else with one typo if the word allows it. start -1 = not
// found; length in `hay`
Range find(const QString& word, const QString& hay);

// Every word in at least one of `texts` (no words = everything matches)
bool matches(const QStringList& words, const QStringList& texts);
bool matches(const QStringList& words, const QString& text);

// Where the words are in `text` (original positions, sorted, overlaps merged) - for showing them; words not in it
// are left out
QVector<Range> ranges(const QStringList& words, const QString& text);

} // namespace NameMatch
