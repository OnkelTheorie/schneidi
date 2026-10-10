// schneidi-cli: commands that look at the material (probe, silence, scenes, frames) without changing a project
#include "cli/CommandsDetail.h"

#include "core/I18n.h"
#include "core/Loudness.h"
#include "core/ProjectFile.h"
#include "core/Subtitles.h"
#include "core/Transcript.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"
#include "engine/Bundle.h"
#include "engine/Engine.h"
#include "engine/Exporter.h"
#include "engine/Extensions.h"
#include "engine/Transcriber.h"
#include "engine/Profiles.h"
#include "engine/Snapshot.h"
#include "ui/Scopes.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <memory>

namespace Cli::detail {

namespace {
// Media file + the frame rate frames count in (project format, or the file's own rate)
struct MediaSource {
    ProjectFormat format;
    MediaInfo info;
};

MediaSource openMedia(const QJsonObject& a)
{
    const QString path = a["file"].toString();
    if (!QFileInfo::exists(path)) fail("NOT_FOUND", "file not found: " + path);
    MediaSource m;
    if (a.contains("project")) {
        Session s(a["project"].toString());
        m.format = s.format();
    } else {
        const ClipFormat cf = detectClipFormat(path);
        if (cf.ok) {
            m.format.rate = cf.suggested;
            m.format.width = cf.width & ~1;
            m.format.height = cf.height & ~1;
        }
    }
    m.info = Engine::probe(m.format, path);
    if (m.info.length <= 0 || (!m.info.hasVideo && !m.info.hasAudio)) fail("BAD_MEDIA", "cannot read media: " + path);
    return m;
}

QImage sheet(const QVector<QImage>& images, const QStringList& labels)
{
    const int cols = std::min<int>(images.size(), images.size() <= 4 ? 2 : images.size() <= 9 ? 3 : 4);
    const int rows = (images.size() + cols - 1) / cols;
    const QSize cell = images.first().size();
    QImage out(cols * cell.width(), rows * cell.height(), QImage::Format_RGB32);
    out.fill(Qt::black);
    QPainter p(&out);
    QFont font = p.font();
    font.setPixelSize(std::max(12, cell.height() / 14));
    p.setFont(font);
    for (int i = 0; i < images.size(); ++i) {
        const QPoint at((i % cols) * cell.width(), (i / cols) * cell.height());
        p.drawImage(at, images[i]);
        const QRect box(at + QPoint(4, 4), QSize(p.fontMetrics().horizontalAdvance(labels[i]) + 8, p.fontMetrics().height() + 2));
        p.fillRect(box, QColor(0, 0, 0, 170));
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, labels[i]);
        p.setPen(QColor(60, 60, 60));
        p.drawRect(QRect(at, cell).adjusted(0, 0, -1, -1));
    }
    return out;
}

} // namespace

QJsonObject cmdProbe(const QJsonObject& a, Context&)
{
    const MediaSource m = openMedia(a);
    const ClipFormat cf = detectClipFormat(m.info.path);
    QJsonObject o{{"file", m.info.path}, {"frames", m.info.length}, {"seconds", seconds(m.info.length, m.format)},
                  {"duration_tc", tc(m.info.length, m.format)}, {"video", m.info.hasVideo}, {"audio", m.info.hasAudio},
                  {"frames_count_in", formatJson(m.format)["fps"]}};
    if (m.info.isImage) o["image"] = true;
    if (cf.ok) {
        o["width"] = cf.width;
        o["height"] = cf.height;
        o["fps"] = std::round(cf.rate.fps() * 1000) / 1000;
        if (cf.variable) o["variable_frame_rate"] = true;
    }
    if (m.info.hasAudio) {
        QJsonArray streams;
        for (int i = 0; i < m.info.audioStreamCount(); ++i) streams << m.info.audioStreamName(i);
        o["audio_streams"] = streams;
    }
    return o;
}

