// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli: reading the picture of a video for the analyses, in-process (engine/video/Scan) or, for comparison,
// with an ffmpeg process as before 0.15 (SCHNEIDI_ANALYSIS=ffmpeg)
#include "cli/AnalysisDetail.h"

#include "engine/Bundle.h"
#include "engine/video/Scan.h"

#include <QProcess>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace Cli::detail {

namespace {

constexpr double kNoScore = -1;

// The mean absolute difference of two pictures' brightness (0..255): ffmpeg's scene score looks at the luma plane only
double meanDifference(const Video::Scan::Frame& a, const Video::Scan::Frame& b)
{
    uint64_t sad = 0;
    for (int y = 0; y < a.height; ++y) {
        const uint8_t* x = a.data[0] + size_t(y) * size_t(a.linesize[0]);
        const uint8_t* z = b.data[0] + size_t(y) * size_t(b.linesize[0]);
        uint32_t row = 0;
        for (int i = 0; i < a.width; ++i) row += uint32_t(std::abs(int(x[i]) - int(z[i])));
        sad += row;
    }
    return a.width > 0 && a.height > 0 ? double(sad) / (double(a.width) * a.height) : 0.0;
}

double meanLuma(const Video::Scan::Frame& f)
{
    uint64_t sum = 0;
    for (int y = 0; y < f.height; ++y) {
        const uint8_t* row = f.data[0] + size_t(y) * size_t(f.linesize[0]);
        uint32_t s = 0;
        for (int x = 0; x < f.width; ++x) s += row[x];
        sum += s;
    }
    return f.width > 0 && f.height > 0 ? double(sum) / (double(f.width) * f.height) : 0.0;
}

std::vector<PictureScore> scoresFfmpeg(const MediaSource& m, int width, const std::function<void(double)>& progress)
{
    QProcess ff;
    ff.setProgram(Bundle::tool("ffmpeg"));
    ff.setArguments({"-hide_banner", "-nostdin", "-nostats", "-loglevel", "info", "-i", m.info.path, "-map", "0:v:0",
                     "-an", "-sn", "-vf",
                     QString("setpts=PTS-STARTPTS,scale=%1:-2,signalstats,metadata=mode=print:key=lavfi.signalstats.YAVG,"
                             "select='gte(scene\\,0)',metadata=mode=print:key=lavfi.scene_score")
                         .arg(width),
                     "-f", "null", "-", "-progress", "pipe:1"});
    ff.start();
    if (!ff.waitForStarted()) fail("TOOL_MISSING", "ffmpeg not found (needed to look at the picture)");
    const double fps = m.format.rate.fps(), total = m.info.length / fps;
    static const QRegularExpression timeRe("pts_time:([0-9.]+)"), lumaRe("signalstats\\.YAVG=([0-9.]+)"),
        scoreRe("scene_score=([0-9.]+)");
    std::vector<PictureScore> out;
    QByteArray pending;
    const auto parse = [&](const QByteArray& chunk) {
        pending += chunk;
        const qsizetype end = pending.lastIndexOf('\n');
        if (end < 0) return;
        for (const QByteArray& raw : pending.left(end).split('\n')) {
            const QString line = QString::fromUtf8(raw);
            if (const auto t = timeRe.match(line); t.hasMatch()) {
                // Both metadata filters print the frame's time: one entry per frame
                const double at = t.captured(1).toDouble();
                if (out.empty() || out.back().seconds != at) out.push_back({at, 0.0, -1.0});
            }
            else if (const auto l = lumaRe.match(line); l.hasMatch() && !out.empty())
                out.back().luma = l.captured(1).toDouble();
            else if (const auto s = scoreRe.match(line); s.hasMatch() && !out.empty())
                out.back().score = s.captured(1).toDouble();
        }
        pending.remove(0, end + 1);
    };
    while (ff.state() != QProcess::NotRunning) {
        ff.waitForReadyRead(200);
        parse(ff.readAllStandardError());
        for (const QByteArray& line : ff.readAllStandardOutput().split('\n'))
            if (line.startsWith("out_time_us=") && progress && total > 0)
                progress(std::clamp(line.mid(12).toDouble() / 1e6 / total, 0.0, 1.0));
    }
    parse(ff.readAllStandardError() + '\n');
    if (ff.exitStatus() != QProcess::NormalExit || ff.exitCode() != 0)
        fail("DECODE_FAILED", "ffmpeg could not read the video of " + m.info.path);
    return out;
}

} // namespace

bool ffmpegAnalysis()
{
    return qEnvironmentVariable("SCHNEIDI_ANALYSIS") == QLatin1String("ffmpeg");
}

