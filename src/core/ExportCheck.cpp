#include "core/ExportCheck.h"

#include "core/I18n.h"
#include "core/Loudness.h"

#include <QFileInfo>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace ExportCheck {

namespace {

QString num(double v, int decimals = 1) { return QString::number(v, 'f', decimals).replace('.', I18n::decimalPoint()); }

Issue issue(Severity s, const char* check, const QString& message, int from = -1, int to = -1)
{
    return {s, QString::fromLatin1(check), message, from, to};
}

// Merged [from, to) ranges covered by visible picture inside [lo, hi)
QVector<QPair<int, int>> pictureCoverage(const Timeline& tl, int lo, int hi)
{
    QVector<QPair<int, int>> ranges;
    for (const Track& t : tl.video) {
        if (t.hidden) continue;
        for (const Clip& c : t.clips)
            if (c.enabled && c.end() > lo && c.start < hi) ranges.append({std::max(lo, c.start), std::min(hi, c.end())});
    }
    std::sort(ranges.begin(), ranges.end());
    QVector<QPair<int, int>> merged;
    for (const auto& r : ranges) {
        if (!merged.isEmpty() && r.first <= merged.last().second) merged.last().second = std::max(merged.last().second, r.second);
        else merged.append(r);
    }
    return merged;
}

} // namespace

const char* severityId(Severity s)
{
    switch (s) {
    case Severity::Error: return "error";
    case Severity::Warning: return "warning";
    case Severity::Info: return "info";
    }
    return "info";
}

QVector<Issue> timeline(const Timeline& tl, int from, int to, const ProjectFormat& f, const Options& o)
{
    QVector<Issue> out;
    if (to <= from) return out;
    const double fps = f.rate.fps();

    // Media that are not where the project says (moved, deleted, other computer): renders as a placeholder
    QSet<QString> seen;
    for (const QVector<Track>* tracks : {&tl.video, &tl.audio})
        for (const Track& t : *tracks)
            for (const Clip& c : t.clips) {
                if (!c.enabled || c.kind != ClipKind::Media || c.mediaPath.isEmpty() || c.end() <= from || c.start >= to)
                    continue;
                if (seen.contains(c.mediaPath) || QFileInfo::exists(c.mediaPath)) continue;
                seen.insert(c.mediaPath);
                out << issue(Severity::Error, "offline", T("Datei fehlt: %1").arg(c.mediaPath), std::max(from, c.start),
                             std::min(to, c.end()));
            }

    // Frames without any picture: black in the video (a forgotten frame between two clips is the classic)
    const QVector<QPair<int, int>> covered = pictureCoverage(tl, from, to);
    if (covered.isEmpty()) {
        out << issue(Severity::Warning, "black", T("Die Timeline hat kein Bild (nur Ton)"), from, to);
    } else {
        int at = from;
        const auto gap = [&](int a, int b) {
            const int n = b - a;
            out << issue(Severity::Warning, "black",
                         n == 1 ? T("Lücke: 1 Bild schwarz") : T("Lücke: %1 Bilder schwarz (%2 s)").arg(n).arg(num(n / fps)),
                         a, b);
        };
        for (const auto& [a, b] : covered) {
            if (a > at) gap(at, a);
            at = b;
        }
        if (at < to) gap(at, to);
    }

    // Subtitles: readable long enough, not faster than people read, not cut off by the end
    for (const SubtitleTrack& st : tl.subtitles) {
        if (!st.enabled) continue;
        for (const SubtitleCue& cue : st.cues) {
            if (cue.end <= from || cue.start >= to) continue;
            const double seconds = cue.length() / fps;
            if (cue.end > to)
                out << issue(Severity::Warning, "subtitle_end", T("Untertitel läuft über das Ende hinaus: „%1“").arg(cue.text.simplified()),
                             cue.start, cue.end);
            if (seconds < o.subtitleMinS)
                out << issue(Severity::Warning, "subtitle_short",
                             T("Untertitel zu kurz zum Lesen (%1 s): „%2“").arg(num(seconds), cue.text.simplified()),
                             cue.start, cue.end);
            QString letters = cue.text;
            letters.remove('\n');
            const double cps = seconds > 0 ? letters.trimmed().size() / seconds : 0;
            if (seconds >= o.subtitleMinS && cps > o.subtitleMaxCps)
                out << issue(Severity::Warning, "subtitle_fast",
                             T("Untertitel zu schnell (%1 Zeichen/s, höchstens %2): „%3“")
                                 .arg(num(cps, 0), num(o.subtitleMaxCps, 0), cue.text.simplified()),
                             cue.start, cue.end);
        }
    }
    return out;
}