QJsonObject cmdSilence(const QJsonObject& a, Context& ctx)
{
    const MediaSource m = openMedia(a);
    if (!m.info.hasAudio) fail("NO_AUDIO", "file has no audio: " + m.info.path);
    const ProjectFormat& f = m.format;
    const double thresholdDb = a.contains("threshold_db") ? a["threshold_db"].toDouble() : -35.0;
    const int minLen = a.contains("min") ? parseTime(a["min"], f, "min") : int(std::lround(0.5 * f.rate.fps()));
    const int pad = a.contains("pad") ? parseTime(a["pad"], f, "pad") : int(std::lround(0.1 * f.rate.fps()));
    Clip c;
    c.mediaPath = m.info.path;
    c.out = m.info.length - 1;
    const auto peaks = AudioAnalysis::clipFramePeaks(f, c, [&ctx](double p) {
        if (ctx.progress) ctx.progress(p);
        return true;
    });
    if (!peaks) fail("NO_AUDIO", "could not decode the audio of " + m.info.path);
    const float limit = float(std::pow(10.0, thresholdDb / 20.0));
    const int n = int(peaks->size());
    // Quiet runs of at least minLen frames; the pad of sound stays on both sides (not at the file start/end)
    QJsonArray silence, sound;
    int keepFrom = 0, silent = 0;
    for (int i = 0; i <= n; ++i) {
        if (i < n && (*peaks)[i] < limit) {
            ++silent;
            continue;
        }
        if (silent >= minLen) {
            const int from = i - silent, to = i;
            const int cutFrom = from == 0 ? 0 : std::min(to, from + pad);
            const int cutTo = to == n ? n : std::max(cutFrom, to - pad);
            if (cutTo > cutFrom) {
                silence << range(cutFrom, cutTo, f);
                if (cutFrom > keepFrom) sound << range(keepFrom, cutFrom, f);
                keepFrom = cutTo;
            }
        }
        silent = 0;
    }
    if (keepFrom < n) sound << range(keepFrom, n, f);
    int kept = 0;
    for (const QJsonValue& r : sound) kept += r["to"].toInt() - r["from"].toInt();
    return {{"file", m.info.path}, {"frames", n}, {"fps", formatJson(f)["fps"]}, {"threshold_db", thresholdDb},
            {"silence", silence}, {"sound", sound}, {"kept_frames", kept}, {"kept_s", seconds(kept, f)},
            {"removed_s", seconds(n - kept, f)}};
}


