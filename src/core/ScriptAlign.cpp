// SPDX-License-Identifier: AGPL-3.0-only
#include "core/ScriptAlign.h"

#include <QHash>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace ScriptAlign {

namespace {

constexpr double kMatch = 0.75;   // words this similar are the same word (whisper hears "Schnitt" as "Schnit")
constexpr double kPauseS = 1.0;   // a pause inside a take longer than this counts against it
constexpr double kJoinS = 0.35;   // a word this close to the take's end is still said in one go
constexpr double kStopS = 0.3;    // a pause this long after a word: the speaker stopped there

const QHash<QString, QString>& numberWords()
{
    static const QHash<QString, QString> map = [] {
        QHash<QString, QString> m;
        const char* de[] = {"null", "eins", "zwei", "drei", "vier", "fuenf", "sechs", "sieben", "acht", "neun", "zehn",
                            "elf", "zwoelf", "dreizehn", "vierzehn", "fuenfzehn", "sechzehn", "siebzehn", "achtzehn",
                            "neunzehn", "zwanzig"};
        const char* en[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
                            "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen",
                            "nineteen", "twenty"};
        for (int i = 0; i <= 20; ++i) {
            m.insert(QString::fromLatin1(de[i]), QString::number(i));
            m.insert(QString::fromLatin1(en[i]), QString::number(i));
        }
        m.insert("ein", "1");
        m.insert("eine", "1");
        // Tens and their compounds up to 99 ("einundzwanzig", "twenty-one" = "twentyone"), and a hundred
        const char* deTens[] = {"zwanzig", "dreissig", "vierzig", "fuenfzig", "sechzig", "siebzig", "achtzig", "neunzig"};
        const char* enTens[] = {"twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};
        for (int t = 0; t < 8; ++t) {
            const int tens = (t + 2) * 10;
            m.insert(QString::fromLatin1(deTens[t]), QString::number(tens));
            m.insert(QString::fromLatin1(enTens[t]), QString::number(tens));
            for (int u = 1; u <= 9; ++u) {
                const QString deUnit = u == 1 ? QString("ein") : QString::fromLatin1(de[u]);
                m.insert(deUnit + "und" + QString::fromLatin1(deTens[t]), QString::number(tens + u));
                m.insert(QString::fromLatin1(enTens[t]) + QString::fromLatin1(en[u]), QString::number(tens + u));
            }
        }
        m.insert("hundert", "100");
        m.insert("hundred", "100");
        return m;
    }();
    return map;
}

int editDistance(const QString& a, const QString& b)
{
    QVector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (int j = 0; j <= b.size(); ++j) prev[j] = j;
    for (int i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (int j = 1; j <= b.size(); ++j)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

bool endsSentence(const QString& text)
{
    QString t = text.trimmed();
    while (!t.isEmpty() && (t.endsWith('"') || t.endsWith(QChar(0x201c)) || t.endsWith(QChar(0x00bb)) || t.endsWith(')')))
        t.chop(1);
    return t.endsWith('.') || t.endsWith('!') || t.endsWith('?') || t.endsWith(QChar(0x2026));
}

// The best local alignment of `sentence` in words [lo, hi) of `norm` (normalized, fillers already left out)
struct Hit {
    double score = 0;
    int first = -1, last = -1; // into norm
    int matched = 0, extra = 0;
    int missingEnd = 0; // sentence words after the last one aligned (not heard as written: "9er" for "Neuner")
};

Hit align(const QStringList& sentence, const QStringList& norm, int lo, int hi)
{
    const int n = int(sentence.size()), m = hi - lo;
    Hit best;
    if (n == 0 || m <= 0) return best;
    // H[i][j]: best score of an alignment ending at sentence word i, spoken word j; with the counts to trace back
    struct Cell {
        double h = 0;
        int start = -1, matched = 0, extra = 0;
    };
    QVector<Cell> prev(m + 1), cur(m + 1);
    for (int i = 1; i <= n; ++i) {
        cur[0] = Cell{};
        for (int j = 1; j <= m; ++j) {
            const double sim = similarity(sentence[i - 1], norm[lo + j - 1]);
            Cell c; // 0 = start anew
            const Cell& d = prev[j - 1];
            const double diag = d.h + (sim >= kMatch ? 2 * sim : -1.0);
            if (diag > c.h) c = Cell{diag, d.start < 0 ? lo + j - 1 : d.start, d.matched + (sim >= kMatch ? 1 : 0),
                                     d.extra + (sim >= kMatch ? 0 : 1)};
            const Cell& up = prev[j]; // a sentence word not said
            if (up.h - 1 > c.h) c = Cell{up.h - 1, up.start, up.matched, up.extra};
            const Cell& left = cur[j - 1]; // a word said that is not in the sentence
            if (left.h - 1 > c.h) c = Cell{left.h - 1, left.start, left.matched, left.extra + 1};
            if (c.h <= 0) c = Cell{};
            cur[j] = c;
            if (c.h > best.score && c.start >= 0) best = Hit{c.h, c.start, lo + j - 1, c.matched, c.extra, n - i};
        }
        std::swap(prev, cur);
    }
    return best;
}

void rate(Take& t)
{
    t.score = 10 * t.coverage - 1.0 * t.extra - 0.5 * t.fillers - 1.0 * t.pauses;
    t.good = t.coverage >= 0.9 && t.extra == 0 && t.pauses == 0;
}

// The words without fillers, and where each one is in `words`
void filtered(const QVector<Word>& words, int from, int to, QStringList* norm, QVector<int>* index)
{
    for (int i = from; i < to; ++i) {
        const QString w = normalize(words[i].text);
        if (w.isEmpty() || isFiller(w)) continue;
        *norm << w;
        *index << i;
    }
}

Take takeOf(const QVector<Word>& words, int source, int first, int last, double coverage, int extra)
{
    Take t;
    t.source = source;
    t.first = first;
    t.last = last;
    t.from = words[first].from;
    t.to = words[last].to;
    t.coverage = coverage;
    t.extra = extra;
    for (int i = first; i <= last; ++i) {
        if (isFiller(normalize(words[i].text))) ++t.fillers;
        if (i > first && words[i].from - words[i - 1].to > kPauseS) ++t.pauses;
    }
    rate(t);
    return t;
}

int pickTake(const QVector<Take>& takes, Pick pick, QString* why)
{
    if (takes.isEmpty()) return -1;
    int best = 0;
    for (int i = 1; i < takes.size(); ++i)
        if (takes[i].score > takes[best].score) best = i;
    if (pick == Pick::LastGood)
        for (int i = int(takes.size()) - 1; i >= 0; --i)
            if (takes[i].good) {
                if (why)
                    *why = takes.size() == 1 ? QString("said once")
                                             : QString("take %1 of %2: the last complete one").arg(i + 1).arg(takes.size());
                return i;
            }
    if (why) {
        const Take& t = takes[best];
        *why = takes.size() == 1 ? QString("said once") : QString("take %1 of %2: the best one").arg(best + 1).arg(takes.size());
        if (!t.good)
            *why += QString(" (%1 % of the words%2%3)")
                        .arg(int(std::lround(t.coverage * 100)))
                        .arg(t.extra ? QString(", %1 extra").arg(t.extra) : QString())
                        .arg(t.pauses ? QString(", %1 long pause%2").arg(t.pauses).arg(t.pauses == 1 ? "" : "s") : QString());
    }
    return best;
}

} // namespace

QString normalize(const QString& word)
{
    QString s = word.toLower();
    s.replace(QChar(0x00e4), "ae").replace(QChar(0x00f6), "oe").replace(QChar(0x00fc), "ue").replace(QChar(0x00df), "ss");
    QString out;
    for (const QChar c : s)
        if (c.isLetterOrNumber()) out += c;
    out = out.normalized(QString::NormalizationForm_KD);
    QString plain; // accents off (é -> e)
    for (const QChar c : out)
        if (c.category() != QChar::Mark_NonSpacing) plain += c;
    if (const auto it = numberWords().constFind(plain); it != numberWords().constEnd()) return it.value();
    // A number with the German "-er" ("Neuner", "Zwanziger") is written "9er" just as often: both the same word
    if (plain.size() > 2 && plain.endsWith(QLatin1String("er"))) {
        const QString base = plain.chopped(2);
        if (const auto it = numberWords().constFind(base); it != numberWords().constEnd() && base != QLatin1String("ein"))
            return it.value() + "er";
    }
    return plain;
}

bool isFiller(const QString& w)
{
    static const QStringList fillers{"aeh", "aehm", "oeh", "hm", "hmm", "uh", "uhm", "um", "umm", "erm", "eh", "ehm", "mh"};
    return fillers.contains(w);
}

namespace {

// "eeeh" -> "eh", "hmmm" -> "hm": letters said longer are the same sound
QString collapsed(const QString& w)
{
    QString s = w;
    for (const char* pair : {"ae", "oe", "ue"}) // "ääh" was folded to "aeaeh"
        while (s.contains(QLatin1String(pair) + QLatin1String(pair))) s.replace(QLatin1String(pair) + QLatin1String(pair), pair);
    QString out;
    for (const QChar c : s)
        if (out.isEmpty() || out.back() != c) out += c;
    return out;
}

// Hesitation sounds per UI language, as written (folded with normalize at first use). Only sounds, no words that carry
// meaning ("also", "halt", "like", "este", "ну", "あの" stay), and no answers that mean yes ("mhm", 嗯, 어): a cut must
// not take out speech. "um" (German, Portuguese: a word) and "eh" (Portuguese "é") count there only when held.
const QHash<QString, QStringList>& exactSets()
{
    static const QHash<QString, QStringList> sets = [] {
        const QHash<QString, QVector<const char*>> raw{
            {"de", {"äh", "ähm", "öh", "öhm", "ehm", "hm", "hmm", "erm"}},
            {"en", {"uh", "uhm", "um", "umm", "erm", "uhh", "hm", "hmm", "eh", "ehm"}},
            {"fr", {"euh", "euhm", "hm", "hmm"}},
            {"es", {"eh", "ehm", "em", "hm", "hmm"}},
            {"it", {"eh", "ehm", "uhm", "hm", "hmm"}},
            {"pt", {"ahn", "ehm", "hm", "hmm"}},
            {"nl", {"eh", "ehm", "uh", "uhm", "hm", "hmm"}},
            {"pl", {"yyy", "eee", "emm", "hm", "hmm", "eh", "ehm"}},
            {"tr", {"ııı", "ıı", "eeh", "hm", "hmm"}},
            {"id", {"eh", "ehm", "hm", "hmm"}},
            {"ru", {"э", "ээ", "эээ", "эм", "эмм", "хм", "хмм", "мм", "ммм"}},
            {"uk", {"е", "гм", "хм", "хмм", "мм", "ммм", "ем"}},
            {"hi", {"उम्म", "उम", "हम्म", "अं"}},
            {"ja", {"えー", "えっと", "えーと", "えーっと", "あー", "うーん", "んー", "うー"}},
            {"ko", {"음", "으음", "흠"}},
            {"zh", {"呃", "呃呃"}},
        };
        QHash<QString, QStringList> out;
        for (auto it = raw.begin(); it != raw.end(); ++it)
            for (const char* w : it.value()) out[it.key()] << normalize(QString::fromUtf8(w));
        return out;
    }();
    return sets;
}

// Held sounds of any language ("eeeh", "ummm", "yyyy", "ммм"), with or without a known language
const QStringList& heldSet()
{
    static const QStringList set = [] {
        QStringList s;
        for (const char* w : {"e", "eh", "em", "ehm", "uh", "um", "uhm", "hm", "m", "y", "ı", "ae", "aeh", "aehm", "oe",
                              "oeh", "oehm", "euh", "э", "е", "м", "эм", "хм"})
            s << normalize(QString::fromUtf8(w));
        return s;
    }();
    return set;
}

} // namespace

bool isFiller(const QString& w, const QString& language)
{
    if (w.isEmpty()) return false;
    if (exactSets().value(language.trimmed().toLower().left(2)).contains(w)) return true;
    const QString c = collapsed(w);
    // Held: one letter at least three times ("eee", not "ee": Russian "её" folds to "ее"), or a longer sound held at all
    if (w.size() < c.size() + (c.size() == 1 ? 2 : 1)) return false;
    return heldSet().contains(c);
}

double similarity(const QString& a, const QString& b)
{
    if (a == b) return 1;
    if (a.isEmpty() || b.isEmpty()) return 0;
    const int longest = int(std::max(a.size(), b.size()));
    if (longest <= 3) return 0; // short words must be exact ("ich" is not "ist")
    return 1.0 - double(editDistance(a, b)) / longest;
}

QVector<Sentence> sentences(const QString& script)
{
    QVector<Sentence> out;
    static const QRegularExpression paragraph(R"(\n\s*\n)");
    for (const QString& block : script.split(paragraph)) {
        QString current;
        const auto flush = [&] {
            Sentence s;
            s.text = current.simplified();
            for (const QString& w : s.text.split(' ', Qt::SkipEmptyParts))
                if (const QString n = normalize(w); !n.isEmpty() && !isFiller(n)) s.words << n;
            if (!s.words.isEmpty()) out << s;
            current.clear();
        };
        for (const QString& w : block.simplified().split(' ', Qt::SkipEmptyParts)) {
            current += w + ' ';
            if (endsSentence(w)) flush();
        }
        flush();
    }
    return out;
}

QVector<Take> findTakes(const QStringList& sentence, const QVector<Word>& words, int source, double minCoverage)
{
    QVector<Take> out;
    if (sentence.isEmpty()) return out;
    QStringList norm;
    QVector<int> index;
    filtered(words, 0, int(words.size()), &norm, &index);
    QVector<bool> used(norm.size(), false);
    for (;;) {
        // The unused stretches; the best alignment in any of them
        Hit best;
        for (int lo = 0; lo < norm.size();) {
            if (used[lo]) {
                ++lo;
                continue;
            }
            int hi = lo;
            while (hi < norm.size() && !used[hi]) ++hi;
            const Hit h = align(sentence, norm, lo, hi);
            if (h.score > best.score) best = h;
            lo = hi;
        }
        if (best.first < 0) break;
        const double coverage = double(best.matched) / sentence.size();
        if (coverage < minCoverage) break;
        // The sentence's last words not recognised: the take must not end before them. As many spoken words as are
        // missing join it when they follow without a pause and the speaker stops after them (a pause, the sentence's
        // end in the transcript, or the end) - else the take keeps its matched end
        if (best.missingEnd > 0) {
            int last = best.last;
            for (int k = 0; k < best.missingEnd && last + 1 < norm.size() && !used[last + 1]; ++k) {
                if (words[index[last + 1]].from - words[index[last]].to > kJoinS) break;
                ++last;
                const int w = index[last];
                const bool stops = w + 1 >= words.size() || words[w + 1].from - words[w].to >= kStopS
                                   || endsSentence(words[w].text);
                if (stops) { // not counted as extra: they stand for the sentence's own words
                    best.last = last;
                    break;
                }
            }
        }
        for (int i = best.first; i <= best.last; ++i) used[i] = true;
        out << takeOf(words, source, index[best.first], index[best.last], coverage, best.extra);
    }
    std::sort(out.begin(), out.end(), [](const Take& a, const Take& b) { return a.from < b.from; });
    return out;
}

QVector<Choice> roughCut(const QVector<Sentence>& script, const QVector<QVector<Word>>& sources, Pick pick)
{
    QVector<Choice> out;
    QVector<QVector<QPair<int, int>>> taken(sources.size()); // word ranges used by an earlier sentence
    for (int k = 0; k < script.size(); ++k) {
        Choice c;
        c.sentence = k;
        for (int s = 0; s < sources.size(); ++s)
            for (const Take& t : findTakes(script[k].words, sources[s], s)) {
                const bool clash = std::any_of(taken[s].begin(), taken[s].end(),
                                               [&](const QPair<int, int>& r) { return t.first <= r.second && r.first <= t.last; });
                if (!clash) c.takes << t;
            }
        c.chosen = pickTake(c.takes, pick, &c.why);
        if (c.chosen >= 0) taken[c.takes[c.chosen].source] << qMakePair(c.takes[c.chosen].first, c.takes[c.chosen].last);
        else c.why = "not found in the transcripts";
        out << c;
    }
    return out;
}

QVector<Repeat> repeats(const QVector<Word>& words, int source, double minCoverage)
{
    // The spoken sentences: word ranges ending with . ! ?
    struct Spoken {
        int first, last;
        QStringList norm;
    };
    QVector<Spoken> spoken;
    int start = 0;
    for (int i = 0; i < words.size(); ++i)
        if (endsSentence(words[i].text) || i + 1 == words.size()) {
            Spoken s{start, i, {}};
            QVector<int> index;
            filtered(words, start, i + 1, &s.norm, &index);
            if (!s.norm.isEmpty()) spoken << s;
            start = i + 1;
        }
    // How much of a's words b says (in order)
    const auto covered = [](const QStringList& a, const QStringList& b) {
        const Hit h = align(a, b, 0, int(b.size()));
        return a.isEmpty() ? 0.0 : double(h.matched) / a.size();
    };
    QVector<Repeat> out;
    for (int i = 0; i < spoken.size();) {
        QVector<int> group{i};
        int j = i + 1;
        // The next sentence says this one again (or this one was its false start: its words are the next one's start)
        while (j < spoken.size()) {
            const Spoken& a = spoken[group.last()];
            const Spoken& b = spoken[j];
            const bool again = std::min(a.norm.size(), b.norm.size()) >= 2
                               && (covered(b.norm, a.norm) >= minCoverage || covered(a.norm, b.norm) >= minCoverage);
            if (!again) break;
            group << j++;
        }
        if (group.size() > 1) {
            // Rated against the last one (people say it again until it is right), unless that is a false start
            // (less than 70 % of the longest): then against the longest
            int longest = group.first();
            for (int g : group)
                if (spoken[g].norm.size() > spoken[longest].norm.size()) longest = g;
            if (spoken[group.last()].norm.size() >= 0.7 * spoken[longest].norm.size()) longest = group.last();
            Repeat r;
            for (int g : group) {
                const Hit h = align(spoken[longest].norm, spoken[g].norm, 0, int(spoken[g].norm.size()));
                r.takes << takeOf(words, source, spoken[g].first, spoken[g].last,
                                  double(h.matched) / spoken[longest].norm.size(),
                                  int(spoken[g].norm.size()) - h.matched);
            }
            r.keep = pickTake(r.takes, Pick::LastGood, nullptr);
            out << r;
        }
        i = j;
    }
    return out;
}

} // namespace ScriptAlign
