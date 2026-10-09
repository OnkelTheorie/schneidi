#include "core/Transcript.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace Transcript {

bool parseWhisperJson(const QByteArray& json, Result* out, QString* error)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = "invalid whisper output: " + err.errorString();
        return false;
    }
    const QJsonObject root = doc.object();
    out->language = root["result"].toObject()["language"].toString();
    out->words.clear();
    for (const QJsonValue& v : root["transcription"].toArray()) {
        const QJsonObject seg = v.toObject();
        const QString text = seg["text"].toString().trimmed();
        if (text.isEmpty() || (text.startsWith('[') && text.endsWith(']'))) continue; // pause, [_BEG_], [Music]
        const QJsonObject off = seg["offsets"].toObject();
        Word w{qint64(off["from"].toDouble()), qint64(off["to"].toDouble()), text};
        if (w.to < w.from) w.to = w.from;
        // Punctuation that whisper split off ("," as its own word) belongs to the word before
        if (!out->words.isEmpty() && text.size() <= 2 && !text.at(0).isLetterOrNumber()) {
            out->words.last().text += text;
            out->words.last().to = std::max(out->words.last().to, w.to);
            continue;
        }
        out->words << w;
    }
    return true;
}

qint64 spokenEnd(const Word& w)
{
    // About 70 ms per character plus 250 ms: generous for slow speakers, cuts long pauses
    return std::min(w.to, w.from + 250 + 70 * qint64(w.text.size()));
}

namespace {
qint64 spokenLength(const Word& w) { return 250 + 70 * qint64(w.text.size()); }
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
    for (SubtitleCue& c : cues) {
        const int a = int(std::max(0.0, (c.start - offset) * 1000.0 / fps / fm));
        const int b = int(std::min<double>(levels.db.size(), (c.end - offset) * 1000.0 / fps / fm));
        for (int f = a; f + kRun <= b; ++f) {
            if (voiced[f + kRun] - voiced[f] < kRun) continue;
            // 100 ms before the sound, so the line is there when the first syllable is
            const int start = offset + int(std::floor((f * fm - 100) / 1000.0 * fps));
            if (start > c.start && start < c.end - 1) c.start = start;
            break;
        }
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
    for (int i = 0; i < words.size();) {
        if (!stretched(words[i])) {
            ++i;
            continue;
        }
        // A run of stretched words, e.g. "Das ist scheiße" over 9 s of laughter: they belong together
        int j = i;
        while (j + 1 < words.size() && stretched(words[j + 1]) && words[j + 1].from - words[j].to < kTouch) ++j;
        qint64 len = 0;
        for (int k = i; k <= j; ++k) len += spokenLength(words[k]);
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
        i = j + 1;
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

} // namespace Transcript