std::vector<PictureScore> pictureScores(const MediaSource& m, int width, const std::function<void(double)>& progress)
{
    if (ffmpegAnalysis()) return scoresFfmpeg(m, width, progress);
    Video::Scan::Options o;
    o.width = width;
    o.height = -2;
    o.format = AV_PIX_FMT_YUV420P;
    std::mutex mutex;
    std::vector<PictureScore> out; // score = the mean difference to the picture before here
    const Video::Scan::Result r = Video::Scan::run(
        m.info.path, o,
        [&](const Video::Scan::Frame& f, const Video::Scan::Frame* previous) {
            const PictureScore s{f.seconds, previous ? meanDifference(f, *previous) : kNoScore, meanLuma(f)};
            std::lock_guard lock(mutex);
            out.push_back(s);
        },
        [&](double p) {
            if (progress) progress(p);
            return true;
        });
    if (!r.ok) fail("DECODE_FAILED", "could not read the video of " + m.info.path + ": " + r.error);
    std::sort(out.begin(), out.end(), [](const PictureScore& a, const PictureScore& b) { return a.seconds < b.seconds; });
    // Scene score as ffmpeg's select filter defines it: the smaller of the difference to the picture before and its
    // change against the difference before (a pan changes every picture alike: not a cut), in 0..1
    double before = 0;
    for (PictureScore& s : out) {
        if (s.score == kNoScore) {
            s.score = 0;
            continue;
        }
        const double mad = s.score;
        s.score = std::clamp(std::min(mad, std::abs(mad - before)) / 100.0, 0.0, 1.0);
        before = mad;
    }
    return out;
}

void pictureSamples(const MediaSource& m, const QRectF& crop, QSize size, int rate, const QString& ffmpegFilter,
                    const std::function<void(int, const uchar*)>& each, const std::function<void(double)>& progress)
{
    const ProjectFormat& f = m.format;
    const double total = m.info.length / f.rate.fps() * rate;
    const qsizetype frameBytes = qsizetype(size.width()) * size.height() * 3;
    if (ffmpegAnalysis()) {
        QProcess ff;
        ff.setProgram(Bundle::tool("ffmpeg"));
        ff.setArguments({"-hide_banner", "-nostdin", "-nostats", "-loglevel", "error", "-i", m.info.path, "-map", "0:v:0",
                         "-an", "-sn", "-vf", QString("setpts=PTS-STARTPTS,fps=%1,%2,format=rgb24").arg(rate).arg(ffmpegFilter),
                         "-f", "rawvideo", "-"});
        ff.start();
        if (!ff.waitForStarted()) fail("TOOL_MISSING", "ffmpeg not found (needed for region events)");
        QByteArray raw;
        int sample = 0;
        const auto consume = [&] {
            qsizetype at = 0;
            for (; raw.size() - at >= frameBytes; at += frameBytes)
                each(sample++, reinterpret_cast<const uchar*>(raw.constData()) + at);
            raw.remove(0, at);
        };
        while (ff.state() != QProcess::NotRunning) {
            ff.waitForReadyRead(200);
            raw += ff.readAllStandardOutput();
            consume();
            if (progress && total > 0) progress(std::clamp(sample / total, 0.0, 1.0));
        }
        raw += ff.readAllStandardOutput();
        consume();
        if (ff.exitStatus() != QProcess::NormalExit || ff.exitCode() != 0)
            fail("DECODE_FAILED",
                 "ffmpeg could not read the video: " + QString::fromUtf8(ff.readAllStandardError().right(400)).trimmed());
        return;
    }
    Video::Scan::Options o;
    o.width = size.width();
    o.height = size.height();
    o.crop = crop;
    o.format = AV_PIX_FMT_RGB24;
    o.rate = rate;
    // Samples a fraction of a frame earlier than with every picture decoded: same events, at most a frame or two
    // apart (SCHNEIDI_ANALYSIS=full decodes all and matches the ffmpeg way exactly)
    o.skipNonReference = qEnvironmentVariable("SCHNEIDI_ANALYSIS") != QLatin1String("full");
    const Video::Scan::Result r = Video::Scan::run(
        m.info.path, o,
        [&](const Video::Scan::Frame& fr, const Video::Scan::Frame*) {
            const int row = fr.width * 3;
            if (fr.linesize[0] == row) {
                each(int(fr.sample), fr.data[0]);
                return;
            }
            std::vector<uchar> packed(static_cast<size_t>(frameBytes));
            for (int y = 0; y < fr.height; ++y)
                std::memcpy(packed.data() + size_t(y) * size_t(row), fr.data[0] + size_t(y) * size_t(fr.linesize[0]),
                            size_t(row));
            each(int(fr.sample), packed.data());
        },
        [&](double p) {
            if (progress) progress(p);
            return true;
        });
    if (!r.ok) fail("DECODE_FAILED", "could not read the video of " + m.info.path + ": " + r.error);
}

} // namespace Cli::detail
