// SPDX-License-Identifier: AGPL-3.0-only
#include "core/Transcript.h"

#include "core/Subtitles.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace Transcript {

namespace {
qint64 spokenLength(const Word& w) { return 250 + 70 * qint64(w.text.size()); }
}

bool parseWhisperJson(const QByteArray& json, Result* out, QString* error)
{
    out->language.clear();
    out->words.clear();
    return appendWhisperJson(json, [](qint64 t, bool) { return t; }, out, error);
}

bool appendWhisperJson(const QByteArray& json, const std::function<qint64(qint64, bool)>& map, Result* out,
                       QString* error)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = "invalid whisper output: " + err.errorString();
        return false;
    }
    const QJsonObject root = doc.object();
    if (out->language.isEmpty()) out->language = root["result"].toObject()["language"].toString();
    const qsizetype first = out->words.size();
    for (const QJsonValue& v : root["transcription"].toArray()) {
        const QJsonObject seg = v.toObject();
        const QString text = seg["text"].toString().trimmed();
        if (text.isEmpty() || (text.startsWith('[') && text.endsWith(']'))) continue; // pause, [_BEG_], [Music]
        const QJsonObject off = seg["offsets"].toObject();
        qint64 from = qint64(off["from"].toDouble());
        const qint64 to = qint64(off["to"].toDouble());
        // DTW (-dtw, full output -ojf): the first real token's time, in 10 ms steps; -1 = none
        for (const QJsonValue& t : seg["tokens"].toArray()) {
            const QJsonObject token = t.toObject();
            if (token["text"].toString().startsWith('[')) continue; // [_BEG_], [_TT_123]
            const double dtw = token["t_dtw"].toDouble(-1);
            if (dtw >= 0) from = qint64(dtw * 10);
            break;
        }
        Word w{map(from, false), map(std::max(from, to), true), text};
        if (w.to < w.from) w.to = w.from;
        // Punctuation that whisper split off ("," as its own word) belongs to the word before
        if (out->words.size() > first && text.size() <= 2 && !text.at(0).isLetterOrNumber()) {
            out->words.last().text += text;
            out->words.last().to = std::max(out->words.last().to, w.to);
            continue;
        }
        out->words << w;
    }
    // DTW starts can pass whisper's own ends: a word lasts at least until the next one starts, never past it
    for (qsizetype i = first; i + 1 < out->words.size(); ++i) {
        Word& w = out->words[i];
        const qint64 next = out->words[i + 1].from;
        if (next < w.from) out->words[i + 1].from = w.from; // DTW is monotonic in practice; keep the order anyway
        w.to = std::clamp(w.to, w.from, std::max(w.from, out->words[i + 1].from));
    }
    if (out->words.size() > first) out->words.last().to = std::max(out->words.last().to, out->words.last().from);
    return true;
}

qint64 pieceLength(const Piece& piece)
{
    return piece.isEmpty() ? 0 : piece.last().at + piece.last().length;
}

qint64 pieceTime(const Piece& piece, qint64 t, bool end)
{
    for (qsizetype i = 0; i < piece.size(); ++i) {
        const PieceSpan& s = piece[i];
        if (t < s.at) { // in the silence before this stretch
            if (end && i > 0) return piece[i - 1].from + piece[i - 1].length;
            return s.from;
        }
        if (t <= s.at + s.length) return s.from + (t - s.at);
    }
    return piece.isEmpty() ? t : piece.last().from + piece.last().length;
}

