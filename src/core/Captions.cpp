// SPDX-License-Identifier: AGPL-3.0-only
#include "core/Captions.h"

#include <QRegularExpression>
#include <algorithm>
#include <climits>
#include <cmath>

namespace Captions {

QStringList words(const QString& text)
{
    static const QRegularExpression space(QStringLiteral("\\s+"));
    return text.split(space, Qt::SkipEmptyParts);
}

namespace {

bool ownTimes(const SubtitleCue& cue, const QStringList& list)
{
    return !cue.words.isEmpty() && cue.words.size() == list.size();
}

} // namespace

QVector<CueWord> timing(const SubtitleCue& cue)
{
    const QStringList list = words(cue.text);
    if (ownTimes(cue, list)) return cue.words;
    QVector<CueWord> out;
    if (list.isEmpty()) return out;
    // By length (a long word takes longer to say), the space after each word counted
    qint64 total = 0;
    for (const QString& w : list) total += w.size() + 1;
    const int length = std::max(1, cue.length());
    qint64 sum = 0;
    for (const QString& w : list) {
        const int from = int(sum * length / total);
        sum += w.size() + 1;
        out << CueWord{from, std::max(from + 1, int(sum * length / total))};
    }
    return out;
}

int activeWord(const SubtitleCue& cue, int frame)
{
    if (frame < cue.start || frame >= cue.end) return -1;
    const QVector<CueWord> t = timing(cue);
    const int rel = frame - cue.start;
    int index = -1;
    for (int i = 0; i < t.size(); ++i)
        if (t[i].from <= rel) index = i;
    return index;
}

Pop popIn(double ageMs, int popMs)
{
    if (popMs <= 0 || ageMs >= popMs) return {};
    const double t = std::clamp(ageMs / popMs, 0.0, 1.0);
    // Ease out with an overshoot ("back" easing): 0 -> 1, peaking at about 1.19 halfway, so the word grows from 60 %
    // to about 108 % and settles
    constexpr double c1 = 2.5, c3 = c1 + 1;
    const double u = t - 1;
    const double ease = 1 + c3 * u * u * u + c1 * u * u;
    return {0.6 + 0.4 * ease, std::min(1.0, 2 * t)};
}

QVector<int> lineStarts(const QVector<double>& widths, double spaceWidth, double maxWidth, int maxWords)
{
    QVector<int> out;
    double line = 0;
    int count = 0;
    for (int i = 0; i < widths.size(); ++i) {
        const double next = count ? line + spaceWidth + widths[i] : widths[i];
        if (count == 0 || next > maxWidth || (maxWords > 0 && count >= maxWords)) {
            out << i;
            line = widths[i];
            count = 1;
        } else {
            line = next;
            ++count;
        }
    }
    return out;
}

void remap(SubtitleCue& cue, const std::function<int(int)>& pos, int newStart, int newEnd)
{
    const QStringList list = words(cue.text);
    if (ownTimes(cue, list)) {
        QStringList kept;
        QVector<CueWord> times;
        for (int i = 0; i < list.size(); ++i) {
            const int from = pos(cue.start + cue.words[i].from), to = std::min(pos(cue.start + cue.words[i].to), newEnd);
            if (to <= from || from < newStart || from >= newEnd) continue;
            kept << list[i];
            times << CueWord{from - newStart, to - newStart};
        }
        if (kept.size() != list.size()) cue.text = kept.join(' ');
        cue.words = times;
    }
    cue.start = newStart;
    cue.end = newEnd;
}

void crop(SubtitleCue& cue, int start, int end)
{
    remap(cue, [](int f) { return f; }, start, end);
}

bool lostAllWords(const SubtitleCue& before, const SubtitleCue& after)
{
    return !before.text.trimmed().isEmpty() && after.text.trimmed().isEmpty();
}

int maxStartDelta(const SubtitleCue& cue)
{
    if (!ownTimes(cue, words(cue.text))) return INT_MAX;
    return cue.words.last().from;
}

int minEndDelta(const SubtitleCue& cue)
{
    if (!ownTimes(cue, words(cue.text))) return INT_MIN;
    return cue.words.first().from + 1 - cue.length();
}

TitleStyle defaultStyle(QSize format, CaptionMode mode)
{
    TitleStyle s = subtitleBaseStyle();
    s.text.clear();
    s.bold = true;
    s.boxOn = false;
    s.outlineOn = true;
    const int w = std::max(1, format.width()), h = std::max(1, format.height());
    const double k = std::min(w, h) / 1080.0;
    const double big = mode == CaptionMode::Word ? 1.5 : 1.0;
    s.size = std::round(84 * k * big);
    s.outlineWidth = std::round(6 * k * big * 10) / 10;
    s.posY = std::round(h > w ? 0.30 * h : 0.10 * h);
    return s;
}

double lineWidth(QSize format)
{
    return format.width() * (format.height() > format.width() ? 0.76 : 0.85);
}

const CaptionModeInfo* modeInfo(const QString& id)
{
    for (const CaptionModeInfo& m : kCaptionModes)
        if (id == QLatin1String(m.id)) return &m;
    return nullptr;
}

const CaptionModeInfo& modeInfo(CaptionMode mode)
{
    for (const CaptionModeInfo& m : kCaptionModes)
        if (m.mode == mode) return m;
    return kCaptionModes[0];
}

QJsonObject toJson(const CaptionStyle& c)
{
    const CaptionStyle d;
    QJsonObject o;
    if (c.mode != d.mode) o["mode"] = modeInfo(c.mode).id;
    if (c.highlight != d.highlight) o["highlight"] = c.highlight.name(QColor::HexArgb);
    if (c.highlightScale != d.highlightScale) o["highlightScale"] = c.highlightScale;
    if (c.maxWordsPerLine != d.maxWordsPerLine) o["maxWordsPerLine"] = c.maxWordsPerLine;
    if (c.popMs != d.popMs) o["popMs"] = c.popMs;
    if (c.uppercase != d.uppercase) o["uppercase"] = c.uppercase;
    if (c.shadow != d.shadow) o["shadow"] = c.shadow;
    return o;
}

CaptionStyle fromJson(const QJsonObject& o, const CaptionStyle& base)
{
    CaptionStyle c = base;
    if (o.contains("mode")) {
        const CaptionModeInfo* m = modeInfo(o.value("mode").toString());
        c.mode = m ? m->mode : CaptionMode::Lines;
    }
    const QColor h(o.value("highlight").toString());
    if (h.isValid()) c.highlight = h;
    c.highlightScale = std::clamp(o.value("highlightScale").toDouble(c.highlightScale), 0.5, 3.0);
    c.maxWordsPerLine = std::clamp(o.value("maxWordsPerLine").toInt(c.maxWordsPerLine), 0, 50);
    c.popMs = std::clamp(o.value("popMs").toInt(c.popMs), 0, 2000);
    c.uppercase = o.value("uppercase").toBool(c.uppercase);
    c.shadow = o.value("shadow").toBool(c.shadow);
    return c;
}

QVector<TranscriptCut::Word> onBeats(const QVector<TranscriptCut::Word>& words, const QVector<int>& beats)
{
    if (beats.isEmpty()) return words;
    QVector<TranscriptCut::Word> out;
    int next = 0; // first beat still free
    for (const TranscriptCut::Word& w : words) {
        if (next >= beats.size()) break; // the song ends: the remaining words have no beat
        int best = next;
        for (int j = next; j < beats.size(); ++j) {
            if (std::abs(beats[j] - w.from) < std::abs(beats[best] - w.from)) best = j;
            if (beats[j] > w.from) break;
        }
        TranscriptCut::Word moved = w;
        moved.from = beats[best];
        out << moved;
        next = best + 1;
    }
    for (int i = 0; i < out.size(); ++i) {
        const int length = std::max(1, words[i].spokenTo - words[i].from);
        const int end = i + 1 < out.size() ? out[i + 1].from : out[i].from + length;
        out[i].to = out[i].spokenTo = std::max(out[i].from + 1, end);
    }
    return out;
}

QVector<SubtitleCue> fromWords(const QVector<TranscriptCut::Word>& input, const Options& o)
{
    QVector<TranscriptCut::Word> spoken;
    for (const TranscriptCut::Word& w : input)
        if (!w.text.trimmed().isEmpty()) spoken << w;
    const QVector<TranscriptCut::Word> list = onBeats(spoken, o.beats);
    QVector<SubtitleCue> cues;
    const auto add = [&](int a, int b) { // words [a, b]
        if (a > b) return;
        SubtitleCue c;
        c.start = list[a].from;
        if (!cues.isEmpty()) c.start = std::max(c.start, cues.last().end);
        c.end = std::max(c.start + 1, list[b].spokenTo);
        QStringList text;
        for (int k = a; k <= b; ++k) {
            text << list[k].text.trimmed();
            const int from = std::max(0, list[k].from - c.start);
            c.words << CueWord{from, std::max(from + 1, list[k].spokenTo - c.start)};
        }
        c.text = text.join(' ');
        cues << c;
    };
    const int maxWords = std::max(1, o.maxWords);
    int first = 0;
    for (int i = 0; i < list.size(); ++i) {
        if (i > first && list[i].from - list[i - 1].spokenTo > o.maxGapFrames) add(first, i - 1), first = i;
        const QString t = list[i].text.trimmed();
        const QChar last = t.back();
        const int count = i - first + 1;
        if (count >= maxWords || last == '.' || last == '?' || last == '!'
            || ((last == ',' || last == ';' || last == ':') && count * 2 >= maxWords))
            add(first, i), first = i + 1;
    }
    add(first, int(list.size()) - 1);
    // Long enough to read, up to the next cue
    for (int i = 0; i < cues.size(); ++i) {
        int end = std::max(cues[i].end, cues[i].start + o.minFrames);
        if (i + 1 < cues.size()) end = std::min(end, cues[i + 1].start);
        cues[i].end = std::max(cues[i].end, end);
    }
    return cues;
}

} // namespace Captions
