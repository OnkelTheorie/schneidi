// SPDX-License-Identifier: AGPL-3.0-only
#include "core/IndexSearch.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace IndexSearch {

namespace {

// What a word of the question can mean besides itself
enum class Quality { None, Calm, Busy, Dark, Bright, Blurry, Sharp, Pan, Face, Speech };

struct Group {
    const char* event;          // AudioEvents id, or nullptr
    Quality quality;
    QVector<const char*> stems; // folded (see fold); a stem of 4+ letters also matches longer words starting with it
};

// Small on purpose: what people ask for in their material, in German and English
const QVector<Group>& groups()
{
    static const QVector<Group> g{
        {"laughter", Quality::None, {"lach", "gelachter", "kicher", "laugh", "laughter", "giggl", "chuckl"}},
        {"applause", Quality::None, {"klatsch", "applaus", "beifall", "applause", "clap"}},
        {"cheering", Quality::None, {"jubel", "jubl", "jauchz", "hurra", "cheer"}},
        {"shot", Quality::None, {"schuss", "schusse", "schies", "knall", "explosion", "gunshot", "shot", "shoot", "bang"}},
        {"scream", Quality::None, {"schrei", "kreisch", "brull", "scream", "shout", "yell"}},
        {"music", Quality::None, {"musik", "lied", "gesang", "music", "song"}},
        {"speech", Quality::Speech, {"sprache", "sprich", "spricht", "sprech", "rede", "redet", "gesprach", "stimme",
                                     "speech", "talk", "speak", "voice"}},
        {"silence", Quality::None, {"stille", "leise", "silence", "silent", "quiet"}},
        {nullptr, Quality::Calm, {"ruhig", "statisch", "still", "calm", "static", "steady"}},
        {nullptr, Quality::Busy, {"bewegung", "hektisch", "schnell", "action", "fast", "busy", "moving", "motion"}},
        {nullptr, Quality::Dark, {"dunkel", "finster", "nacht", "unterbelichtet", "dark", "night", "underexposed"}},
        {nullptr, Quality::Bright, {"hell", "grell", "uberbelichtet", "bright", "overexposed"}},
        {nullptr, Quality::Blurry, {"unscharf", "verschwommen", "verwackelt", "blurry", "blur", "unfocused"}},
        {nullptr, Quality::Sharp, {"scharf", "sharp", "crisp"}},
        {nullptr, Quality::Pan, {"schwenk", "kamerafahrt", "pan", "panning"}},
        {nullptr, Quality::Face, {"gesicht", "person", "mensch", "leute", "face", "people", "portrait", "portrat"}},
        {nullptr, Quality::None, {"totale", "weit", "uberblick", "wide", "establishing"}},
        {nullptr, Quality::None, {"nah", "nahe", "nahaufnahme", "close", "closeup", "detail"}},
        {nullptr, Quality::None, {"laden", "geschaft", "shop", "store"}},
        {nullptr, Quality::None, {"draussen", "aussen", "outdoor", "outside"}},
        {nullptr, Quality::None, {"drinnen", "innen", "indoor", "inside"}},
    };
    return g;
}

const QSet<QString>& fillers()
{
    static const QSet<QString> f{
        "wo", "wann", "wie", "was", "wer", "er", "sie", "es", "ich", "du", "wir", "ihr", "man", "jemand", "der", "die",
        "das", "den", "dem", "des", "ein", "eine", "einen", "einem", "einer", "und", "oder", "mit", "vom", "von", "im",
        "in", "am", "an", "auf", "zu", "zum", "zur", "ist", "sind", "gibt", "mal", "bei", "beim", "fur", "aus", "hat",
        "the", "a", "an", "of", "on", "at", "where", "when", "what", "who", "is", "are", "he", "she", "it", "they",
        // "shot" in "a calm shot" is the take, not a gunshot (alone it still searches: queryWords)
        "with", "and", "or", "from", "someone", "somebody", "to", "for", "by", "there", "any", "some", "shot", "shots"};
    return f;
}

QString fold(QString s)
{
    s = s.toLower();
    s.replace(QChar(0xe4), 'a').replace(QChar(0xf6), 'o').replace(QChar(0xfc), 'u').replace(QChar(0xdf), "ss");
    s.replace(QChar(0xe9), 'e').replace(QChar(0xe8), 'e').replace(QChar(0xe0), 'a');
    return s;
}

bool stemMatches(const QString& token, const QString& stem)
{
    // German compounds end in their head word ("Kameraschwenk"); a short front is a prefix, not a word ("unscharf")
    return token == stem || (stem.size() >= 4 && token.startsWith(stem))
           || (stem.size() >= 5 && token.size() >= stem.size() + 3 && token.endsWith(stem));
}

// How well a term matches the words of a field: 1 = the same word, 0.6 = part of a word (compound, inflection)
double termScore(const QString& term, const QStringList& fieldWords)
{
    double best = 0;
    for (const QString& w : fieldWords) {
        if (w == term) return 1.0;
        if (term.size() >= 4 && w.contains(term)) best = 0.6;
        else if (w.size() >= 4 && term.startsWith(w)) best = std::max(best, 0.5);
    }
    return best;
}

bool hasQuality(const Shot& s, Quality q)
{
    switch (q) {
    case Quality::Calm: return s.motion >= 0 && s.motion < 1.0 && !s.pan;
    case Quality::Busy: return s.motion >= 5.0;
    case Quality::Dark: return s.dark;
    case Quality::Bright: return s.overexposed;
    case Quality::Blurry: return s.blurry;
    case Quality::Sharp: return s.sharp;
    case Quality::Pan: return s.pan;
    case Quality::Face: return s.face;
    case Quality::Speech: return !s.speech.trimmed().isEmpty();
    case Quality::None: break;
    }
    return false;
}

const char* qualityName(Quality q)
{
    switch (q) {
    case Quality::Calm: return "calm";
    case Quality::Busy: return "motion";
    case Quality::Dark: return "dark";
    case Quality::Bright: return "overexposed";
    case Quality::Blurry: return "blurry";
    case Quality::Sharp: return "sharp";
    case Quality::Pan: return "pan";
    case Quality::Face: return "face likely";
    case Quality::Speech: return "speech";
    case Quality::None: break;
    }
    return "";
}

struct Meaning {
    QStringList terms;       // the word itself first, then its synonyms
    QStringList events;      // event ids it stands for
    QVector<Quality> quality;
};

Meaning meaningOf(const QString& token)
{
    Meaning m;
    m.terms << token;
    for (const Group& g : groups()) {
        bool hit = false;
        for (const char* s : g.stems)
            if (stemMatches(token, QString::fromLatin1(s))) hit = true;
        if (!hit) continue;
        for (const char* s : g.stems)
            if (!m.terms.contains(QLatin1String(s))) m.terms << QString::fromLatin1(s);
        if (g.event && !m.events.contains(QLatin1String(g.event))) m.events << QString::fromLatin1(g.event);
        if (g.quality != Quality::None) m.quality << g.quality;
    }
    return m;
}

} // namespace