QVector<Piece> speechPieces(const QVector<QPair<qint64, qint64>>& segments, qint64 lengthMs, qint64 maxGapMs,
                            qint64 maxMs, qint64 padMs, qint64 silenceMs)
{
    QVector<QPair<qint64, qint64>> segs;
    for (auto [from, to] : segments) {
        from = std::clamp<qint64>(from, 0, lengthMs);
        to = std::clamp<qint64>(to, from, lengthMs);
        if (to > from) segs << qMakePair(from, to);
    }
    // Padded segments, never past the middle of the pause to a neighbour; one longer than a piece is split (the voice
    // detection already splits at 25 s)
    QVector<QPair<qint64, qint64>> padded;
    QVector<qint64> gapBefore; // the real pause before each padded segment (0 inside a split one)
    for (qsizetype i = 0; i < segs.size(); ++i) {
        qint64 from = std::max<qint64>(0, segs[i].first - padMs);
        qint64 to = std::min(lengthMs, segs[i].second + padMs);
        if (i > 0) from = std::max(from, (segs[i - 1].second + segs[i].first) / 2);
        if (i + 1 < segs.size()) to = std::min(to, (segs[i].second + segs[i + 1].first) / 2);
        qint64 gap = i > 0 ? segs[i].first - segs[i - 1].second : 0;
        while (to - from > maxMs) {
            padded << qMakePair(from, from + maxMs);
            gapBefore << gap;
            from += maxMs;
            gap = 0;
        }
        padded << qMakePair(from, to);
        gapBefore << gap;
    }
    // The spans of padded[a, b): a short pause stays, a long one shrinks to silenceMs
    const auto build = [&](qsizetype a, qsizetype b) {
        Piece p;
        for (qsizetype k = a; k < b; ++k) {
            const auto [from, to] = padded[k];
            if (!p.isEmpty() && from - (p.last().from + p.last().length) <= maxGapMs)
                p.last().length = to - p.last().from;
            else
                p << PieceSpan{p.isEmpty() ? 0 : pieceLength(p) + silenceMs, from, to - from};
        }
        return p;
    };
    QVector<Piece> pieces;
    for (qsizetype start = 0; start < padded.size();) {
        // As many whole segments as fit
        qsizetype end = start + 1;
        while (end < padded.size() && pieceLength(build(start, end + 1)) <= maxMs) ++end;
        if (end < padded.size()) {
            // Full: cut at the longest pause in its second half (between two sentences rather than inside one -
            // "Was" | "bist du?" heard in two pieces lost its place)
            qsizetype cut = end;
            for (qsizetype c = start + 1; c <= end; ++c)
                if (pieceLength(build(start, c)) >= maxMs / 2 && gapBefore[c] > gapBefore[cut]) cut = c;
            end = cut;
        }
        pieces << build(start, end);
        start = end;
    }
    return pieces;
}

int dropPromptEcho(QVector<Word>& words, const QString& prompt)
{
    const auto plain = [](QString t) {
        t = t.toLower();
        t.removeIf([](QChar c) { return !c.isLetterOrNumber(); });
        return t;
    };
    QStringList echo;
    for (const QString& w : prompt.split(' ', Qt::SkipEmptyParts))
        if (const QString p = plain(w); !p.isEmpty()) echo << p;
    if (echo.size() < 2) return 0;
    int dropped = 0;
    for (qsizetype i = 0; i + echo.size() <= words.size();) {
        bool same = true;
        for (qsizetype k = 0; k < echo.size() && same; ++k) same = plain(words[i + k].text) == echo[k];
        if (!same) {
            ++i;
            continue;
        }
        words.remove(i, echo.size());
        dropped += int(echo.size());
    }
    return dropped;
}

qint64 spokenEnd(const Word& w)
{
    // About 70 ms per character plus 250 ms: generous for slow speakers, cuts long pauses
    return std::min(w.to, w.from + 250 + 70 * qint64(w.text.size()));
}


Levels levelsOf(const qint16* samples, qint64 count, int sampleRate, int frameMs)
{
    Levels l;
    l.frameMs = frameMs;
    const qint64 n = std::max<qint64>(1, qint64(sampleRate) * frameMs / 1000);
    for (qint64 i = 0; i + n <= count; i += n) {
        double sum = 0;
        for (qint64 j = i; j < i + n; ++j) sum += double(samples[j]) * samples[j];
        const double rms = std::sqrt(sum / n) / 32768.0;
        l.db << float(rms > 1e-6 ? 20 * std::log10(rms) : -120.0);
    }
    return l;
}

