// SPDX-License-Identifier: AGPL-3.0-only
#include "core/TranscriptCut.h"

#include "core/Transcript.h"

#include <QHash>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace TranscriptCut {

namespace {

const MediaInfo* findMedia(const QVector<MediaInfo>& media, const QString& path)
{
    for (const MediaInfo& m : media)
        if (m.path == path) return &m;
    return nullptr;
}

bool usable(const Clip& c) { return c.enabled && c.kind == ClipKind::Media && !c.isRetimed(); }

template<typename Fn>
void forClips(const Timeline& tl, int audioTrack, Fn fn)
{
    for (int t = 0; t < tl.audio.size(); ++t)
        if (audioTrack < 0 || audioTrack == t)
            for (const Clip& c : tl.audio[t].clips)
                if (usable(c)) fn(c, t);
}

} // namespace

QVector<Word> timelineWords(const Timeline& tl, const QVector<MediaInfo>& media, double fps, int audioTrack)
{
    const auto frame = [fps](qint64 ms) { return int(std::floor(ms * fps / 1000.0 + 0.5)); };
    QVector<Word> out;
    forClips(tl, audioTrack, [&](const Clip& c, int track) {
        const MediaInfo* m = findMedia(media, c.mediaPath);
        const MediaTranscript* tr = m ? m->transcript(c.audioStream) : nullptr;
        if (!tr) return;
        for (const TranscriptWord& w : tr->words) {
            if (w.text.isEmpty()) continue; // placeholder of a transcript without speech
            const int sf = frame(w.from);
            if (sf < c.in || sf > c.out) continue;
            Word word;
            word.from = c.start + sf - c.in;
            word.to = std::clamp(c.start + frame(w.to) - c.in, word.from + 1, std::max(word.from + 1, c.end()));
            word.spokenTo = std::clamp(c.start + frame(Transcript::spokenEnd(w)) - c.in, word.from + 1, word.to);
            word.text = w.text;
            word.clipId = c.id;
            word.track = track;
            out << word;
        }
    });
    std::stable_sort(out.begin(), out.end(), [](const Word& a, const Word& b) {
        return a.from != b.from ? a.from < b.from : a.track < b.track;
    });
    // Same file and stream on two tracks (e.g. a copy) -> one word
    QVector<Word> unique;
    for (const Word& w : out)
        if (unique.isEmpty() || unique.last().from != w.from || unique.last().text != w.text) unique << w;
    return unique;
}

QVector<QPair<QString, int>> missingTranscripts(const Timeline& tl, const QVector<MediaInfo>& media, int audioTrack)
{
    QVector<QPair<QString, int>> out;
    forClips(tl, audioTrack, [&](const Clip& c, int) {
        const MediaInfo* m = findMedia(media, c.mediaPath);
        if (!m || m->transcript(c.audioStream)) return;
        const QPair<QString, int> key{c.mediaPath, c.audioStream};
        if (!out.contains(key)) out << key;
    });
    return out;
}

QPair<int, int> cutRange(const QVector<Word>& words, int first, int last, int maxPauseFrames)
{
    if (first < 0 || last >= words.size() || first > last) return {0, 0};
    const Word& end = words[last];
    int to = end.spokenTo;
    if (last + 1 < words.size()) {
        const Word& next = words[last + 1];
        if (next.from >= end.spokenTo && next.from - end.spokenTo <= maxPauseFrames) to = next.from;
    }
    return {words[first].from, std::max(words[first].from + 1, to)};
}

QVector<QPair<int, int>> pauseRanges(const QVector<Word>& words, int minFrames, int keepFrames)
{
    QVector<QPair<int, int>> out;
    keepFrames = std::max(0, keepFrames);
    for (int i = 0; i + 1 < words.size(); ++i) {
        const Word& a = words[i];
        const Word& b = words[i + 1];
        if (a.clipId != b.clipId) continue;
        const int gap = b.from - a.spokenTo;
        if (gap <= std::max(minFrames, keepFrames)) continue;
        const int from = a.spokenTo + keepFrames / 2;
        const int to = b.from - (keepFrames - keepFrames / 2);
        if (to > from) out << qMakePair(from, to);
    }
    return mergeRanges(out);
}

QVector<QPair<int, int>> mergeRanges(QVector<QPair<int, int>> ranges)
{
    ranges.removeIf([](const QPair<int, int>& r) { return r.second <= r.first; });
    std::sort(ranges.begin(), ranges.end());
    QVector<QPair<int, int>> out;
    for (const auto& r : ranges) {
        if (!out.isEmpty() && r.first <= out.last().second) out.last().second = std::max(out.last().second, r.second);
        else out << r;
    }
    return out;
}

} // namespace TranscriptCut
