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