bool isSilent(const Levels& levels)
{
    // No levels = not measured (unknown), not silence
    return !levels.db.isEmpty() && std::none_of(levels.db.begin(), levels.db.end(), [](float db) { return db > -60.f; });
}

int dropHallucinations(QVector<Word>& words, const Levels& levels)
{
    // Lower-case letters and digits of each word, split at everything else ("Amara.org-Community" = 3 tokens)
    QStringList tokens;
    QVector<int> wordOf; // token -> word index
    for (int i = 0; i < words.size(); ++i) {
        QString t;
        for (const QChar c : words[i].text.toLower()) t += c.isLetterOrNumber() ? c : QChar(' ');
        for (const QString& x : t.split(' ', Qt::SkipEmptyParts)) {
            tokens << x;
            wordOf << i;
        }
    }
    struct Phrase { QStringList tokens; bool always; };
    static const QVector<Phrase> phrases = {
        {{"untertitelung", "des", "zdf"}, true},
        {{"untertitelung", "im", "auftrag", "des", "zdf"}, true},
        {{"untertitel", "im", "auftrag", "des", "zdf"}, true},
        {{"untertitel", "der", "amara", "org", "community"}, true},
        {{"untertitel", "von", "stephanie", "geiges"}, true},
        {{"subtitles", "by", "the", "amara", "org", "community"}, true},
        {{"untertitelung", "br"}, true},
        {{"vielen", "dank", "fürs", "zuschauen"}, false},
        {{"vielen", "dank", "für", "s", "zuschauen"}, false},
        {{"danke", "fürs", "zuschauen"}, false},
        {{"thanks", "for", "watching"}, false},
        {{"thank", "you", "for", "watching"}, false},
        {{"bis", "zum", "nächsten", "mal"}, false},
    };
    const auto quiet = [&](qint64 from, qint64 to) {
        if (levels.db.isEmpty()) return false;
        const int a = int(std::clamp<qint64>(from / levels.frameMs, 0, levels.db.size()));
        const int b = int(std::clamp<qint64>(to / levels.frameMs + 1, a, levels.db.size()));
        for (int i = a; i < b; ++i)
            if (levels.db[i] > -45.f) return false;
        return true;
    };
    QVector<bool> drop(words.size(), false);
    for (int t = 0; t < tokens.size(); ++t)
        for (const Phrase& p : phrases) {
            if (t + p.tokens.size() > tokens.size()) continue;
            bool same = true;
            for (int k = 0; k < p.tokens.size() && same; ++k) same = tokens[t + k] == p.tokens[k];
            if (!same) continue;
            int last = t + int(p.tokens.size()) - 1;
            // ", 2020" or "2018" after it belongs to the line
            while (last + 1 < tokens.size() && tokens[last + 1].size() == 4 && tokens[last + 1].toInt() > 1900) ++last;
            const int w0 = wordOf[t], w1 = wordOf[last];
            if (!p.always && !quiet(words[w0].from, words[w1].to)) continue;
            for (int w = w0; w <= w1; ++w) drop[w] = true;
        }
    int removed = 0;
    QVector<Word> kept;
    for (int i = 0; i < words.size(); ++i) {
        if (drop[i]) ++removed;
        else kept << words[i];
    }
    words = kept;
    return removed;
}

namespace {
// Prefix sums of the loud frames; loud = clearly above the quiet parts (20th percentile), at least -50 dB
QVector<int> loudPrefix(const Levels& levels)
{
    QVector<float> sorted = levels.db;
    std::sort(sorted.begin(), sorted.end());
    const float threshold = sorted.isEmpty() ? 0.f : std::max(-50.f, sorted[sorted.size() / 5] + 10.f);
    QVector<int> voiced(levels.db.size() + 1, 0);
    for (int i = 0; i < levels.db.size(); ++i) voiced[i + 1] = voiced[i] + (levels.db[i] >= threshold);
    return voiced;
}
}