QStringList words(const QString& text)
{
    QStringList out;
    QString cur;
    const QString f = fold(text);
    for (const QChar c : f) {
        if (c.isLetterOrNumber()) {
            cur += c;
        } else if (!cur.isEmpty()) {
            out << cur;
            cur.clear();
        }
    }
    if (!cur.isEmpty()) out << cur;
    return out;
}

QStringList queryWords(const QString& query)
{
    const QStringList all = words(query);
    QStringList out;
    for (const QString& w : all)
        if (!fillers().contains(w) || (w == "shot" && all.size() == 1)) out << w;
    return out.isEmpty() ? all : out; // a question of filler words only still searches for them
}

QVector<Hit> search(const QString& query, const QVector<Shot>& shots, int top)
{
    const QStringList tokens = queryWords(query);
    if (tokens.isEmpty()) return {};
    QVector<Meaning> meanings;
    for (const QString& t : tokens) meanings << meaningOf(t);

    QVector<Hit> hits;
    for (const Shot& s : shots) {
        QStringList tags;
        for (const QString& t : s.tags) tags += words(t);
        const QStringList desc = words(s.desc), speech = words(s.speech);
        double total = 0;
        int matched = 0;
        QVector<Reason> reasons;
        const Event* bestEvent = nullptr;
        for (const Meaning& m : meanings) {
            double best = 0;
            Reason reason;
            const Event* event = nullptr;
            const auto consider = [&](double score, const Reason& r, const Event* e = nullptr) {
                if (score > best) {
                    best = score;
                    reason = r;
                    event = e;
                }
            };
            for (int i = 0; i < m.terms.size(); ++i) {
                const QString& term = m.terms[i];
                const double weight = i == 0 ? 1.0 : 0.8; // the asked word beats its synonyms
                consider(3.0 * weight * termScore(term, tags), {"tag", term});
                consider(2.0 * weight * termScore(term, desc), {"desc", term});
                consider(1.5 * weight * termScore(term, speech), {"speech", term});
            }
            for (const Event& e : s.events) {
                const bool same = m.events.contains(e.kind) || m.terms.first() == e.kind;
                if (same)
                    consider(2.0 * std::clamp(e.conf, 0.1, 1.0), {"event", e.kind, e.conf}, &e);
            }
            for (Quality q : m.quality)
                if (hasQuality(s, q)) consider(1.0, {"quality", QString::fromLatin1(qualityName(q))});
            if (best <= 0) continue;
            total += best;
            ++matched;
            reasons << reason;
            if (event && (!bestEvent || event->conf > bestEvent->conf)) bestEvent = event;
        }
        if (matched == 0) continue;
        QStringList why;
        for (const Reason& r : reasons)
            why << (r.field == "event"     ? QString("event %1 %2").arg(r.term).arg(r.conf, 0, 'f', 2)
                    : r.field == "quality" ? QString("quality %1").arg(r.term)
                                           : QString("%1 '%2'").arg(r.field, r.term));
        Hit h{s.file, s.n, s.from, s.to, total * matched / tokens.size(), why.join(", "), reasons};
        if (bestEvent) {
            h.from = std::max(s.from, bestEvent->from);
            h.to = std::min(s.to, bestEvent->to);
            if (h.to <= h.from) {
                h.from = bestEvent->from;
                h.to = bestEvent->to;
            }
        }
        h.score = std::round(h.score * 100) / 100;
        hits << h;
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    if (top > 0 && hits.size() > top) hits.resize(top);
    return hits;
}

} // namespace IndexSearch
