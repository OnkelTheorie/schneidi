#include "core/Transcript.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

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
        // No "keep the start after a pause if there is sound": laughter is sound too (and whisper's voice detection
        // takes it for speech) – measured: "Diese" put on laughter at 6.5 s, spoken at 13.9 s
        // A run of stretched words, e.g. "Das ist scheiße" over 9 s of laughter: they belong together
        int j = i;
        while (j + 1 < words.size() && stretched(words[j + 1]) && words[j + 1].from - words[j].to < kTouch) ++j;
        qint64 len = 0;
        for (int k = i; k <= j; ++k) len += spokenLength(words[k]);
        const qint64 from = words[i].from, to = words[j].to;
        const bool nextTouches = j + 1 < words.size() && words[j + 1].from - to < kTouch;
        const bool prevTouches = i > 0 && from - words[i - 1].to < kTouch;
        qint64 start;
        if (nextTouches) {
            start = to - len; // spoken right before the next word (whisper hands the pause to the words before)
        } else if (prevTouches) {
            start = from;
        } else {
            // Alone between pauses: the loudest stretch of the spoken length, the last one of equal ones
            const int steps = int((to - from - len) / fm);
            QVector<int> score(steps + 1);
            int best = 0;
            for (int s = 0; s <= steps; ++s) best = std::max(best, score[s] = loud(from + qint64(s) * fm, from + qint64(s) * fm + len));
            int pick = steps;
            while (pick > 0 && score[pick] < best) --pick;
            start = best > 0 ? from + qint64(pick) * fm : from; // nothing loud (or no levels): as whisper says
        }
        for (int k = i; k <= j; ++k) {
            const qint64 l = spokenLength(words[k]);
            words[k].from = start;
            words[k].to = k == j && nextTouches ? to : start + l;
            start += l;
        }
        i = j + 1;
    }
}

QVector<SubtitleCue> toCues(const QVector<Word>& words, double fps, int maxChars, int maxGapMs, int offset)
{
    QVector<SubtitleCue> cues;
    const auto frame = [&](qint64 ms) { return offset + int(std::lround(ms / 1000.0 * fps)); };
    QString text;
    qint64 start = 0, end = 0;
    const auto flush = [&] {
        if (text.isEmpty()) return;
        int a = frame(start), b = std::max(frame(end), a + 1);
        if (!cues.isEmpty()) a = std::max(a, cues.last().end);
        cues << SubtitleCue{0, a, std::max(b, a + 1), text};
        text.clear();
    };
    for (int i = 0; i < words.size(); ++i) {
        const Word& w = words[i];
        const bool gap = !text.isEmpty() && w.from - end > maxGapMs;
        if (gap || (!text.isEmpty() && text.size() + 1 + w.text.size() > maxChars)) flush();
        if (text.isEmpty()) start = w.from;
        text += (text.isEmpty() ? "" : " ") + w.text;
        end = spokenEnd(w);
        const QChar last = w.text.back();
        // Sentence end: always a new cue; a comma in the second half of a full line: a good place for one
        if (last == '.' || last == '?' || last == '!' || ((last == ',' || last == ';') && text.size() > maxChars / 2))
            flush();
    }
    flush();
    return cues;
}

} // namespace Transcript