QVector<SubtitleCue> mergeCues(const QVector<QVector<SubtitleCue>>& tracks)
{
    QVector<SubtitleCue> all;
    for (const auto& t : tracks) all += t;
    std::stable_sort(all.begin(), all.end(), [](const SubtitleCue& a, const SubtitleCue& b) { return a.start < b.start; });
    QVector<SubtitleCue> out;
    for (const SubtitleCue& c : all) {
        if (!out.isEmpty() && c.start < out.last().end) {
            out.last().text += '\n' + c.text;
            out.last().end = std::max(out.last().end, c.end);
        } else {
            out << c;
        }
    }
    return out;
}

void startAtSound(QVector<SubtitleCue>& cues, const Levels& levels, double fps, int offset)
{
    if (levels.db.isEmpty() || fps <= 0) return;
    const QVector<int> voiced = loudPrefix(levels);
    const int fm = std::max(1, levels.frameMs);
    constexpr int kRun = 5; // 50 ms of sound, not a click
    for (int i = 0; i < cues.size(); ++i) {
        SubtitleCue& c = cues[i];
        const int a = int(std::max(0.0, (c.start - offset) * 1000.0 / fps / fm));
        const int b = int(std::min<double>(levels.db.size(), (c.end - offset) * 1000.0 / fps / fm));
        // Not a single loud frame: whisper made it up in silence ("Hey," over 3 s of digital silence)
        if (a < b && voiced[b] == voiced[a]) {
            cues.remove(i--);
            continue;
        }
        for (int f = a; f + kRun <= b; ++f) {
            if (voiced[f + kRun] - voiced[f] < kRun) continue;
            // 100 ms before the sound, so the line is there when the first syllable is. Multiplied before the
            // division: (ms / 1000.0) * fps lands just below a whole frame on some machines (x87 on 32-bit x86)
            const int start = offset + int(std::floor((f * fm - 100) * fps / 1000.0));
            if (start > c.start && start < c.end - 1) c.start = start;
            break;
        }
    }
}

void endAtSound(QVector<SubtitleCue>& cues, const Levels& levels, double fps, int offset, int holdMs, int minMs)
{
    if (levels.db.isEmpty() || fps <= 0) return;
    // The noise floor of the whole sound, as for startAtSound: sound is clearly above it
    QVector<float> sorted = levels.db;
    std::sort(sorted.begin(), sorted.end());
    const float floorDb = sorted[sorted.size() / 5] + 10.f;
    const int fm = std::max(1, levels.frameMs);
    constexpr int kRun = 3;          // 30 ms of sound, not a click
    constexpr float kBelowPeak = 30; // dB under the cue's loud part: the fading end of a word still counts
    const auto frameOf = [&](int ms) { return int(std::floor(ms * fps / 1000.0)); };
    for (SubtitleCue& c : cues) {
        const int a = int(std::clamp<double>((c.start - offset) * 1000.0 / fps / fm, 0, levels.db.size()));
        const int b = int(std::clamp<double>(std::ceil((c.end - offset) * 1000.0 / fps / fm), a, levels.db.size()));
        if (b - a < kRun) continue;
        // The cue's loud part: its 90th percentile (one click does not raise it)
        QVector<float> own(levels.db.begin() + a, levels.db.begin() + b);
        std::sort(own.begin(), own.end());
        const float threshold = std::max(floorDb, own[qsizetype(own.size() * 9 / 10)] - kBelowPeak);
        int last = -1; // the last frame of sound (kRun frames above the threshold end there)
        for (int f = b - 1, run = 0; f >= a; --f) {
            run = levels.db[f] >= threshold ? run + 1 : 0;
            if (run == kRun) {
                last = f + kRun - 1;
                break;
            }
        }
        if (last < 0 || last >= b - 1) continue; // no sound found (startAtSound's business) or sound to the end
        // Multiplied before the division (see startAtSound)
        const int end = std::max(offset + frameOf((last + 1) * fm + holdMs), c.start + std::max(1, frameOf(minMs)));
        if (end < c.end) c.end = end;
    }
}

