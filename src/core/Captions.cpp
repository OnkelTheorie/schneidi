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

bool timed(const SubtitleCue& cue)
{
    return ownTimes(cue, words(cue.text));
}

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

QVector<QPair<int, int>> phrases(const QVector<TranscriptCut::Word>& words, int pauseFrames)
{
    QVector<QPair<int, int>> out;
    for (int i = 0; i < words.size(); ++i) {
        const QString before = i > 0 ? words[i - 1].text.trimmed() : QString();
        const bool ended = i == 0 || before.isEmpty() || before.endsWith('.') || before.endsWith('?') || before.endsWith('!')
                           || words[i].from - words[i - 1].spokenTo > pauseFrames;
        if (ended) out << qMakePair(i, i);
        else out.last().second = i;
    }
    return out;
}

QVector<TranscriptCut::Word> mergeSpeakers(const QVector<QVector<TranscriptCut::Word>>& speakers)
{
    QVector<TranscriptCut::Word> out;
    for (int k = 0; k < speakers.size(); ++k)
        for (TranscriptCut::Word w : speakers[k]) {
            if (w.text.trimmed().isEmpty()) continue;
            w.track = k;
            out << w;
        }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.from != b.from ? a.from < b.from : a.track < b.track;
    });
    return out;
}

int snapToSound(QVector<TranscriptCut::Word>& words, const std::vector<float>& framePeaks, double fps, int pauseFrames,
                const SnapOptions& o)
{
    const int n = int(framePeaks.size());
    if (words.isEmpty() || n == 0 || fps <= 0) return 0;
    const int window = std::max(1, int(std::lround(o.windowS * fps)));
    const int minLen = std::max(1, int(std::lround(o.minWordS * fps)));
    const int fill = std::max(1, int(std::lround(0.1 * fps)));     // a pause inside a word (a plosive's stop)
    const int minRun = std::max(1, int(std::lround(0.035 * fps))); // a click is no speech
    // Levels (dBFS of the peak; nothing = -100) and the stream's quiet and speech around the words
    std::vector<double> db(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) db[size_t(i)] = framePeaks[size_t(i)] > 1e-5f ? 20 * std::log10(framePeaks[size_t(i)]) : -100.0;
    const int a = std::clamp(words.first().from - window, 0, n), b = std::clamp(words.last().spokenTo + window, 0, n);
    if (b - a < 2 * window) return 0;
    std::vector<double> sorted(db.begin() + a, db.begin() + b);
    std::sort(sorted.begin(), sorted.end());
    const double quiet = sorted[size_t(0.2 * (sorted.size() - 1))], speech = sorted[size_t(0.95 * (sorted.size() - 1))];
    if (speech - quiet < o.minContrastDb) return 0; // constant noise (or no speech): no edges to trust
    const double threshold = std::max(quiet + 10, speech - 30);
    std::vector<char> on(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) on[size_t(i)] = db[size_t(i)] > threshold;
    // Short pauses inside speech filled, short blips dropped
    const auto runs = [&](char value, int maxLength, char set, bool inner) {
        for (int i = 0; i < n;) {
            if (on[size_t(i)] != value) {
                ++i;
                continue;
            }
            int j = i;
            while (j < n && on[size_t(j)] == value) ++j;
            if (j - i <= maxLength && (!inner || (i > 0 && j < n)))
                for (int k = i; k < j; ++k) on[size_t(k)] = set;
            i = j;
        }
    };
    runs(0, fill, 1, true);
    runs(1, minRun - 1, 0, false);
    const auto isOn = [&](int f) { return f >= 0 && f < n && on[size_t(f)]; };

    int moved = 0;
    int before = INT_MIN; // where the speaker's phrase before ends being spoken
    for (const auto& [p, q] : phrases(words, pauseFrames)) {
        const int next = q + 1 < words.size() ? words[q + 1].from : INT_MAX;
        const int oldStart = words[p].from, oldEnd = words[q].spokenTo;
        // Onset: inside speech at whisper's start -> back to where it begins; in the quiet -> the first speech after it
        int start = oldStart;
        if (isOn(oldStart)) {
            int s = oldStart;
            while (s > oldStart - window && s > before && isOn(s - 1)) --s;
            if (!isOn(s - 1)) start = s; // else no clear edge (speech on beyond the window or up to the phrase before)
        } else {
            for (int f = oldStart + 1; f <= oldStart + window && f < next && f < n; ++f)
                if (isOn(f)) {
                    start = f;
                    break;
                }
        }
        // Offset: inside speech at the end -> on to where it stops; in the quiet -> back to the last speech
        // (from the onset on when that lies after whisper's end: a word placed before its sound)
        int end = oldEnd;
        const int ref = std::max(oldEnd, start + 1);
        if (isOn(ref - 1)) {
            int e = ref;
            while (e < ref + window && e < next && isOn(e)) ++e;
            if (!isOn(e)) end = e;
        } else {
            int e = ref - 1;
            while (e >= ref - window && e >= start && !isOn(e)) --e;
            if (e >= start && isOn(e)) end = e + 1;
        }
        if (end <= start || (start == oldStart && end == oldEnd)) {
            before = words[q].spokenTo;
            continue;
        }
        const auto fits = [&](const QVector<TranscriptCut::Word>& t) {
            for (int i = 0; i < t.size(); ++i) {
                const TranscriptCut::Word& old = words[p + i];
                const int length = t[i].spokenTo - t[i].from;
                if (length < std::min(minLen, old.spokenTo - old.from) || length < 1) return false;
                if (i + 1 < t.size() && t[i].spokenTo > t[i + 1].from) return false;
            }
            return true;
        };
        // Inner words kept
        QVector<TranscriptCut::Word> trial = words.mid(p, q - p + 1);
        trial.first().from = start;
        trial.last().spokenTo = end;
        if (!fits(trial)) {
            // ... or spread in proportion
            trial = words.mid(p, q - p + 1);
            const double k = double(end - start) / std::max(1, oldEnd - oldStart);
            const auto map = [&](int t) { return start + int(std::lround((t - oldStart) * k)); };
            for (TranscriptCut::Word& w : trial) {
                w.from = map(w.from);
                w.spokenTo = map(w.spokenTo);
                w.to = map(w.to);
            }
            trial.first().from = start;
            trial.last().spokenTo = end;
            if (!fits(trial)) {
                before = words[q].spokenTo;
                continue;
            }
        }
        for (int i = 0; i < trial.size(); ++i) {
            TranscriptCut::Word& w = trial[i];
            const int limit = i + 1 < trial.size() ? trial[i + 1].from : next;
            w.to = std::max(w.spokenTo, std::min(w.to, limit));
            if (i + 1 == trial.size()) w.to = std::max(w.to, end);
            words[p + i] = w;
        }
        // The phrase before ends where this one begins now at the latest
        if (p > 0) words[p - 1].to = std::max(words[p - 1].spokenTo, std::min(words[p - 1].to, start));
        before = end;
        ++moved;
    }
    return moved;
}