QVector<QPair<int, int>> silentStretches(const std::vector<float>& framePeaks, int offset, double thresholdDb,
                                         int minFrames)
{
    const float limit = float(std::pow(10.0, thresholdDb / 20));
    QVector<QPair<int, int>> out;
    const int n = int(framePeaks.size());
    for (int i = 0; i < n;) {
        if (framePeaks[size_t(i)] >= limit) {
            ++i;
            continue;
        }
        int j = i;
        while (j < n && framePeaks[size_t(j)] < limit) ++j;
        if (j - i >= minFrames) out.append({offset + i, offset + j});
        i = j;
    }
    return out;
}

QVector<Issue> sound(const std::vector<float>& framePeaks, int offset, double integratedLufs, double truePeakDb,
                     const ProjectFormat& f, const Options& o, bool limiter)
{
    QVector<Issue> out;
    const int n = int(framePeaks.size());
    const double fps = f.rate.fps();
    const QVector<QPair<int, int>> silent = silentStretches(framePeaks, offset, o.silenceDb, 1);
    if (n == 0 || integratedLufs <= LoudnessMeter::kSilence
        || (silent.size() == 1 && silent[0].first == offset && silent[0].second == offset + n)) {
        out << issue(Severity::Warning, "no_sound", T("Das Video hat keinen Ton"));
        return out;
    }

    if (truePeakDb > 0)
        out << issue(Severity::Error, "clipping",
                     (limiter ? T("Übersteuert: True Peak %1 dBTP (über 0 dB verzerrt)")
                              : T("Übersteuert: True Peak %1 dBTP (über 0 dB verzerrt) – Limiter im Mixer einschalten "
                                  "oder leiser mischen"))
                         .arg(num(truePeakDb)));
    else if (truePeakDb > o.maxTruePeakDb)
        out << issue(Severity::Warning, "true_peak",
                     (limiter ? T("True Peak %1 dBTP liegt über %2 dBTP (Codecs können übersteuern)")
                              : T("True Peak %1 dBTP liegt über %2 dBTP (Codecs können übersteuern) – Limiter im "
                                  "Mixer einschalten"))
                         .arg(num(truePeakDb), num(o.maxTruePeakDb)));
    if (std::abs(integratedLufs - o.targetLufs) > o.toleranceLu)
        out << issue(Severity::Warning, "loudness",
                     T("Lautheit %1 LUFS, Ziel %2 LUFS (%3)")
                         .arg(num(integratedLufs), num(o.targetLufs),
                              integratedLufs < o.targetLufs ? T("zu leise") : T("zu laut")));

    // Silence: at the end it looks like a forgotten tail, at the start and inside it may be intended
    const int endMin = int(std::lround(o.silenceEndS * fps)), middleMin = int(std::lround(o.silenceMiddleS * fps));
    for (const auto& [a, b] : silent) {
        const double seconds = (b - a) / fps;
        if (b == offset + n && b - a > endMin)
            out << issue(Severity::Warning, "silence_end", T("%1 s Stille am Ende").arg(num(seconds)), a, b);
        else if (a == offset && b - a > endMin)
            out << issue(Severity::Info, "silence_start", T("%1 s Stille am Anfang").arg(num(seconds)), a, b);
        else if (a != offset && b != offset + n && b - a > middleMin)
            out << issue(Severity::Info, "silence", T("%1 s Stille").arg(num(seconds)), a, b);
    }
    return out;
}

int errors(const QVector<Issue>& issues)
{
    return int(std::count_if(issues.begin(), issues.end(), [](const Issue& i) { return i.severity == Severity::Error; }));
}

int warnings(const QVector<Issue>& issues)
{
    return int(std::count_if(issues.begin(), issues.end(), [](const Issue& i) { return i.severity == Severity::Warning; }));
}

} // namespace ExportCheck
