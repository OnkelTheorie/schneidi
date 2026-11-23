// SPDX-License-Identifier: AGPL-3.0-only
#include "core/NameMatch.h"

#include <QRegularExpression>

#include <algorithm>

namespace NameMatch {

namespace {

constexpr int kTypoMinLength = 4; // shorter words must match exactly ("cs2", "4k", "mp4")

// a and b differ by at most one inserted, dropped or replaced character
bool oneEditAway(QStringView a, QStringView b)
{
    if (a.size() < b.size()) std::swap(a, b);
    const qsizetype la = a.size(), lb = b.size();
    if (la - lb > 1) return false;
    qsizetype i = 0;
    while (i < lb && a[i] == b[i]) ++i;
    if (i == lb) return true; // equal, or a has one more at the end
    if (la == lb) return a.mid(i + 1) == b.mid(i + 1); // one replaced
    return a.mid(i + 1) == b.mid(i);                   // one dropped from a
}

// One part (no separators in it: "a.mp4" is "a" next to "mp4", a typo there would find "b.mp4"), long enough, with a
// letter
bool typoAllowed(const QString& word)
{
    if (word.size() < kTypoMinLength || word.contains(' ')) return false;
    return std::any_of(word.cbegin(), word.cend(), [](QChar c) { return c.isLetter(); });
}

} // namespace

QString normalize(const QString& text, QVector<int>* origin)
{
    QString out;
    out.reserve(text.size());
    if (origin) {
        origin->clear();
        origin->reserve(text.size());
    }
    bool pendingSpace = false;
    auto put = [&](QChar c, int at) {
        if (pendingSpace && !out.isEmpty()) {
            out += ' ';
            if (origin) *origin << at;
        }
        pendingSpace = false;
        out += c;
        if (origin) *origin << at;
    };
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (c.isHighSurrogate() && i + 1 < text.size()) { // outside the BMP (emoji, rare scripts): kept as it is
            const char32_t u = QChar::surrogateToUcs4(c, text[i + 1]);
            if (QChar::isLetterOrNumber(u)) {
                put(c, i);
                out += text[i + 1];
                if (origin) *origin << i;
            } else {
                pendingSpace = true;
            }
            ++i;
            continue;
        }
        if (!c.isLetterOrNumber()) {
            pendingSpace = true;
            continue;
        }
        if (c == QChar(0xDF)) { // ß
            put('s', i);
            put('s', i);
            continue;
        }
        if (c.unicode() < 0x80) {
            put(c.toLower(), i);
            continue;
        }
        // Accents: the base letters of the decomposed form (é -> e + combining accent)
        const QString d = QString(c).normalized(QString::NormalizationForm_KD);
        for (const QChar b : d)
            if (b.category() != QChar::Mark_NonSpacing && b.category() != QChar::Mark_SpacingCombining
                && b.category() != QChar::Mark_Enclosing)
                put(b.toCaseFolded(), i);
    }
    return out;
}

QStringList queryWords(const QString& query)
{
    QStringList out;
    for (const QString& part : query.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts)) {
        const QString w = normalize(part);
        if (!w.isEmpty()) out << w;
    }
    return out;
}

Range find(const QString& word, const QString& hay)
{
    if (word.isEmpty()) return {0, 0};
    if (const qsizetype at = hay.indexOf(word); at >= 0) return {int(at), int(word.size())};
    if (!typoAllowed(word)) return {-1, 0};
    const qsizetype m = word.size();
    const QStringView h(hay);
    for (const qsizetype len : {m, m - 1, m + 1}) { // a replaced character first, then a missing or an extra one
        if (len <= 0) continue;
        for (qsizetype s = 0; s + len <= h.size(); ++s) {
            if (h[s] == ' ' || h[s + len - 1] == ' ') continue; // not half a neighbouring word
            if (oneEditAway(word, h.mid(s, len))) return {int(s), int(len)};
        }
    }
    return {-1, 0};
}

bool matches(const QStringList& words, const QStringList& texts)
{
    if (words.isEmpty()) return true;
    QStringList hays;
    hays.reserve(texts.size());
    for (const QString& t : texts) hays << normalize(t);
    for (const QString& w : words)
        if (std::none_of(hays.cbegin(), hays.cend(), [&](const QString& h) { return find(w, h).start >= 0; }))
            return false;
    return true;
}

bool matches(const QStringList& words, const QString& text)
{
    return matches(words, QStringList{text});
}

QVector<Range> ranges(const QStringList& words, const QString& text)
{
    QVector<int> origin;
    const QString hay = normalize(text, &origin);
    QVector<Range> found;
    for (const QString& w : words) {
        const Range r = find(w, hay);
        if (r.start < 0 || r.length <= 0) continue;
        const int from = origin[r.start];
        const int to = origin[r.start + r.length - 1] + 1;
        found << Range{from, to - from};
    }
    std::sort(found.begin(), found.end(), [](const Range& a, const Range& b) { return a.start < b.start; });
    QVector<Range> merged;
    for (const Range& r : found) {
        if (!merged.isEmpty() && r.start <= merged.last().start + merged.last().length)
            merged.last().length = std::max(merged.last().length, r.start + r.length - merged.last().start);
        else
            merged << r;
    }
    return merged;
}

} // namespace NameMatch