QColor speakerColor(int k, const QColor& first)
{
    if (k <= 0) return first;
    // Bright and clearly apart from the default yellow and from each other: cyan, pink, lime
    static const QColor palette[] = {QColor(64, 216, 255), QColor(255, 106, 213), QColor(140, 255, 90)};
    return palette[(k - 1) % 3];
}

void stackLanes(QVector<SubtitleTrack>& lanes, const std::function<double(const SubtitleTrack&, const SubtitleCue&)>& height)
{
    if (lanes.size() < 2) return;
    const double base = lanes.first().style.posY;
    for (int k = 1; k < lanes.size(); ++k) {
        double y = base;
        for (int j = 0; j < k; ++j) {
            double tallest = 0;
            for (const SubtitleCue& low : lanes[j].cues)
                for (const SubtitleCue& high : lanes[k].cues)
                    if (low.start < high.end && high.start < low.end && !low.text.trimmed().isEmpty()) {
                        tallest = std::max(tallest, height(lanes[j], low));
                        break;
                    }
            if (tallest > 0) y = std::max(y, lanes[j].style.posY + tallest + 0.2 * lanes[j].style.size);
        }
        lanes[k].style.posY = std::round(y);
    }
}

QVector<SubtitleCue> fromWords(const QVector<TranscriptCut::Word>& input, const Options& o)
{
    QVector<TranscriptCut::Word> spoken;
    for (const TranscriptCut::Word& w : input)
        if (!w.text.trimmed().isEmpty()) spoken << w;
    const QVector<TranscriptCut::Word> list = onBeats(spoken, o.beats);
    QVector<SubtitleCue> cues;
    QVector<int> speaker; // of each cue
    const auto add = [&](int a, int b) { // words [a, b]
        if (a > b) return;
        speaker << list[a].track;
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
    // The first cut after frame `f`; INT_MAX = none
    const auto cutAfter = [&](int f) {
        const auto it = std::upper_bound(o.cuts.begin(), o.cuts.end(), f);
        return it == o.cuts.end() ? INT_MAX : *it;
    };
    for (int i = 0; i < list.size(); ++i) {
        if (i > first && list[i].from - list[i - 1].spokenTo > o.maxGapFrames) add(first, i - 1), first = i;
        // Another speaker (another track's words): a cue of their own
        if (i > first && list[i].track != list[i - 1].track) add(first, i - 1), first = i;
        // A cut between two clips: never one caption over it
        if (i > first && cutAfter(list[i - 1].from) <= list[i].from) add(first, i - 1), first = i;
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
        // ... never on into the next clip
        if (const int cut = cutAfter(cues[i].start); cut < cues[i].end) {
            cues[i].end = cut;
            for (CueWord& w : cues[i].words) {
                w.to = std::clamp(w.to, 1, cues[i].length());
                w.from = std::clamp(w.from, 0, w.to - 1);
            }
        }
    }
    // A cue the cut leaves too short to read: joins the one before when its words are heard, else it goes
    if (o.minAtCutFrames > 0) {
        QVector<SubtitleCue> kept;
        QVector<int> keptSpeaker;
        for (int i = 0; i < cues.size(); ++i) {
            SubtitleCue c = cues[i];
            const int cut = cutAfter(c.start);
            if (cut != c.end || c.length() >= o.minAtCutFrames) {
                kept << c;
                keptSpeaker << speaker[i];
                continue;
            }
            const int first = c.start + (c.words.isEmpty() ? 0 : c.words.first().from);
            if (cut - first < o.heardFrames) continue; // its word is cut off before it is heard
            if (!kept.isEmpty()) {
                SubtitleCue& prev = kept.last();
                const bool joinable = keptSpeaker.last() == speaker[i] && c.start - prev.end <= o.maxGapFrames
                                      && cutAfter(prev.start) >= c.end && timed(prev) && timed(c);
                if (joinable) {
                    for (const CueWord& w : c.words)
                        prev.words << CueWord{w.from + c.start - prev.start, w.to + c.start - prev.start};
                    prev.text += ' ' + c.text;
                    prev.end = c.end;
                    continue;
                }
            }
            kept << c;
            keptSpeaker << speaker[i];
        }
        cues = kept;
    }
    return cues;
}

} // namespace Captions