void fitToSpeech(QVector<Word>& words, const Levels& levels)
{
    const QVector<int> voiced = loudPrefix(levels);
    const int fm = std::max(1, levels.frameMs);
    const auto loud = [&](qint64 a, qint64 b) { // loud frames in [a, b) ms
        const int fa = int(std::clamp<qint64>(a / fm, 0, levels.db.size()));
        const int fb = int(std::clamp<qint64>(b / fm, 0, levels.db.size()));
        return fb > fa ? voiced[fb] - voiced[fa] : 0;
    };
    constexpr qint64 kTouch = 60; // ms: a word that starts this close after another follows it directly
    const auto stretched = [](const Word& w) { return w.to - w.from > spokenLength(w) + 200; };
    for (int i = 0, done = 0; i < words.size();) {
        if (!stretched(words[i])) {
            ++i;
            continue;
        }
        // A run of stretched words, e.g. "Das ist scheiße" over 9 s of laughter: they belong together
        int j = i;
        while (j + 1 < words.size() && stretched(words[j + 1]) && words[j + 1].from - words[j].to < kTouch) ++j;
        qint64 len = 0;
        for (int k = i; k <= j; ++k) len += spokenLength(words[k]);
        // Words whisper gave no time of their own start together with the stretched one ("Wie" "geht" "das?" all at
        // 12.28 s, "das?" stretched to 14.05 s; "Hey," "auf" at 7.79 s in silence, spoken at 10.4 s): they go along
        while (i > done && std::abs(words[i - 1].from - words[i].from) < 20
               && len + spokenLength(words[i - 1]) <= words[j].to - words[i - 1].from)
            len += spokenLength(words[--i]);
        const qint64 from = words[i].from, to = words[j].to;
        const bool nextTouches = j + 1 < words.size() && words[j + 1].from - to < kTouch;
        const bool prevTouches = i > 0 && from - words[i - 1].to < kTouch;
        // Where the sound is: the loudest stretch of the spoken length ("Turm auf F7" with silence after it, whisper
        // stretched "F7" over the silence); equally loud (laughter, game sound) or no levels: the side of the word that
        // follows directly (whisper hands the pause to the words before), else the one before
        const bool toEnd = nextTouches || !prevTouches;
        const int steps = int((to - from - len) / fm);
        QVector<int> score(steps + 1);
        int best = 0;
        for (int s = 0; s <= steps; ++s) best = std::max(best, score[s] = loud(from + qint64(s) * fm, from + qint64(s) * fm + len));
        int pick = toEnd ? steps : 0;
        while (score[pick] < best) pick += toEnd ? -1 : 1;
        if (best == 0 && !nextTouches && !prevTouches) pick = 0; // nothing loud, alone: as whisper says
        qint64 start = from + qint64(pick) * fm;
        const bool atEnd = pick == steps;
        for (int k = i; k <= j; ++k) {
            const qint64 l = spokenLength(words[k]);
            words[k].from = start;
            words[k].to = k == j && atEnd ? to : start + l;
            start += l;
        }
        i = done = j + 1;
    }
    if (levels.db.isEmpty()) return;
    // Words in complete silence just before a word with sound ("Wie geht" 0.8 s before "das?", all spoken at once):
    // whisper started them too early, they move up to it
    constexpr qint64 kPull = 1500; // ms: further away it is probably a word of its own
    const auto silent = [&](const Word& w) { return loud(w.from, std::max(spokenEnd(w), w.from + fm)) == 0; };
    for (int i = 0; i < words.size();) {
        if (!silent(words[i])) {
            ++i;
            continue;
        }
        int j = i;
        while (j + 1 < words.size() && silent(words[j + 1])) ++j;
        if (j + 1 < words.size()) {
            const qint64 shift = words[j + 1].from - spokenEnd(words[j]);
            if (shift > 0 && shift < kPull)
                for (int k = i; k <= j; ++k) words[k].from += shift, words[k].to = std::min(words[k].to + shift, words[j + 1].from);
        }
        i = j + 1;
    }
}

