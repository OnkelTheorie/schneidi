#include "engine/CheckMeasure.h"

#include "core/Loudness.h"
#include "engine/AudioAnalysis.h"
#include "engine/FrameGaps.h"
#include "engine/Snapshot.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <climits>
#include <cmath>

namespace CheckMeasure {

namespace {

// [from, to) frames covered by the track's enabled clips inside [lo, hi), merged
QVector<QPair<int, int>> coverage(const Track& t, int lo, int hi)
{
    QVector<QPair<int, int>> out;
    for (const Clip& c : t.clips) {
        if (!c.enabled || c.end() <= lo || c.start >= hi) continue;
        const int a = std::max(lo, c.start), b = std::min(hi, c.end());
        if (!out.isEmpty() && a <= out.last().second) out.last().second = std::max(out.last().second, b);
        else out.append({a, b});
    }
    return out;
}

} // namespace

std::optional<QVector<ExportCheck::Issue>> mix(const ProjectFormat& f, const Timeline& renderTl, int from, int to,
                                               const std::function<bool(const QString& path)>& hasPicture,
                                               const ExportCheck::Options& o, const Progress& progress)
{
    using ExportCheck::SoundRole;
    const int n = int(renderTl.audio.size());
    QVector<SoundRole> roles;
    QVector<QVector<QPair<int, int>>> covered;
    for (int i = 0; i < n; ++i) {
        roles << ExportCheck::soundRole(renderTl, i, hasPicture);
        covered << coverage(renderTl.audio[i], from, to);
    }
    // Only where music (or what may be music) plays together with another track is there anything to compare
    int a = INT_MAX, b = INT_MIN;
    QSet<int> measure;
    for (int m = 0; m < n; ++m) {
        if (roles[m] != SoundRole::Music && roles[m] != SoundRole::Undecided) continue;
        for (int j = 0; j < n; ++j) {
            if (j == m || roles[j] == SoundRole::Other) continue;
            for (const auto& [x0, x1] : covered[m])
                for (const auto& [y0, y1] : covered[j])
                    if (std::max(x0, y0) < std::min(x1, y1)) {
                        a = std::min(a, std::max(x0, y0));
                        b = std::max(b, std::min(x1, y1));
                        measure << m << j;
                    }
        }
    }
    if (measure.isEmpty()) return QVector<ExportCheck::Issue>{};

    QList<int> tracks = measure.values();
    std::sort(tracks.begin(), tracks.end());
    QVector<ExportCheck::TrackSound> sounds;
    bool canceled = false;
    for (int k = 0; k < tracks.size(); ++k) {
        const int i = tracks[k];
        Timeline alone = renderTl;
        for (int x = 0; x < n; ++x) {
            alone.audio[x].muted = x != i;
            alone.audio[x].solo = false;
        }
        alone.masterLimiter = false; // it would treat a loud track alone differently than in the mix
        LoudnessMeter meter;
        std::vector<float> peaks;
        peaks.reserve(size_t(b - a));
        const bool ok = AudioAnalysis::timelineSound(
            f, alone, a, b,
            [&](const float* pcm, int samples, int channels, int rate) {
                float peak = 0.f;
                if (pcm) {
                    meter.addPlanar(pcm, samples, channels, rate);
                    for (int s = 0; s < samples * channels; ++s) peak = std::max(peak, std::abs(pcm[s]));
                }
                peaks.push_back(peak);
            },
            [&](double p) {
                if (progress && !progress((k + p) / tracks.size())) canceled = true;
                return !canceled;
            });
        if (canceled) return std::nullopt;
        if (!ok) continue;
        const bool music = roles[i] == SoundRole::Music
                           || (roles[i] == SoundRole::Undecided && ExportCheck::continuous(renderTl, i, peaks, a));
        sounds << ExportCheck::TrackSound{i, music, meter.blocks()};
    }
    if (progress) progress(1.0);
    return ExportCheck::mix(renderTl, sounds, a, f, o);
}

std::optional<double> motion(const ProjectFormat& f, const Timeline& renderTl, const QVector<int>& frames)
{
    if (frames.size() < 2) return std::nullopt;
    const auto source = Snapshot::Source::timeline(f, renderTl);
    QVector<QImage> gray;
    for (int fr : frames) {
        // Rendered larger and shrunk by averaging: MLT's scaling straight to a few pixels samples single pixels, so a
        // slow zoom over a fine texture flickers like motion
        const QImage img = source->frame(fr, 384);
        if (img.isNull()) return std::nullopt;
        gray << img.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_Grayscale8);
    }
    // Only the picture counts: black bars of a fitted 16:9 clip (two thirds of a 9:16 frame) would dilute it
    double sum = 0;
    for (int k = 1; k < gray.size(); ++k) {
        const QImage& x = gray[k - 1];
        const QImage& y = gray[k];
        const int w = std::min(x.width(), y.width()), h = std::min(x.height(), y.height());
        qint64 diff = 0, n = 0;
        for (int row = 0; row < h; ++row) {
            const uchar* p = x.constScanLine(row);
            const uchar* q = y.constScanLine(row);
            for (int col = 0; col < w; ++col)
                if (std::max(p[col], q[col]) > 20) {
                    diff += std::abs(int(p[col]) - int(q[col]));
                    ++n;
                }
        }
        // Next to nothing but black (a black start): no motion
        const double d = n * 20 >= qint64(w) * h ? double(diff) / double(n) / 2.55 : 0.0;
        sum += d;
    }
    return sum / (gray.size() - 1);
}

QVector<ExportCheck::Issue> hook(const ProjectFormat& f, const Timeline& renderTl, int from, int to,
                                 const ExportCheck::Options& o)
{
    if (!ExportCheck::wantsHook(from, to, f, o)) return {};
    const QVector<int> frames = ExportCheck::hookFrames(from, to, f, o);
    // Only what can show in the first second (a dissolve shows the next clip before its start): building every clip
    // of the timeline would open all its files
    Timeline start = renderTl;
    const int until = frames.isEmpty() ? from : frames.last() + int(std::lround(2 * f.rate.fps()));
    for (Track& t : start.video) t.clips.removeIf([until](const Clip& c) { return c.start > until; });
    for (Track& t : start.audio) t.clips.clear();
    const auto m = motion(f, start, frames);
    return m ? ExportCheck::hook(*m, from, to, f, o) : QVector<ExportCheck::Issue>{};
}

std::optional<QVector<ExportCheck::Issue>> stutter(const ProjectFormat& f, const Timeline& tl, int from, int to,
                                                   const std::function<int(const QString& path)>& fileLength,
                                                   const Progress& progress)
{
    QStringList files;
    for (const Track& t : tl.video) {
        if (t.hidden) continue;
        for (const Clip& c : t.clips)
            if (c.enabled && c.kind == ClipKind::Media && !c.freeze && !c.mediaPath.isEmpty() && c.end() > from
                && c.start < to && !files.contains(c.mediaPath))
                files << c.mediaPath;
    }
    // Each file once (cached after the first time); the judgement looks them up
    QHash<QString, std::optional<Stutter::Scan>> scans;
    for (int i = 0; i < files.size(); ++i) {
        if (progress && !progress(double(i) / files.size())) return std::nullopt;
        scans.insert(files[i], FrameGaps::scan(files[i]));
    }
    if (progress) progress(1.0);
    return ExportCheck::stutter(tl, from, to, f, [&](const QString& p) { return scans.value(p); }, fileLength);
}

} // namespace CheckMeasure