QJsonObject cmdScenes(const QJsonObject& a, Context& ctx)
{
    const MediaSource m = openMedia(a);
    if (!m.info.hasVideo || m.info.isImage) fail("NO_VIDEO", "file has no moving picture: " + m.info.path);
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

QJsonObject cmdFrames(const QJsonObject& a, Context& ctx)
{
    const QString src = a["source"].toString();
    const bool isProject = src.endsWith("." + QString(ProjectFile::Extension));
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
        f = media.format;
        length = media.info.length;
    }
    if (length <= 0) fail("EMPTY", "nothing to show (empty timeline)");
    QVector<int> at;
    for (const QJsonValue& v : a["at"].toArray()) at << parseTime(v, f, "at");
    if (a.contains("every")) {
        const int step = parseTime(a["every"], f, "every");
        if (step <= 0) fail("BAD_ARGUMENT", "'every' must be > 0");
        for (int fr = 0; fr < length; fr += step) at << fr;
    }
    if (a.contains("count")) {
        const int n = std::clamp(a["count"].toInt(), 1, 100);
        for (int i = 0; i < n; ++i) at << int((i + 0.5) * length / n);
    }
    if (at.isEmpty()) fail("BAD_ARGUMENT", "give 'at' (times), 'every' (interval) or 'count' (evenly spread)");
    if (at.size() > 100) fail("BAD_ARGUMENT", QString("too many frames (%1, max 100)").arg(at.size()));
    const int maxEdge = a.contains("width") ? std::clamp(a["width"].toInt(), 64, 3840) : (ctx.mcp ? 480 : 640);
    const QString outDir = a["out"].toString();
    const bool asSheet = a["sheet"].toBool();
    if (outDir.isEmpty() && !ctx.mcp) fail("BAD_ARGUMENT", "missing 'out' (folder for the images, or the sheet file with --sheet)");

    const Timeline tl = session ? session->project.renderTimeline() : Timeline{};
    QVector<QImage> images;
    QStringList labels;
    QJsonArray frames;
    for (int i = 0; i < at.size(); ++i) {
        const int fr = std::clamp(at[i], 0, length - 1);
        QImage img = session ? Snapshot::timeline(f, tl, fr, maxEdge) : Snapshot::media(f, media.info.path, fr, maxEdge);
        if (img.isNull()) fail("DECODE_FAILED", QString("could not decode frame %1").arg(fr));
        images << img;
        labels << tc(fr, f);
        QJsonObject o{{"frame", fr}, {"tc", tc(fr, f)}, {"s", seconds(fr, f)}};
        if (!asSheet && !outDir.isEmpty()) {
            QDir().mkpath(outDir);
            const QString file = QDir(outDir).absoluteFilePath(QString("frame_%1.jpg").arg(fr, 6, 10, QChar('0')));
            if (!img.save(file, "JPG", 85)) fail("WRITE_FAILED", "cannot write " + file);
            o["file"] = file;
        }
        frames << o;
        if (ctx.progress) ctx.progress(double(i + 1) / at.size());
    }
    QJsonObject result{{"source", absolute(src)}, {"frames", frames}};
    if (asSheet) {
        const QImage s = sheet(images, labels);
        if (!outDir.isEmpty()) {
            QString file = absolute(outDir);
            if (QFileInfo(file).isDir()) file = QDir(file).filePath("sheet.jpg");
            if (!s.save(file, "JPG", 85)) fail("WRITE_FAILED", "cannot write " + file);
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
    const bool isProject = src.endsWith("." + QString(ProjectFile::Extension));
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
        f = media.format;
        length = media.info.length;
        if (!media.info.hasVideo) fail("NO_VIDEO", "the file has no picture");
    }
    if (length <= 0) fail("EMPTY", "nothing to show (empty timeline)");
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

namespace {

double db1(double v) { return v <= LoudnessMeter::kSilence ? -200.0 : std::round(v * 10) / 10; }

QJsonObject loudnessJson(const AudioAnalysis::Loudness& l)
{
    return {{"integrated_lufs", db1(l.integrated)}, {"range_lu", db1(l.range)},
            {"true_peak_db", db1(l.truePeakDb)}, {"sample_peak_db", db1(l.peakDb)},
            {"max_momentary_lufs", db1(l.maxMomentary)}, {"max_short_term_lufs", db1(l.maxShortTerm)}};
}
} // namespace

QJsonObject cmdLoudness(const QJsonObject& a, Context& ctx)
{
    const QString src = a["source"].toString();
    const auto progress = [&ctx](double p) {
        if (ctx.progress) ctx.progress(p);
        return true;
    };
    QJsonObject result{{"source", absolute(src)}};
    if (!src.endsWith("." + QString(ProjectFile::Extension))) {
        QJsonObject ma{{"file", src}};
        const MediaSource m = openMedia(ma);
        if (!m.info.hasAudio) fail("NO_AUDIO", "the file has no sound");
        Clip c;
        c.mediaPath = m.info.path;
        c.out = m.info.length - 1;
        c.audioStream = a["audio_stream"].toInt();
        if (c.audioStream >= m.info.audioStreamCount()) fail("BAD_ARGUMENT", QString("the file has %1 audio stream(s)").arg(m.info.audioStreamCount()));
        const auto l = AudioAnalysis::clipLoudness(m.format, c, progress);
        if (!l) fail("DECODE_FAILED", "could not read the sound");
        result["loudness"] = loudnessJson(*l);
        return result;
    }
    Session s(src);
    s.selectTimeline(a["timeline"]);
    const ProjectFormat& f = s.format();
    const QJsonArray clips = a["clips"].toArray();
    if (!clips.isEmpty()) {
        // Every clip on its own (its sound without volume/fades, what 'normalize' measures)
        QJsonArray list;
        for (int i = 0; i < clips.size(); ++i) {
            const Clip* c = TimelineOps::findClip(s.project.timeline(), clips[i].toVariant().toInt());
            if (!c) fail("NOT_FOUND", "no clip with id " + clips[i].toVariant().toString());
            QJsonObject o{{"clip", c->id}};
            if (const auto l = AudioAnalysis::clipLoudness(f, *c)) o["loudness"] = loudnessJson(*l);
            else o["no_sound"] = true;
            list << o;
            progress(double(i + 1) / clips.size());
        }
        result["clips"] = list;
        return result;
    }
    // The mix as exported, optionally only some audio tracks
    Timeline tl = s.project.renderTimeline();
    const int end = TimelineOps::endFrame(tl);
    if (end <= 0) fail("EMPTY", "the timeline is empty");
    const int from = a.contains("from") ? parseTime(a["from"], f, "from") : 0;
    const int to = a.contains("to") ? std::min(end, parseTime(a["to"], f, "to")) : end;
    if (to <= from) fail("BAD_ARGUMENT", "'to' must be after 'from'");
    QVector<int> only;
    for (const QString& t : a["tracks"].toString().split(',', Qt::SkipEmptyParts)) {
        TrackKind k;
        const int i = parseTrack(t, &k);
        if (k != TrackKind::Audio || i >= tl.audio.size()) fail("BAD_ARGUMENT", "no audio track " + t.trimmed());
        only << i;
    }
    if (!only.isEmpty())
        for (int i = 0; i < tl.audio.size(); ++i) {
            tl.audio[i].muted = !only.contains(i);
            tl.audio[i].solo = false;
        }
    const auto l = AudioAnalysis::timelineLoudness(f, tl, from, to, progress);
    if (!l) fail("DECODE_FAILED", "could not mix the sound");
    result["range"] = range(from, to, f);
    result["loudness"] = loudnessJson(*l);
    return result;
}

QJsonObject cmdTranscribe(const QJsonObject& a, Context& ctx)
{
    const QString src = a["source"].toString();
    TranscribeRequest req;
    req.model = Extensions::whisperModelPath(a["model"].toString());
    if (req.model.isEmpty())
        fail("NOT_INSTALLED", a.contains("model") ? "whisper model not installed: " + a["model"].toString()
                                                  : "no whisper model installed (see `extensions`, e.g. --install whisper,small)");
    if (Extensions::whisperProgram().isEmpty())
        fail("NOT_INSTALLED", "whisper is not installed (see `extensions`, e.g. --install whisper,small)");
    req.language = a.contains("language") ? a["language"].toString() : "auto";
    req.threads = a["threads"].toInt();
    ProjectFormat f;
    int offset = 0, length = 0;
    if (src.endsWith("." + QString(ProjectFile::Extension))) {
        // The timeline's sound as it is mixed for the export; times are timeline frames
        Session s(src);
        s.selectTimeline(a["timeline"]);
        f = s.format();
        req.timeline = s.project.renderTimeline();
        req.format = f;
        const int end = TimelineOps::endFrame(req.timeline);
        if (end <= 0) fail("EMPTY", "the timeline is empty");
        req.from = a.contains("from") ? parseTime(a["from"], f, "from") : 0;
        req.to = a.contains("to") ? std::min(end, parseTime(a["to"], f, "to")) : end;
        if (req.to <= req.from) fail("BAD_ARGUMENT", "'to' must be after 'from'");
        for (const QString& t : a["tracks"].toString().split(',', Qt::SkipEmptyParts)) {
            TrackKind k;
            const int i = parseTrack(t, &k);
            if (k != TrackKind::Audio || i >= req.timeline.audio.size()) fail("BAD_ARGUMENT", "no audio track " + t.trimmed());
            req.audioTracks << i;
        }
        offset = req.from;
        length = req.to - req.from;
    } else {
        QJsonObject ma{{"file", src}};
        if (a.contains("project")) ma["project"] = a["project"];
        const MediaSource m = openMedia(ma);
        if (!m.info.hasAudio) fail("NO_AUDIO", "file has no audio: " + m.info.path);
        f = m.format;
        req.media = m.info.path;
        req.audioStream = std::clamp(a["audio_stream"].toInt(), 0, std::max(0, m.info.audioStreamCount() - 1));
        length = m.info.length;
    }

    Transcriber t;
    QEventLoop loop;
    bool ok = false;
    QString message;
    QObject::connect(&t, &Transcriber::progress, &loop, [&ctx](double p) {
        if (ctx.progress) ctx.progress(p);
    });
    QObject::connect(&t, &Transcriber::finished, &loop, [&](bool success, const QString& msg) {
        ok = success;
        message = msg;
        loop.quit();
    });
    QString error;
    if (!t.start(req, &error)) fail("TRANSCRIBE_FAILED", error);
    loop.exec();
    if (!ok) fail("TRANSCRIBE_FAILED", message);

    const Transcript::Result& r = t.result();
    const int maxChars = a.contains("max_chars") ? std::clamp(a["max_chars"].toInt(), 10, 200) : 42;
    QVector<SubtitleCue> cues = Transcript::toCues(r.words, f.rate.fps(), maxChars, 700, offset);
    Transcript::startAtSound(cues, r.levels, f.rate.fps(), offset);
    QJsonArray segments;
    for (const SubtitleCue& c : cues) {
        QJsonObject o = range(c.start, std::min(c.end, offset + length), f);
        o["text"] = c.text;
        segments << o;
    }
    QJsonObject out{{"source", absolute(src)}, {"language", r.language}, {"fps", formatJson(f)["fps"]},
                    {"model", QFileInfo(req.model).fileName()}, {"segments", segments}};
    if (a["words"].toBool()) {
        // Word times for cutting: `to` is where the word is spoken to (pauses after it are left out)
        QJsonArray words;
        const auto fr = [&](qint64 ms) { return offset + int(std::lround(ms / 1000.0 * f.rate.fps())); };
        for (const Transcript::Word& w : r.words)
            words << QJsonObject{{"from", fr(w.from)}, {"to", std::max(fr(w.from) + 1, fr(Transcript::spokenEnd(w)))},
                                 {"text", w.text}};
        out["words"] = words;
    }
    if (a.contains("srt")) {
        const QString path = absolute(a["srt"].toString());
        QFile file(path);
        QVector<SubtitleCue> shifted = cues;
        if (!file.open(QIODevice::WriteOnly) || file.write(Subtitles::toSrt(shifted, f.rate.fps(), offset)) < 0)
            fail("WRITE_FAILED", "cannot write " + path);
        out["srt"] = path;
    }
    return out;
}

QJsonObject cmdExtensions(const QJsonObject& a, Context& ctx)
{
    // "whisper,small" -> items; short model names work too
    const auto items = [](const QString& list) {
        QVector<const Extensions::Item*> out;
        for (QString id : list.split(',', Qt::SkipEmptyParts)) {
            id = id.trimmed();
            const Extensions::Item* i = Extensions::find(id);
            if (!i) i = Extensions::find("whisper-model-" + id);
            if (!i) fail("NOT_FOUND", "no extension " + id + " (see `extensions`)");
            out << i;
        }
        return out;
    };
    QJsonArray done;
    for (const Extensions::Item* i : Extensions::withDependencies(items(a["remove"].toString()))) {
        if (!Extensions::remove(*i)) fail("FAILED", "could not remove " + i->id);
        done << QJsonObject{{"removed", i->id}};
    }
    const QVector<const Extensions::Item*> install = Extensions::withDependencies(items(a["install"].toString()));
    qint64 total = 0, before = 0;
    for (const Extensions::Item* i : install) total += i->size;
    for (const Extensions::Item* i : install) {
        if (Extensions::isInstalled(*i) && !a["reinstall"].toBool()) {
            before += i->size;
            continue;
        }
        Extensions::Installer installer;
        QEventLoop loop;
        bool ok = false;
        QString message;
        QObject::connect(&installer, &Extensions::Installer::progress, &loop, [&](qint64 received, qint64) {
            if (ctx.progress && total > 0) ctx.progress(double(before + received) / total);
        });
        QObject::connect(&installer, &Extensions::Installer::finished, &loop, [&](bool success, const QString& msg) {
            ok = success;
            message = msg;
            loop.quit();
        });
        QString error;
        if (!installer.start(*i, &error)) fail("DOWNLOAD_FAILED", error);
        loop.exec();
        if (!ok) fail("DOWNLOAD_FAILED", i->id + ": " + message);
        before += i->size;
        done << QJsonObject{{"installed", i->id}};
    }
    QJsonArray list;
    for (const Extensions::Item& i : Extensions::catalog()) {
        QJsonObject o{{"id", i.id}, {"name", T(i.name)}, {"description", T(i.description)},
                      {"download_mb", std::round(i.size / 1e5) / 10}, {"installed", Extensions::isInstalled(i)}};
        list << o;
    }
    QJsonObject out{{"extensions", list}, {"folder", Extensions::rootDir()}};
    if (!done.isEmpty()) out["done"] = done;
    if (Extensions::find("whisper") == nullptr && !Extensions::whisperProgram().isEmpty())
        out["whisper_program"] = Extensions::whisperProgram(); // own build on PATH
    return out;
}

} // namespace Cli::detail