namespace {
// Break points of a phrase that is too long for one line: as few lines as greedy filling needs, but even lengths
// ("aber wenn du Softbox guckst, | beleidigt dich nicht." instead of "… beleidigt dich | nicht.")
QVector<int> balancedBreaks(const QVector<Word>& words, int first, int last, int maxChars)
{
    const int m = last - first + 1;
    QVector<int> pre(m + 1, 0); // characters of words [0, k) plus one space each
    for (int k = 0; k < m; ++k) pre[k + 1] = pre[k] + int(words[first + k].text.size()) + 1;
    const auto len = [&](int a, int b) { return pre[b] - pre[a] - 1; }; // words [a, b)
    int lines = 1;
    for (int k = 0, a = 0; k < m; ++k)
        if (k > a && len(a, k + 1) > maxChars) a = k, ++lines;
    if (lines == 1) return {};
    const double target = double(len(0, m)) / lines;
    constexpr double kNone = 1e18;
    // cost[l][k]: best for words [0, k) in l lines; one word longer than maxChars is a line of its own
    QVector<QVector<double>> cost(lines + 1, QVector<double>(m + 1, kNone));
    QVector<QVector<int>> from(lines + 1, QVector<int>(m + 1, -1));
    cost[0][0] = 0;
    for (int l = 1; l <= lines; ++l)
        for (int k = 1; k <= m; ++k)
            for (int a = k - 1; a >= 0; --a) {
                if (k - a > 1 && len(a, k) > maxChars) break;
                if (cost[l - 1][a] >= kNone) continue;
                const double d = len(a, k) - target;
                if (cost[l - 1][a] + d * d < cost[l][k]) cost[l][k] = cost[l - 1][a] + d * d, from[l][k] = a;
            }
    QVector<int> breaks; // index of the first word of each line after the first
    for (int l = lines, k = m; l > 1; --l) breaks.prepend(first + (k = from[l][k]));
    return breaks;
}
}

QVector<SubtitleCue> toCues(const QVector<Word>& words, double fps, int maxChars, int maxGapMs, int offset)
{
    QVector<SubtitleCue> cues;
    const auto frame = [&](qint64 ms) { return offset + int(std::lround(ms / 1000.0 * fps)); };
    const auto add = [&](int a, int b) { // words [a, b] as one cue
        QStringList text;
        for (int k = a; k <= b; ++k) text << words[k].text;
        int s = frame(words[a].from), e = std::max(frame(spokenEnd(words[b])), s + 1);
        if (!cues.isEmpty()) s = std::max(s, cues.last().end);
        cues << SubtitleCue{0, s, std::max(e, s + 1), text.join(' ')};
    };
    const auto flush = [&](int a, int b) { // a phrase: split into even lines if too long
        if (a > b) return;
        for (int k : balancedBreaks(words, a, b, maxChars)) add(a, k - 1), a = k;
        add(a, b);
    };
    int first = 0, chars = 0;
    for (int i = 0; i < words.size(); ++i) {
        const Word& w = words[i];
        if (i > first && w.from - spokenEnd(words[i - 1]) > maxGapMs) flush(first, i - 1), first = i, chars = 0;
        chars += (chars ? 1 : 0) + int(w.text.size());
        const QChar last = w.text.back();
        // Sentence end: always a new cue; a comma after half a line: a good place for one
        if (last == '.' || last == '?' || last == '!' || ((last == ',' || last == ';') && chars > maxChars / 2))
            flush(first, i), first = i + 1, chars = 0;
    }
    flush(first, int(words.size()) - 1);
    // Long enough to read (0.6 s + 50 ms per character), up to the next cue: whisper gives "Ja, ja, ja," 0.1 s
    for (int i = 0; i < cues.size(); ++i) {
        const int minimum = int(std::lround((0.6 + 0.05 * cues[i].text.size()) * fps));
        int end = std::max(cues[i].end, cues[i].start + minimum);
        if (i + 1 < cues.size()) end = std::min(end, cues[i + 1].start);
        cues[i].end = std::max(cues[i].end, end);
    }
    return cues;
}

