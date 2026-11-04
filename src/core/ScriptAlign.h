// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// A script against what was said (0.17 package "rough cut"): normalized words (case, umlauts, punctuation, number
// words, fillers), each script sentence found in the transcripts by a local word alignment (Smith-Waterman on words,
// fuzzy: a word whisper misheard by a letter or two still counts), the repeats of a sentence as takes, and a rating
// of every take (complete, no false start or extra words, few fillers and long pauses). Pure functions on words with
// seconds, no Qt beyond containers and strings: testable and the same on every platform. The edit ops rough_cut and
// best_takes (cli/EditOpsTalk.cpp) cut by it.

#include <QString>
#include <QStringList>
#include <QVector>

namespace ScriptAlign {

// A spoken word: its text as the transcript has it (punctuation attached), [from, to) in seconds of its source
struct Word {
    QString text;
    double from = 0, to = 0;
};

// "Größe," -> "groesse", "Zwanzig" -> "20", "äh" stays "aeh" (isFiller tells): lower case, umlauts and ß folded,
// everything but letters and digits removed, number words up to twenty (German, English) as digits
QString normalize(const QString& word);
bool isFiller(const QString& normalized); // äh, ähm, öh, hm, uh, um, uhm, erm …
// Similarity of two normalized words, 0..1 (1 = equal; by edit distance)
double similarity(const QString& a, const QString& b);

// The sentences of a script: split after . ! ? and at empty lines; each one's words normalized (fillers out)
struct Sentence {
    QString text;       // as written
    QStringList words;  // normalized
};
QVector<Sentence> sentences(const QString& script);

// One time a sentence was said: words [first, last] of its source
struct Take {
    int source = 0;
    int first = 0, last = 0;
    double from = 0, to = 0;  // seconds: start of the first word, end of the last
    double coverage = 0;      // share of the sentence's words found in it, 0..1
    int extra = 0;            // spoken words in it that are not in the sentence (a slip, a restart) - fillers aside
    int fillers = 0;
    int pauses = 0;           // pauses of more than a second inside it
    double score = 0;         // higher = better (see rate)
    bool good = false;        // complete enough (coverage >= 0.9), no extra word, no long pause (fillers are fine)
};

// Every take of `sentence` in `words` (one source), best alignment first, then the next one in the rest, as long as
// its coverage is at least minCoverage; sorted by time
QVector<Take> findTakes(const QStringList& sentence, const QVector<Word>& words, int source = 0, double minCoverage = 0.7);

enum class Pick { LastGood, Best };

// The rough cut: for each sentence of the script its takes in all sources and the one to use (chosen = index into
// takes, -1 = not found: `missing`). LastGood = the last good take (people say it again until it is right), else the
// best one; Best = the highest score. A take is not used twice (a sentence said once that two script sentences share
// goes to the first).
struct Choice {
    int sentence = 0;
    QVector<Take> takes;
    int chosen = -1;
    QString why;
};
QVector<Choice> roughCut(const QVector<Sentence>& script, const QVector<QVector<Word>>& sources, Pick pick);

// Without a script: the sentences of a transcript (split after . ! ?) said more than once in a row - a group of takes
// (the earlier ones may be false starts: a sentence that the next one says again from its start). keep = the take
// that stays (the last good one, else the best; each take is measured against the last one, unless that is a false
// start: a slip in a longer earlier take counts as extra words); the others are cut out by best_takes.
struct Repeat {
    QVector<Take> takes; // by time
    int keep = 0;
};
QVector<Repeat> repeats(const QVector<Word>& words, int source = 0, double minCoverage = 0.7);

} // namespace ScriptAlign
