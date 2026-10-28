// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli: looking at the picture of the material (scenes, region events, frames, scopes)
#include "cli/AnalysisDetail.h"

#include "core/ContactSheet.h"
#include "core/TimelineOps.h"
#include "engine/Bundle.h"
#include "engine/Snapshot.h"
#include "ui/Scopes.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace Cli::detail {

namespace {
// The frames of one source (media file or project timeline) for `frames`
struct FrameSet {
    QString source;
    QVector<QImage> images;
    QStringList labels;
    QJsonArray frames;
};

FrameSet framesOf(const QString& src, const QJsonObject& a, Context& ctx, int maxEdge, const QString& labelPrefix)
{
    std::unique_ptr<Session> session;
    MediaSource media;
    ProjectFormat f;
    int length = 0;
    if (isProjectFile(src)) {
        session = std::make_unique<Session>(src);
        session->selectTimeline(a["timeline"]);
        f = session->format();
        length = TimelineOps::endFrame(session->project.timeline());
    } else {
        QJsonObject ma{{"file", src}};
        if (a.contains("project")) ma["project"] = a["project"];
        media = openMedia(ma);
        f = pictureFormat(media);
        length = media.info.length;
    }
    if (length <= 0) {
        if (session) session->failEmpty();
        fail("EMPTY", "nothing to show: " + src);
    }
    QVector<int> at;
    for (const QJsonValue& v : a["at"].toArray()) at << parseTime(v, f, "at");
    const int from = a.contains("from") ? std::clamp(parseTime(a["from"], f, "from"), 0, length - 1) : 0;
    const int to = a.contains("to") ? std::clamp(parseTime(a["to"], f, "to"), from + 1, length) : length;
    if (a.contains("every")) {
        const int step = parseTime(a["every"], f, "every");
        if (step <= 0) fail("BAD_ARGUMENT", "'every' must be > 0");
        for (int fr = from; fr < to; fr += step) at << fr;
    }
    if (a.contains("count")) {
        const int n = std::clamp(a["count"].toInt(), 1, 100);
        at += ContactSheet::spread(from, to, n);
    }
    if (at.isEmpty()) fail("BAD_ARGUMENT", "give 'at' (times), 'every' (interval) or 'count' (evenly spread)");
    if (at.size() > 100) fail("BAD_ARGUMENT", QString("too many frames (%1, max 100)").arg(at.size()));
    // Crop: decode the full picture, cut the region out, scale it down only if it is larger than maxEdge
    const bool crop = a.contains("crop");
    const QSize full = f.size();
    const QRect region = crop ? cropRect(a["crop"].toString(), full) : QRect();
    const int decodeEdge = crop ? std::max(full.width(), full.height()) : maxEdge;

    const auto snap = session ? Snapshot::Source::timeline(f, session->project.renderTimeline())
                              : Snapshot::Source::media(f, media.info.path);
    FrameSet out;
    out.source = absolute(src);
    for (int i = 0; i < at.size(); ++i) {
        const int fr = std::clamp(at[i], 0, length - 1);
        QImage img = snap->frame(fr, decodeEdge);
        if (img.isNull()) fail("DECODE_FAILED", QString("could not decode frame %1 of %2").arg(fr).arg(src));
        if (crop) {
            // The snapshot may come out smaller than the format (odd sizes): scale the region along
            const double kx = double(img.width()) / full.width(), ky = double(img.height()) / full.height();
            img = img.copy(QRect(int(region.x() * kx), int(region.y() * ky), int(region.width() * kx), int(region.height() * ky)));
            if (std::max(img.width(), img.height()) > maxEdge)
                img = img.scaled(maxEdge, maxEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        out.images << img;
        out.labels << labelPrefix + tc(fr, f);
        out.frames << QJsonObject{{"frame", fr}, {"tc", tc(fr, f)}, {"s", seconds(fr, f)}};
        if (ctx.progress) ctx.progress(double(i + 1) / at.size());
    }
    return out;
}

// Events in one region of the picture (kill feed, score, HUD): moments where its content changes and then stays.
// The game behind such overlays moves all the time, so a plain frame difference fires constantly; a pixel only
// counts when it was steady just before, differs afterwards and is steady again (a new line that appeared).
QJsonObject regionChanges(const MediaSource& m, const QJsonObject& a, Context& ctx)
{
    const ProjectFormat f = pictureFormat(m);
    const QSize full(std::max(2, f.width), std::max(2, f.height));
    const QRect r = cropRect(a["region"].toString(), full);
    constexpr int kRate = 10; // samples per second
    const int w = 128, h = std::clamp(int(std::lround(128.0 * r.height() / r.width())) & ~1, 8, 256);
    QProcess ff;
    ff.setProgram(Bundle::tool("ffmpeg"));
    ff.setArguments({"-hide_banner", "-nostdin", "-nostats", "-loglevel", "error", "-i", m.info.path, "-map", "0:v:0",
                     "-an", "-sn", "-vf",
                     QString("setpts=PTS-STARTPTS,fps=%1,crop=w=iw*%2:h=ih*%3:x=iw*%4:y=ih*%5,scale=%6:%7,format=rgb24")
                         .arg(kRate)
                         .arg(QString::number(double(r.width()) / full.width(), 'f', 5),
                              QString::number(double(r.height()) / full.height(), 'f', 5),
                              QString::number(double(r.x()) / full.width(), 'f', 5),
                              QString::number(double(r.y()) / full.height(), 'f', 5))
                         .arg(w)
                         .arg(h),
                     "-f", "rawvideo", "-"});
    ff.start();
    if (!ff.waitForStarted()) fail("TOOL_MISSING", "ffmpeg not found (needed for region events)");
    const qsizetype frameBytes = qsizetype(w) * h * 3;
    const double total = m.info.length / f.rate.fps() * kRate;
    QByteArray raw;
    while (ff.state() != QProcess::NotRunning) {
        ff.waitForReadyRead(200);
        raw += ff.readAllStandardOutput();
        if (ctx.progress && total > 0) ctx.progress(std::clamp(raw.size() / frameBytes / total, 0.0, 1.0));
    }
    raw += ff.readAllStandardOutput();
    if (ff.exitStatus() != QProcess::NormalExit || ff.exitCode() != 0)
        fail("DECODE_FAILED", "ffmpeg could not read the video: " + QString::fromUtf8(ff.readAllStandardError().right(400)).trimmed());
    const int n = int(raw.size() / frameBytes);
    const auto px = [&](int frame, int i) { return reinterpret_cast<const uchar*>(raw.constData()) + frame * frameBytes + i * 3; };
    const auto diff = [](const uchar* x, const uchar* y) {
        return std::max({std::abs(x[0] - y[0]), std::abs(x[1] - y[1]), std::abs(x[2] - y[2])});
    };
    constexpr int kSteady = 12, kChanged = 40;
    const double sensitivity = a.contains("threshold") ? a["threshold"].toDouble() : 0.3;
    if (sensitivity <= 0 || sensitivity >= 1) fail("BAD_ARGUMENT", "threshold must be between 0 and 1 (default 0.3)");
    const double minShare = 0.01 + sensitivity * 0.1; // share of the region that must change (default 4 %)
    // score(t): share of pixels steady over t-2..t-1, changed from t-1 to t+2 (overlays slide in for a few
    // frames), steady over t+2..t+3
    std::vector<double> score(size_t(std::max(n, 0)), 0.0);
    for (int t = 2; t + 3 < n; ++t) {
        int hits = 0;
        for (int i = 0; i < w * h; ++i)
            if (diff(px(t - 2, i), px(t - 1, i)) < kSteady && diff(px(t + 2, i), px(t + 3, i)) < kSteady
                && diff(px(t - 1, i), px(t + 2, i)) > kChanged)
                ++hits;
        score[size_t(t)] = double(hits) / (w * h);
    }
    const int minGap = std::max(1, int(std::lround((a.contains("min") ? parseTime(a["min"], f, "min") / f.rate.fps() : 0.5) * kRate)));
    QJsonArray changes;
    int last = -minGap;
    for (int t = 2; t + 3 < n; ++t) {
        if (score[size_t(t)] < minShare) continue;
        // the strongest sample of a burst
        bool peak = true;
        for (int k = std::max(0, t - minGap + 1); k < std::min(n, t + minGap) && peak; ++k)
            if (score[size_t(k)] > score[size_t(t)]) peak = false;
        if (!peak || t - last < minGap) continue;
        last = t;
        const int frame = std::min(m.info.length - 1, int(std::lround(double(t) / kRate * f.rate.fps())));
        changes << QJsonObject{{"frame", frame}, {"s", seconds(frame, f)}, {"tc", tc(frame, f)},
                               {"changed_pct", std::round(score[size_t(t)] * 1000) / 10}};
    }
    return {{"file", m.info.path}, {"frames", m.info.length}, {"fps", formatJson(f)["fps"]}, {"region", a["region"]},
            {"threshold", sensitivity}, {"changes", changes},
            {"note", "changes = the region's content changed and stayed (a line appeared or went away); check them "
                     "with `frames` crop"}};
}

// Rows with the colour as lines: runs of rows that have some (one empty row inside is a broken outline, not a gap
// between two lines); runs with fewer than `speck` pixels are specks of the background, not a line
std::vector<std::pair<int, int>> colorLines(const std::vector<int>& rows, double speck)
{
    std::vector<std::pair<int, int>> out;
    const int h = int(rows.size());
    for (int y = 0; y < h;) {
        if (rows[size_t(y)] == 0) {
            ++y;
            continue;
        }
        const int first = y;
        int last = y, sum = 0;
        while (y < h && (rows[size_t(y)] > 0 || (y + 1 < h && rows[size_t(y + 1)] > 0))) {
            if (rows[size_t(y)] > 0) {
                last = y;
                sum += rows[size_t(y)];
            }
            ++y;
        }
        if (sum >= speck) out.emplace_back(first, last);
    }
    return out;
}

// Pixels of the colour in `to` (pixels per row of one sample) that the lines of `from` (the other side of a moment)
// do not explain. The same line explains its rows and `margin` rows around them: semi-transparent kill feed lines
// (CS2) match the colour more or less depending on the game behind them, so an outline edge comes and goes. Or all
// lines moved up or down together (an old one went away) and brightened while doing so. What is left is colour in
// a new place: a new line.
int unexplainedColor(const std::vector<int>& from, const int* to, int margin, double speck)
{
    const int h = int(from.size());
    std::vector<char> same(size_t(h), 0), moved(size_t(h), 0);
    int tallest = 0;
    for (const auto& [first, last] : colorLines(from, speck)) {
        tallest = std::max(tallest, last - first + 1);
        for (int y = std::max(0, first - margin); y <= std::min(h - 1, last + margin); ++y) same[size_t(y)] = 1;
        for (int y = std::max(0, first - 1); y <= std::min(h - 1, last + 1); ++y) moved[size_t(y)] = 1;
    }
    int total = 0, explained = 0;
    for (int y = 0; y < h; ++y) {
        total += to[y];
        if (same[size_t(y)]) explained += to[y];
    }
    // Lines move by a whole line (at least their height): a smaller shift would only blur "the same place"
    if (tallest > 0)
        for (int d = std::max(tallest, 2 * margin + 1); d < h; ++d)
            for (const int shift : {-d, d}) {
                int e = 0;
                for (int y = std::max(0, shift); y < std::min(h, h + shift); ++y)
                    if (moved[size_t(y - shift)]) e += to[y];
                explained = std::max(explained, e);
            }
    return total - explained;
}

} // namespace

// Events of one colour in a region (the yellow mark of the player's own kill feed lines): how much of the picture
// has that colour, and moments where it rises (something appeared) or falls and stays. Counting the colour instead
// of comparing pixels ignores lines moving up when an old one goes away; a rise only counts when the new colour is
// in rows it was not in just before (a new line, not the same line on a brighter background).
ColorEvents colorEvents(const MediaSource& m, const QJsonObject& a, const std::function<void(double)>& progress)
{
    const ProjectFormat f = pictureFormat(m);
    const QSize full(std::max(2, f.width), std::max(2, f.height));
    const QRect r = cropRect(a["region"].toString(), full);
    const QColor color = colorOf(a["color"], "color");
    const int tolerance = a.contains("tolerance") ? std::clamp(a["tolerance"].toInt(), 1, 255) : 25;
    const QString only = a.contains("only") ? a["only"].toString() : "appear";
    if (only != "appear" && only != "disappear" && only != "both")
        fail("BAD_ARGUMENT", "only must be appear (default), disappear or both");
    const bool appears = only != "disappear", disappears = only != "appear";
    // Share of the whole picture, so it does not depend on the region's size (default ~200 pixels in 1080p)
    const double minArea = a.contains("min_area") ? a["min_area"].toDouble() : 0.0001;
    if (minArea <= 0 || minArea >= 1) fail("BAD_ARGUMENT", "min_area must be between 0 and 1 (default 0.0001)");
    constexpr int kRate = 15; // samples per second
    // Full resolution of the region: small marks (a 2 px frame) blur into the background when scaled down
    const int w = std::max(2, r.width() & ~1), h = std::max(2, r.height() & ~1);
    QProcess ff;
    ff.setProgram(Bundle::tool("ffmpeg"));
    ff.setArguments({"-hide_banner", "-nostdin", "-nostats", "-loglevel", "error", "-i", m.info.path, "-map", "0:v:0",
                     "-an", "-sn", "-vf",
                     QString("setpts=PTS-STARTPTS,fps=%1,scale=%2:%3,crop=%4:%5:%6:%7,format=rgb24")
                         .arg(kRate)
                         .arg(full.width())
                         .arg(full.height())
                         .arg(w)
                         .arg(h)
                         .arg(r.x())
                         .arg(r.y()),
                     "-f", "rawvideo", "-"});
    ff.start();
    if (!ff.waitForStarted()) fail("TOOL_MISSING", "ffmpeg not found (needed for region events)");
    const qsizetype frameBytes = qsizetype(w) * h * 3;
    const double total = m.info.length / f.rate.fps() * kRate;
    const double picture = double(full.width()) * full.height();
    const int cr = color.red(), cg = color.green(), cb = color.blue();
    std::vector<double> share; // per sample: part of the picture in the colour (inside the region)
    // rise(t): how much more of the colour there is from t on than just before (lines slide in for a few
    // samples), and it stays; fall(t) the other way round
    const auto lo = [&](int from, int to) {
        double v = 1;
        for (int k = std::max(0, from); k <= std::min(int(share.size()) - 1, to); ++k) v = std::min(v, share[size_t(k)]);
        return v;
    };
    const auto hi = [&](int from, int to) {
        double v = 0;
        for (int k = std::max(0, from); k <= std::min(int(share.size()) - 1, to); ++k) v = std::max(v, share[size_t(k)]);
        return v;
    };
    std::vector<double> rise, fall;
    // Where the colour is: pixels per row of the last kWindow samples (t-3 … t+4 around a moment t)
    constexpr int kWindow = 8;
    std::vector<int> rows(size_t(kWindow) * h, 0);
    const auto rowsOf = [&](int sample) { return rows.data() + size_t(sample % kWindow) * h; };
    const int margin = std::max(2, int(std::lround(4.0 * full.height() / 1080))); // an outline edge (1-2 px) and more
    const double speck = minArea * picture / 2;
    // The colour that arrived (left) at t: what the samples after (before) it have outside the rows of the other side
    const auto changed = [&](int t, bool appear) {
        std::vector<int> other(size_t(h), 0);
        for (int k = appear ? t - 3 : t + 2; k <= (appear ? t - 1 : t + 4); ++k)
            for (int y = 0; y < h; ++y) other[size_t(y)] = std::max(other[size_t(y)], rowsOf(k)[y]);
        int least = std::numeric_limits<int>::max();
        for (int k = appear ? t + 2 : t - 3; k <= (appear ? t + 4 : t - 1); ++k)
            least = std::min(least, unexplainedColor(other, rowsOf(k), margin, speck));
        return least;
    };
    QByteArray raw;
    // Decoded samples are counted as they come (a long file would not fit in memory)
    const auto consume = [&] {
        qsizetype at = 0;
        for (; raw.size() - at >= frameBytes; at += frameBytes) {
            const uchar* p = reinterpret_cast<const uchar*>(raw.constData()) + at;
            int* row = rowsOf(int(share.size()));
            int hits = 0;
            for (int y = 0; y < h; ++y) {
                int count = 0;
                for (int x = 0; x < w; ++x, p += 3)
                    count += std::abs(p[0] - cr) <= tolerance && std::abs(p[1] - cg) <= tolerance && std::abs(p[2] - cb) <= tolerance;
                row[y] = count;
                hits += count;
            }
            share.push_back(hits / picture);
            // The window around t = this sample - 4 is complete: a step that counts needs colour in a new place
            rise.push_back(0.0);
            fall.push_back(0.0);
            if (const int t = int(share.size()) - 5; t >= 3) {
                const double up = lo(t + 2, t + 4) - hi(t - 3, t - 1), down = lo(t - 3, t - 1) - hi(t + 2, t + 4);
                if (appears && up >= minArea && changed(t, true) >= speck) rise[size_t(t)] = up;
                if (disappears && down >= minArea && changed(t, false) >= speck) fall[size_t(t)] = down;
            }
        }
        raw.remove(0, at);
    };
    while (ff.state() != QProcess::NotRunning) {
        ff.waitForReadyRead(200);
        raw += ff.readAllStandardOutput();
        consume();
        if (progress && total > 0) progress(std::clamp(share.size() / total, 0.0, 1.0));
    }
    raw += ff.readAllStandardOutput();
    consume();
    if (ff.exitStatus() != QProcess::NormalExit || ff.exitCode() != 0)
        fail("DECODE_FAILED", "ffmpeg could not read the video: " + QString::fromUtf8(ff.readAllStandardError().right(400)).trimmed());

    const int n = int(share.size());
    const int minGap = std::max(1, int(std::lround((a.contains("min") ? parseTime(a["min"], f, "min") / f.rate.fps() : 0.5) * kRate)));
    struct Event {
        int sample;
        bool appear;
        double area;
        int onset;
    };
    QVector<Event> events;
    for (const bool appear : {true, false}) {
        if (appear ? !appears : !disappears) continue;
        const std::vector<double>& score = appear ? rise : fall;
        int last = -minGap;
        for (int t = 0; t < n; ++t) {
            if (score[size_t(t)] < minArea) continue;
            bool peak = true; // the strongest sample of a burst
            for (int k = std::max(0, t - minGap + 1); k < std::min(n, t + minGap) && peak; ++k)
                if (score[size_t(k)] > score[size_t(t)]) peak = false;
            if (!peak || t - last < minGap) continue;
            const double before = appear ? hi(t - 3, t - 1) : lo(t - 3, t - 1);
            // A flash: back to where it was within 0.3 s
            bool flash = false;
            for (int k = t + 2; k <= std::min(n - 1, t + kRate * 3 / 10 + 1) && !flash; ++k)
                flash = appear ? share[size_t(k)] <= before + score[size_t(t)] / 4 : share[size_t(k)] >= before - score[size_t(t)] / 4;
            if (flash) continue;
            last = t;
            // The moment itself: the first sample past half of the step
            int at = t;
            for (int k = t - 1; k <= std::min(n - 1, t + 4); ++k)
                if (appear ? share[size_t(k)] >= before + score[size_t(t)] / 2 : share[size_t(k)] <= before - score[size_t(t)] / 2) {
                    at = k;
                    break;
                }
            // Where it began: back while the colour is already clearly on its way (a tenth of the step)
            int onset = at;
            while (onset > 0 && at - onset < kRate
                   && (appear ? share[size_t(onset - 1)] > before + score[size_t(t)] / 10
                              : share[size_t(onset - 1)] < before - score[size_t(t)] / 10))
                --onset;
            events << Event{at, appear, score[size_t(t)], onset};
        }
    }
    std::sort(events.begin(), events.end(), [](const Event& x, const Event& y) { return x.sample < y.sample; });
    ColorEvents out;
    out.color = color;
    out.tolerance = tolerance;
    out.minArea = minArea;
    out.most = share.empty() ? 0.0 : *std::max_element(share.begin(), share.end());
    const auto frameOf = [&](int sample) {
        return std::min(m.info.length - 1, int(std::lround(double(sample) / kRate * f.rate.fps())));
    };
    for (const Event& e : events) out.events << ColorEvent{frameOf(e.sample), e.appear, e.area, frameOf(e.onset)};
    return out;
}

namespace {
// scenes with region + color
QJsonObject regionColorEvents(const MediaSource& m, const QJsonObject& a, Context& ctx)
{
    const ProjectFormat& f = m.format;
    const ColorEvents ce = colorEvents(m, a, [&ctx](double p) {
        if (ctx.progress) ctx.progress(p);
    });
    QJsonArray changes;
    for (const ColorEvent& e : ce.events)
        changes << QJsonObject{{"frame", e.frame}, {"s", seconds(e.frame, f)}, {"tc", tc(e.frame, f)},
                               {"kind", e.appear ? "appear" : "disappear"}, {"area_pct", std::round(e.area * 1e5) / 1e3}};
    return {{"file", m.info.path}, {"frames", m.info.length}, {"fps", formatJson(f)["fps"]}, {"region", a["region"]},
            {"color", ce.color.name()}, {"tolerance", ce.tolerance}, {"min_area", ce.minArea},
            {"most_pct", std::round(ce.most * 1e5) / 1e3}, {"changes", changes},
            {"note", "changes = more (appear) or less (disappear) of the colour in the region, and it stayed; "
                     "area_pct = how much of the whole picture changed; most_pct = the most of the colour at one time "
                     "(0 = the colour is not there: check it with frames crop)"}};
}

QJsonObject scenesOne(const QJsonObject& a, Context& ctx)
{
    const MediaSource m = openMedia(a);
    if (!m.info.hasVideo || m.info.isImage) fail("NO_VIDEO", "file has no moving picture: " + m.info.path);
    if (a.contains("color") && !a.contains("region")) fail("BAD_ARGUMENT", "color needs a region");
    if (a.contains("color")) return regionColorEvents(m, a, ctx);
    if (a.contains("only") || a.contains("min_area") || a.contains("tolerance"))
        fail("BAD_ARGUMENT", "only, min_area and tolerance need a region and a color");
    if (a.contains("region")) return regionChanges(m, a, ctx);
    const ProjectFormat& f = m.format;
    const double threshold = a.contains("threshold") ? a["threshold"].toDouble() : 0.3;
    if (threshold <= 0 || threshold >= 1) fail("BAD_ARGUMENT", "threshold must be between 0 and 1 (default 0.3)");
    const int minLen = a.contains("min") ? parseTime(a["min"], f, "min") : int(std::lround(0.5 * f.rate.fps()));

    // ffmpeg's scene score (0..1, difference to the previous frame) on a small copy of each frame; metadata=print
    // logs time and score of every frame above the threshold. Times start at 0 like the frames in schneidi.
    QProcess ff;
    ff.setProgram(Bundle::tool("ffmpeg"));
    ff.setArguments({"-hide_banner", "-nostdin", "-nostats", "-loglevel", "info", "-i", m.info.path, "-map", "0:v:0",
                     "-an", "-sn", "-vf",
                     QString("setpts=PTS-STARTPTS,scale=320:-2,select='gt(scene,%1)',metadata=mode=print").arg(threshold),
                     "-f", "null", "-", "-progress", "pipe:1"});
    ff.start();
    if (!ff.waitForStarted()) fail("TOOL_MISSING", "ffmpeg not found (needed for scene detection)");
    const double total = m.info.length / f.rate.fps();
    QByteArray log;
    while (ff.state() != QProcess::NotRunning) {
        ff.waitForReadyRead(200);
        log += ff.readAllStandardError();
        const QList<QByteArray> lines = ff.readAllStandardOutput().split('\n');
        for (const QByteArray& line : lines)
            if (line.startsWith("out_time_us=") && ctx.progress && total > 0)
                ctx.progress(std::clamp(line.mid(12).toDouble() / 1e6 / total, 0.0, 1.0));
    }
    log += ff.readAllStandardError();
    if (ff.exitStatus() != QProcess::NormalExit || ff.exitCode() != 0)
        fail("DECODE_FAILED", "ffmpeg could not read the video: " + QString::fromUtf8(log.right(400)).trimmed());

    // "frame:12 pts:12 pts_time:0.48" followed by "lavfi.scene_score=0.62"
    static const QRegularExpression timeRe("pts_time:([0-9.]+)"), scoreRe("lavfi\\.scene_score=([0-9.]+)");
    QVector<QPair<int, double>> found;
    for (const QString& line : QString::fromUtf8(log).split('\n')) {
        if (const auto t = timeRe.match(line); t.hasMatch())
            found.append({int(std::lround(t.captured(1).toDouble() * f.rate.fps())), 0.0});
        else if (const auto sc = scoreRe.match(line); sc.hasMatch() && !found.isEmpty())
            found.last().second = sc.captured(1).toDouble();
    }
    // Cuts closer than minLen to the previous one (flashes, fast pans) are dropped; no cut at the very start
    QJsonArray cuts, scenes;
    int sceneStart = 0;
    for (const auto& [frame, score] : found) {
        if (frame <= 0 || frame >= m.info.length || frame - sceneStart < minLen) continue;
        cuts << QJsonObject{{"frame", frame}, {"s", seconds(frame, f)}, {"tc", tc(frame, f)},
                            {"score", std::round(score * 1000) / 1000}};
        scenes << range(sceneStart, frame, f);
        sceneStart = frame;
    }
    scenes << range(sceneStart, m.info.length, f);
    return {{"file", m.info.path}, {"frames", m.info.length}, {"fps", formatJson(f)["fps"]}, {"threshold", threshold},
            {"cuts", cuts}, {"scenes", scenes}};
}
} // namespace

QJsonObject cmdScenes(const QJsonObject& a, Context& ctx) { return perFile(a, ctx, scenesOne, 2); }

QJsonObject cmdFrames(const QJsonObject& a, Context& ctx)
{
    // Every source with its arguments: an object entry in 'sources' overrides region and times for that file
    struct Source {
        QString path;
        QJsonObject args;
    };
    QVector<Source> sources;
    if (a.contains("source")) sources << Source{a["source"].toString(), a};
    for (const QJsonValue& v : a["sources"].toArray()) {
        if (!v.isObject()) {
            sources << Source{v.toString(), a};
            continue;
        }
        QJsonObject args = a;
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            static const QStringList own{"file", "crop", "at", "every", "count", "from", "to", "timeline"};
            if (!own.contains(it.key()))
                fail("BAD_ARGUMENT", QString("sources: unknown key '%1' (allowed: %2)").arg(it.key(), own.join(", ")));
            // the source's own times replace the general ones completely
            if (it.key() == "at" || it.key() == "every" || it.key() == "count")
                for (const char* k : {"at", "every", "count"}) args.remove(k);
        }
        for (auto it = o.begin(); it != o.end(); ++it)
            if (it.key() != "file") args[it.key()] = it.value();
        sources << Source{o["file"].toString(), args};
    }
    if (sources.isEmpty()) fail("BAD_ARGUMENT", "give 'source' (or 'sources', several files)");
    // tile_width: decode large enough for portrait pictures too, then scale every image to exactly that width
    const int tileWidth = a.contains("tile_width") ? std::clamp(a["tile_width"].toInt(), 32, 1920) : 0;
    const int maxEdge = tileWidth ? std::min(3840, tileWidth * 2)
                        : a.contains("width") ? std::clamp(a["width"].toInt(), 64, 3840)
                                              : (ctx.mcp ? 480 : 640);
    const QString outDir = a["out"].toString();
    const bool asSheet = a["sheet"].toBool();
    if (outDir.isEmpty() && !ctx.mcp) fail("BAD_ARGUMENT", "missing 'out' (folder for the images, or the sheet file with --sheet)");

    QVector<QImage> images;
    QStringList labels;
    QJsonArray perSource;
    for (int si = 0; si < sources.size(); ++si) {
        Context one;
        one.mcp = ctx.mcp;
        if (ctx.progress) one.progress = [&ctx, si, n = sources.size()](double p) { ctx.progress((si + p) / n); };
        // Several sources on one sheet: label with the source's number (the answer lists them in order)
        FrameSet set = framesOf(sources[si].path, sources[si].args, one, maxEdge,
                                sources.size() > 1 ? QString("%1  ").arg(si + 1) : QString());
        if (tileWidth)
            for (QImage& img : set.images)
                if (img.width() != tileWidth) img = img.scaledToWidth(tileWidth, Qt::SmoothTransformation);
        for (int i = 0; i < set.images.size(); ++i) {
            if (!asSheet && !outDir.isEmpty()) {
                QDir().mkpath(outDir);
                const QString prefix = sources.size() > 1 ? QString("src%1_").arg(si + 1) : QString();
                const QString file = QDir(outDir).absoluteFilePath(
                    QString("%1frame_%2.jpg").arg(prefix).arg(set.frames[i].toObject()["frame"].toInt(), 6, 10, QChar('0')));
                if (!set.images[i].save(file, "JPG", 85)) fail("WRITE_FAILED", "cannot write " + file);
                QJsonObject o = set.frames[i].toObject();
                o["file"] = file;
                set.frames[i] = o;
            }
        }
        images += set.images;
        labels += set.labels;
        if (images.size() > (asSheet ? 100 : 200))
            fail("BAD_ARGUMENT", QString("too many frames in total (max %1)").arg(asSheet ? 100 : 200));
        perSource << QJsonObject{{"n", si + 1}, {"source", set.source}, {"frames", set.frames}};
    }
    QJsonObject result;
    if (sources.size() == 1) result = {{"source", perSource[0].toObject()["source"]}, {"frames", perSource[0].toObject()["frames"]}};
    else result = {{"sources", perSource}};
    if (asSheet) {
        const QImage s = ContactSheet::compose(images, labels, a["columns"].toInt());
        result["sheet_size"] = QJsonArray{s.width(), s.height()};
        if (!outDir.isEmpty()) {
            // A folder (also one to be made: no extension, like "C:/tmp/check" from a model) gets sheet.jpg; the
            // folders on the way are made, as for single images
            QString file = absolute(outDir);
            if (QFileInfo(file).isDir() || QFileInfo(file).suffix().isEmpty()) file = QDir(file).filePath("sheet.jpg");
            QDir().mkpath(QFileInfo(file).absolutePath());
            // What the name says (JPEG bytes in a .png file confuse programs that go by the name), else JPEG
            const char* format = QFileInfo(file).suffix().compare("png", Qt::CaseInsensitive) == 0 ? "PNG" : "JPG";
            if (!s.save(file, format, 85)) fail("WRITE_FAILED", "cannot write " + file);
            result["sheet"] = file;
        }
        if (ctx.mcp) ctx.images = {s};
    } else if (ctx.mcp) {
        ctx.images = images;
    }
    return result;
}

namespace {

// One scope panel (dark background, trace, graticule) like the Color page, in 10-bit levels like DaVinci
QImage scopePanel(const Scopes::Data& d, Scopes::Type type, const QString& title, QSize size)
{
    QImage img(size, QImage::Format_RGB32);
    img.fill(QColor(18, 18, 20));
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    const QRect area = img.rect().adjusted(34, 22, -8, -8);
    QFont font = p.font();
    font.setPixelSize(11);
    p.setFont(font);
    p.setPen(QColor(200, 200, 200));
    p.drawText(QRect(6, 3, size.width() - 12, 16), Qt::AlignLeft | Qt::AlignVCenter, title);
    const QColor grid(70, 70, 75);
    if (type == Scopes::Type::Vectorscope) {
        const int side = std::min(area.width(), area.height());
        const QRect sq(area.center().x() - side / 2, area.center().y() - side / 2, side, side);
        p.drawImage(sq, Scopes::trace(d, type));
        p.setPen(grid);
        p.drawEllipse(sq);
        p.drawLine(sq.center().x(), sq.top(), sq.center().x(), sq.bottom());
        p.drawLine(sq.left(), sq.center().y(), sq.right(), sq.center().y());
        // Skin tone line (about 123° like DaVinci's)
        p.setPen(QColor(200, 150, 90, 160));
        const double a = 123.0 * M_PI / 180.0;
        p.drawLine(sq.center(), sq.center() + QPoint(int(std::cos(a) * side / 2), int(-std::sin(a) * side / 2)));
        return img;
    }
    p.setPen(grid);
    for (int i = 0; i <= 4; ++i) {
        const int y = area.bottom() - i * area.height() / 4;
        p.drawLine(area.left(), y, area.right(), y);
        if (type != Scopes::Type::Histogram) // its height is a count, the levels run along x
            p.drawText(QRect(0, y - 7, 30, 14), Qt::AlignRight | Qt::AlignVCenter, QString::number(i * 1023 / 4));
    }
    if (type == Scopes::Type::Histogram) {
        // Curves R, G, B, Y over the levels (x), heights relative to the highest bin
        quint32 top = 1;
        for (int c = 0; c < 4; ++c)
            for (int i = 1; i < Scopes::kLevels - 1; ++i) top = std::max(top, d.hist[c][i]);
        const QColor colors[] = {QColor(230, 70, 70), QColor(70, 210, 90), QColor(80, 130, 240), QColor(220, 220, 220)};
        for (int c = 0; c < 4; ++c) {
            QPainterPath path;
            for (int i = 0; i < Scopes::kLevels; ++i) {
                const QPointF pt(area.left() + double(i) / (Scopes::kLevels - 1) * area.width(),
                                 area.bottom() - std::min(1.0, double(d.hist[c][i]) / top) * area.height());
                i == 0 ? path.moveTo(pt) : path.lineTo(pt);
            }
            p.setPen(QPen(colors[c], 1.2));
            p.drawPath(path);
        }
        return img;
    }
    p.drawImage(area, Scopes::trace(d, type));
    return img;
}

// Numbers the AI can judge a frame by: brightness spread, clipping, colour cast, saturation
QJsonObject frameStats(const QImage& frame)
{
    const QImage img = frame.scaledToWidth(std::min(frame.width(), 480)).convertToFormat(QImage::Format_RGB32);
    std::array<int, 256> hist{};
    double sum[3] = {0, 0, 0}, sat = 0;
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x, ++n) {
            const int r = qRed(line[x]), g = qGreen(line[x]), b = qBlue(line[x]);
            ++hist[std::clamp(Scopes::luma(r, g, b), 0, 255)];
            sum[0] += r;
            sum[1] += g;
            sum[2] += b;
            double cb = 0, cr = 0;
            Scopes::chroma(r / 255.0, g / 255.0, b / 255.0, &cb, &cr);
            sat += std::hypot(cb, cr);
        }
    }
    if (n == 0) return {};
    const auto percentile = [&](double q) {
        int acc = 0;
        for (int i = 0; i < 256; ++i)
            if ((acc += hist[i]) >= q * n) return i;
        return 255;
    };
    double mean = 0;
    for (int i = 0; i < 256; ++i) mean += double(i) * hist[i];
    const auto pct = [n](double v) { return std::round(v * 1000.0 / n) / 10; };
    int crushed = 0, clipped = 0;
    for (int i = 0; i <= 2; ++i) crushed += hist[i];
    for (int i = 253; i <= 255; ++i) clipped += hist[i];
    const auto lvl = [](double v) { return int(std::lround(v / 255.0 * 1023)); }; // 10-bit like the scopes
    const double avg = (sum[0] + sum[1] + sum[2]) / 3;
    return {{"levels", "10-bit 0..1023 like the scopes"},
            {"luma_mean", lvl(mean / n)}, {"luma_p1", lvl(percentile(0.01))}, {"luma_p50", lvl(percentile(0.5))},
            {"luma_p99", lvl(percentile(0.99))}, {"black_clipped_pct", pct(crushed)}, {"white_clipped_pct", pct(clipped)},
            {"rgb_mean", QJsonArray{lvl(sum[0] / n), lvl(sum[1] / n), lvl(sum[2] / n)}},
            {"cast", avg <= 0 ? QJsonValue("none")
                              : QJsonValue(QJsonObject{{"red", std::round((sum[0] - avg) / avg * 1000) / 10},
                                                       {"green", std::round((sum[1] - avg) / avg * 1000) / 10},
                                                       {"blue", std::round((sum[2] - avg) / avg * 1000) / 10}})},
            {"saturation_mean", std::round(sat / n * 2 * 1000) / 10}}; // 0..100 % of the vectorscope radius
}

} // namespace