QVector<SubtitleCue> wordCues(const QVector<Word>& words, double fps, int offset)
{
    QVector<SubtitleCue> cues;
    const auto frame = [&](qint64 ms) { return offset + int(std::lround(ms / 1000.0 * fps)); };
    for (const Word& w : words) {
        const QString text = w.text.simplified();
        if (text.isEmpty()) continue;
        const int from = frame(w.from);
        cues << SubtitleCue{0, from, std::max(from + 1, frame(spokenEnd(w))), text};
    }
    std::stable_sort(cues.begin(), cues.end(), [](const SubtitleCue& a, const SubtitleCue& b) { return a.start < b.start; });
    for (int i = 0; i + 1 < cues.size(); ++i)
        cues[i].end = std::max(cues[i].start + 1, std::min(cues[i].end, cues[i + 1].start));
    for (SubtitleCue& c : cues)
        if (!c.text.contains(' ')) c.words = {CueWord{0, c.length()}};
    return cues;
}

QVector<SubtitleCue> readableCues(const QVector<Word>& words, const Levels& levels, double fps, int offset, bool music)
{
    QVector<SubtitleCue> cues = Subtitles::readable(wordCues(words, fps, offset), fps);
    if (music) return cues;
    QVector<SubtitleCue> moved = cues;
    for (int i = 0; i < moved.size(); ++i) moved[i].id = i; // which cue it was (startAtSound drops some)
    startAtSound(moved, levels, fps, offset);
    endAtSound(moved, levels, fps, offset);
    for (SubtitleCue& c : moved) {
        const int d = c.start - cues[c.id].start; // the words stay where they are spoken (and within the cue)
        for (CueWord& w : c.words) {
            w.to = std::clamp(w.to - d, 1, c.length());
            w.from = std::clamp(w.from - d, 0, w.to - 1);
        }
        c.id = 0;
    }
    return moved;
}

bool isVoiceName(const QString& name)
{
    // Word starts, so "Comic" or "Music" are no "mic"; "Mix" and the Polish "Mikser" (mixer) never match
    static const char* const kStarts[] = {"mic",    "mikro", "микро", "voice",  "voix",   "voz",     "stimm",
                                          "głos",   "голос", "speech", "sprach", "narrat", "comment", "kommentar",
                                          "headset"};
    static const QRegularExpression split("[^\\p{L}]+");
    // A mix that has the voice in it ("System + Mikrofon", Clipi's mix stream) brings the rest along
    if (name.contains('+') || name.contains('&') || name.contains(',')) return false;
    const QStringList words = name.toLower().split(split, Qt::SkipEmptyParts);
    if (words.contains("mix")) return false;
    for (const QString& word : words)
        for (const char* start : kStarts)
            if (word.startsWith(QString::fromUtf8(start))) return true;
    return false;
}

QVector<int> defaultStreams(const MediaInfo& info)
{
    QVector<int> voice;
    if (info.audioStreamCount() > 1)
        for (int i = 0; i < info.audioStreams.size(); ++i)
            if (info.audioStreamPlayable(i) && isVoiceName(info.audioStreams[i].title)) voice << i;
    if (!voice.isEmpty()) return voice;
    for (int i = 0; i < info.audioStreamCount(); ++i) // the first stream that can be played
        if (info.audioStreamPlayable(i)) return {i};
    return {0};
}

QVector<int> voiceTracks(const Timeline& tl, const std::function<QString(const Clip&)>& streamName)
{
    const bool anySolo = std::any_of(tl.audio.begin(), tl.audio.end(), [](const Track& t) { return t.solo; });
    QVector<int> voice;
    int audible = 0;
    for (int i = 0; i < tl.audio.size(); ++i) {
        const Track& t = tl.audio[i];
        if (t.muted || (anySolo && !t.solo) || t.clips.isEmpty()) continue;
        ++audible;
        bool named = isVoiceName(t.name);
        for (const Clip& c : t.clips)
            named = named || (streamName && isVoiceName(streamName(c)));
        if (named) voice << i;
    }
    return voice.size() < audible ? voice : QVector<int>{};
}

} // namespace Transcript