QJsonObject cmdScopes(const QJsonObject& a, Context& ctx)
{
    const QString src = a["source"].toString();
    const bool isProject = isProjectFile(src);
    std::unique_ptr<Session> session;
    MediaSource media;
    ProjectFormat f;
    int length = 0;
    if (isProject) {
        session = std::make_unique<Session>(src);
        session->selectTimeline(a["timeline"]);
        f = session->format();
        length = TimelineOps::endFrame(session->project.timeline());
    } else {
        QJsonObject ma{{"file", src}};
        if (a.contains("project")) ma["project"] = a["project"];
        media = openMedia(ma);
        f = pictureFormat(media);
        length = media.info.length;
        if (!media.info.hasVideo) fail("NO_VIDEO", "the file has no picture");
    }
    if (length <= 0) {
        if (session) session->failEmpty();
        fail("EMPTY", "nothing to show: " + src);
    }
    const int at = std::clamp(a.contains("at") ? parseTime(a["at"], f, "at") : 0, 0, length - 1);
    const QImage frame = session ? Snapshot::timeline(f, session->project.renderTimeline(), at, 960)
                                 : Snapshot::media(f, media.info.path, at, 960);
    if (frame.isNull()) fail("DECODE_FAILED", QString("could not decode frame %1").arg(at));

    struct Kind {
        const char* id;
        Scopes::Type type;
        const char* title;
    };
    static const Kind kinds[] = {{"waveform", Scopes::Type::Waveform, "Waveform (luma)"},
                                 {"parade", Scopes::Type::Parade, "Parade (R | G | B)"},
                                 {"vectorscope", Scopes::Type::Vectorscope, "Vectorscope"},
                                 {"histogram", Scopes::Type::Histogram, "Histogram (R G B Y)"}};
    QVector<const Kind*> wanted;
    const QString list = a.contains("types") ? a["types"].toString() : "waveform,parade,vectorscope,histogram";
    for (const QString& t : list.split(',', Qt::SkipEmptyParts)) {
        const auto it = std::find_if(std::begin(kinds), std::end(kinds), [&](const Kind& k) { return t.trimmed() == k.id; });
        if (it == std::end(kinds)) fail("BAD_ARGUMENT", "types: waveform, parade, vectorscope, histogram");
        wanted << it;
    }
    const Scopes::Data d = Scopes::compute(frame);
    // One sheet: the frame, then the scopes (2 columns)
    const QSize cell(480, 270);
    QVector<QImage> panels{frame.scaled(cell, Qt::KeepAspectRatio, Qt::SmoothTransformation)};
    for (const Kind* k : wanted) panels << scopePanel(d, k->type, k->title, cell);
    const int cols = 2, rows = (int(panels.size()) + cols - 1) / cols;
    QImage sheet(cols * cell.width(), rows * cell.height(), QImage::Format_RGB32);
    sheet.fill(Qt::black);
    {
        QPainter p(&sheet);
        for (int i = 0; i < panels.size(); ++i) {
            const QPoint pos((i % cols) * cell.width(), (i / cols) * cell.height());
            p.drawImage(pos + QPoint((cell.width() - panels[i].width()) / 2, (cell.height() - panels[i].height()) / 2), panels[i]);
        }
    }
    QJsonObject result{{"source", absolute(src)}, {"frame", at}, {"tc", tc(at, f)}, {"stats", frameStats(frame)}};
    const QString out = a["out"].toString();
    if (!out.isEmpty()) {
        QDir().mkpath(QFileInfo(out).absolutePath());
        if (!sheet.save(out, nullptr, 90)) fail("WRITE_FAILED", "cannot write " + out);
        result["image"] = out;
    }
    if (ctx.mcp) ctx.images = {sheet};
    return result;
}

} // namespace Cli::detail
