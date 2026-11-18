// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/KillFeedLines.h"

#include "core/AppPaths.h"
#include "core/I18n.h"
#include "core/MediaAspect.h"
#include "core/Retime.h"
#include "engine/BarDetect.h"
#include "engine/ReframeAnalysis.h"
#include "engine/video/Reader.h"

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace KillFeedLines {

namespace {

constexpr int kVersion = 2; // bump when the measurement changes (old results are not reused)
constexpr const char* kHeader = "schneidi-killfeed 1";
constexpr const char* kSuffix = ".killfeed";
constexpr double kSamplesPerSecond = 10;
constexpr int kScale = 1080; // the picture is read at this height (sizes below are pixels of it)

constexpr int kEdge = 12;      // a box row differs from the picture next to it by at least this much (a channel)
constexpr int kFlat = 24;      // ... while its top and bottom row differ by at most this much
constexpr int kRightIn = 3;    // the runs start this many pixels left of the box's right edge
constexpr int kStartMiss = 6;  // the first box column within this many pixels of the start
constexpr int kMiss = 10;      // a run ends at a gap of more columns
constexpr double kHits = 0.6;  // ... and has box columns in at least this part of it
constexpr int kMinLine = 80;   // the shortest line ("A [weapon] B")
constexpr int kDark = 45;      // a picture this dark (luma) above and below a line hides its box: the text counts
constexpr int kText = 130;     // text: brightest channel at least this, luma at least kTextLuma
constexpr int kTextLuma = 100;
constexpr int kTextReach = 45; // the text ends within this many pixels of the box's right edge
constexpr int kMaxLines = 10;

int lumaOf(const uchar* p) { return (2 * p[0] + 5 * p[1] + p[2]) / 8; }
int diffOf(const uchar* a, const uchar* b)
{
    return std::max({std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2])});
}

// An RGB888 part of the picture at the 1080p scale: pixel (x, y) of the picture at bits + (y - oy) * stride + ...
struct View {
    const uchar* bits = nullptr;
    qsizetype stride = 0;
    int ox = 0, oy = 0, w = 0, h = 0; // the part's place in the picture
    int pw = 0, ph = 0;               // the picture's size
    bool has(int x, int y) const { return x >= ox && y >= oy && x < ox + w && y < oy + h; }
    const uchar* at(int x, int y) const { return bits + (y - oy) * stride + (x - ox) * 3; }
};

struct Geometry {
    int lineH = 31;     // a line's height
    double pitch = 33.5;
    int firstMin = 65, firstMax = 103;
    int right = 1905;   // the runs start here
    int edge = 1908;    // the box's last column
    int leftmost = 1056; // a line reaches at most this far left
};

Geometry geometryOf(const GameHud::Game& g, int pw, int ph)
{
    Geometry x;
    x.lineH = std::max(8, int(std::lround(g.lineHeight * ph)));
    x.pitch = g.linePitch * ph;
    x.firstMin = int(std::floor(g.firstLineMin * ph));
    x.firstMax = int(std::ceil(g.firstLineMax * ph));
    x.edge = int(std::floor(g.frameRight * pw));
    x.right = x.edge - kRightIn;
    x.leftmost = std::max(0, int(std::floor(std::min(g.frameLeft > 0 ? g.frameLeft : g.feed.left(), g.feed.left()) * pw)));
    return x;
}

// The part of the picture the lines can be in (pixels at the 1080p scale), with room for the rows around them
QRect areaOf(const Geometry& g, int pw, int ph)
{
    // From the picture's top: measure crops the frame at this row scaled to the file's size, and a row that does not
    // fall on a whole (even) row of the file shifts the picture there by a fraction of a row (a 4:2:0 picture its
    // colour half a row) - the boxes' edges blur and lines go missing. The rows above the feed cost little.
    const int top = 0;
    const int bottom = std::min(ph, int(std::ceil(g.firstMax + (kMaxLines - 1) * g.pitch)) + g.lineH + 6);
    const int right = std::min(pw, g.edge + 8);
    return QRect(QPoint(g.leftmost, top), QPoint(right - 1, bottom - 1));
}

// A column of a box whose top row is y. Of the two rows just above (below) it one is enough to differ: a line found
// a row higher or lower, a line right below it
bool boxColumn(const View& v, const Geometry& g, int x, int y)
{
    const int top = y + 3, bottom = y + g.lineH - 4;
    if (!v.has(x, y - 2) || !v.has(x, y + g.lineH + 1)) return false;
    const uchar *i1 = v.at(x, top), *i2 = v.at(x, bottom);
    if (diffOf(i1, i2) > kFlat) return false;
    const int up = std::max(diffOf(v.at(x, y - 1), i1), diffOf(v.at(x, y - 2), i1));
    const int down = std::max(diffOf(v.at(x, y + g.lineH), i2), diffOf(v.at(x, y + g.lineH + 1), i2));
    return std::min(up, down) >= kEdge;
}

// The box of a line whose top row is y: its left edge, or -1
int boxLine(const View& v, const Geometry& g, int y)
{
    // The box ends at its right edge: just right of it the rows are no box (a horizontal edge of the picture is)
    int outside = 0;
    for (int x = g.edge + 2; x <= g.edge + 5; ++x) outside += v.has(x, y) && boxColumn(v, g, x, y);
    if (outside >= 3) return -1;
    int last = -1, hits = 0;
    for (int x = g.right; x >= g.leftmost; --x) {
        if (boxColumn(v, g, x, y)) {
            last = x;
            ++hits;
        } else if (last < 0 ? g.right - x > kStartMiss : last - x > kMiss) {
            break;
        }
    }
    if (last < 0) return -1;
    const int length = g.right - last + 1;
    if (length < kMinLine || hits < kHits * length) return -1;
    return last;
}

// A line's text over a dark picture (its box does not show) whose top row is y: its left edge, or -1. Score: the
// number of text pixels.
int textLine(const View& v, const Geometry& g, int y, int* score)
{
    *score = 0;
    const int above = y - 2, below = y + g.lineH + 1;
    if (!v.has(g.right, above) || !v.has(g.right, below)) return -1;
    long sum = 0;
    int n = 0;
    for (int x = std::max(g.leftmost, g.right - 250); x <= g.right; ++x) {
        sum += lumaOf(v.at(x, above)) + lumaOf(v.at(x, below));
        n += 2;
    }
    if (n == 0 || sum / n >= kDark) return -1;
    const auto text = [&](int x, int row) {
        const uchar* p = v.at(x, row);
        return std::max({p[0], p[1], p[2]}) >= kText && lumaOf(p) >= kTextLuma;
    };
    // The rows at the box's edges are free of text
    for (int row : {y - 1, y, y + 1, y + g.lineH - 2, y + g.lineH - 1, y + g.lineH})
        for (int x = g.right - 250; x <= g.right; ++x)
            if (x >= g.leftmost && text(x, row)) return -1;
    int count = 0, first = -1, last = -1;
    for (int x = g.right; x >= g.leftmost; --x) {
        bool any = false;
        for (int row = y + 6; row <= y + g.lineH - 6; ++row)
            if (text(x, row)) any = true, ++count;
        if (any) {
            if (first < 0) first = x;
            last = x;
        } else if (first >= 0 && last - x > int(1.2 * g.lineH)) {
            break;
        } else if (first < 0 && g.right - x > kTextReach) {
            return -1;
        }
    }
    if (first < 0 || count < 2 * g.lineH || first - last < g.lineH) return -1;
    *score = count;
    return last;
}

// The line at about row y (within `slack` rows): its top and left edge; false = none
bool lineNear(const View& v, const Geometry& g, int from, int to, int* top, int* left)
{
    int bestBox = -1, boxTop = 0, boxLeft = 0;
    for (int y = from; y <= to; ++y) {
        const int l = boxLine(v, g, y);
        if (l >= 0 && (bestBox < 0 || g.right - l > bestBox)) bestBox = g.right - l, boxTop = y, boxLeft = l;
    }
    if (bestBox >= 0) {
        *top = boxTop;
        *left = boxLeft;
        return true;
    }
    // Text only: the middle of the rows that read it best
    int best = 0, a = -1, b = -1, textLeft = 0;
    for (int y = from; y <= to; ++y) {
        int s = 0;
        const int l = textLine(v, g, y, &s);
        if (l < 0) continue;
        if (s > best) best = s, a = b = y, textLeft = l;
        else if (s == best && b == y - 1) b = y;
    }
    if (a < 0) return false;
    *top = (a + b) / 2;
    *left = textLeft;
    return true;
}

Sample detectView(const View& v, const Geometry& g)
{
    Sample s;
    int top = 0, left = 0;
    if (!lineNear(v, g, g.firstMin, g.firstMax, &top, &left)) return s;
    int leftmost = left, lastTop = top;
    s.lines = 1;
    double expected = top;
    while (s.lines < kMaxLines) {
        expected += g.pitch;
        const int y = int(std::lround(expected));
        int t = 0, l = 0;
        if (!lineNear(v, g, y - 2, y + 2, &t, &l)) break;
        ++s.lines;
        leftmost = std::min(leftmost, l);
        lastTop = t;
        expected = t;
    }
    s.left = double(leftmost) / v.pw;
    s.bottom = double(lastTop + g.lineH) / v.ph;
    return s;
}

QString base(const QString& file, const QRectF& content, const QString& game)
{
    const QFileInfo fi(file);
    QString id = QString("%1|%2|%3|%4|%5")
                     .arg(kVersion)
                     .arg(fi.absoluteFilePath())
                     .arg(fi.size())
                     .arg(fi.lastModified().toMSecsSinceEpoch())
                     .arg(game);
    if (BarDetect::hasBars(content))
        id += QString("|%1 %2 %3 %4").arg(content.x(), 0, 'f', 4).arg(content.y(), 0, 'f', 4).arg(content.width(), 0, 'f', 4)
                  .arg(content.height(), 0, 'f', 4);
    return QString::fromLatin1(QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha1).toHex());
}

bool parseName(const QString& name, const QString& b, qint64* from, qint64* to)
{
    if (!name.startsWith(b + '_') || !name.endsWith(kSuffix)) return false;
    const QStringList parts = name.mid(b.size() + 1).chopped(int(std::strlen(kSuffix))).split('_');
    if (parts.size() != 2) return false;
    bool ok1 = false, ok2 = false;
    *from = parts[0].toLongLong(&ok1);
    *to = parts[1].toLongLong(&ok2);
    return ok1 && ok2 && *to >= *from;
}

std::optional<Samples> readSamples(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QList<QByteArray> lines = f.readAll().split('\n');
    if (lines.size() < 2 || lines[0] != kHeader) return std::nullopt;
    bool ok = false;
    const int n = lines[1].toInt(&ok);
    if (!ok || n < 0 || lines.size() < n + 2) return std::nullopt;
    Samples out;
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        const QList<QByteArray> v = lines[i + 2].split(' ');
        if (v.size() != 4) return std::nullopt;
        Sample s;
        bool a = false, b = false, c = false, d = false;
        s.seconds = v[0].toDouble(&a);
        s.lines = v[1].toInt(&b);
        s.left = v[2].toDouble(&c);
        s.bottom = v[3].toDouble(&d);
        if (!a || !b || !c || !d || s.lines < 0 || s.lines > kMaxLines) return std::nullopt;
        out << s;
    }
    return out;
}

QByteArray writeSamples(const Samples& samples)
{
    QByteArray out = QByteArray(kHeader) + "\n" + QByteArray::number(samples.size()) + "\n";
    for (const Sample& s : samples)
        out += QByteArray::number(s.seconds, 'f', 4) + ' ' + QByteArray::number(s.lines) + ' '
               + QByteArray::number(s.left, 'f', 5) + ' ' + QByteArray::number(s.bottom, 'f', 5) + '\n';
    return out;
}

std::optional<Samples> cached(const QString& file, double from, double to, const QRectF& content, const QString& game)
{
    const QString b = base(file, content, game);
    const QDir dir(cacheDir());
    const qint64 f0 = qint64(std::llround(from * 1000)), t0 = qint64(std::llround(to * 1000));
    QString best;
    qint64 bestLength = -1;
    for (const QString& name : dir.entryList({b + "_*" + kSuffix}, QDir::Files)) {
        qint64 f = 0, t = 0;
        if (!parseName(name, b, &f, &t) || f > f0 || t < t0) continue;
        if (best.isEmpty() || t - f < bestLength) best = dir.filePath(name), bestLength = t - f;
    }
    if (best.isEmpty()) return std::nullopt;
    return readSamples(best);
}

} // namespace

bool canMeasure(const GameHud::Game& game)
{
    return game.lineHeight > 0 && game.linePitch > 0 && game.frameRight > 0 && game.firstLineMax >= game.firstLineMin;
}

Sample detect(const QImage& picture, const GameHud::Game& game)
{
    if (!canMeasure(game) || picture.isNull()) return {};
    const int ph = kScale, pw = std::max(2, int(std::lround(double(picture.width()) * kScale / picture.height())));
    const QImage scaled = (picture.height() == ph ? picture : picture.scaled(pw, ph, Qt::IgnoreAspectRatio,
                                                                             Qt::SmoothTransformation))
                              .convertToFormat(QImage::Format_RGB888);
    const Geometry g = geometryOf(game, pw, ph);
    const QRect area = areaOf(g, pw, ph);
    View v;
    v.bits = scaled.constBits() + area.y() * scaled.bytesPerLine() + area.x() * 3;
    v.stride = scaled.bytesPerLine();
    v.ox = area.x();
    v.oy = area.y();
    v.w = area.width();
    v.h = area.height();
    v.pw = pw;
    v.ph = ph;
    return detectView(v, g);
}

QString cacheDir() { return AppPaths::writable(QStandardPaths::CacheLocation) + "/killfeed"; }

std::optional<Samples> measure(const QString& file, double from, double to, const QRectF& contentIn,
                               const GameHud::Game& game, QString* error, const std::atomic<bool>* cancel,
                               const std::function<void(int)>& progress)
{
    auto fail = [&](const QString& msg) -> std::optional<Samples> {
        if (error) *error = msg;
        return std::nullopt;
    };
    if (!canMeasure(game)) return fail(QStringLiteral("no kill feed lines known for %1").arg(game.id));
    const QRectF content = BarDetect::hasBars(contentIn) ? contentIn : QRectF(0, 0, 1, 1);
    if (auto c = cached(file, from, to, content, game.id)) {
        if (progress) progress(1000);
        return c;
    }
    if (!QFileInfo::exists(file)) return fail(T("Datei nicht gefunden: %1").arg(file));
    Video::Reader::Options ro;
    ro.hardware = false; // in order through the range: the software decoder keeps up
    std::unique_ptr<Video::Reader> reader = Video::Reader::open(file, ro);
    if (!reader) return fail(T("Datei lässt sich nicht öffnen: %1").arg(file));
    reader->setFrameRate(AVRational{25, 1});
    const Video::Info& info = reader->info();
    const double srcFps = info.frameRate.num > 0 && info.frameRate.den > 0 ? av_q2d(info.frameRate) : 25.0;
    const int64_t first = int64_t(std::floor(from * srcFps));
    const int64_t last = std::max(first, int64_t(std::ceil(to * srcFps)));
    const double step = std::max(1.0, srcFps / kSamplesPerSecond); // in positions (a 25 fps file: every 2.5th)
    const int64_t endFrame = info.duration > 0 ? int64_t(std::ceil(double(info.duration) / AV_TIME_BASE * srcFps)) : 0;

    std::unique_ptr<SwsContext, void (*)(SwsContext*)> sws(nullptr, [](SwsContext* x) { sws_freeContext(x); });
    std::vector<uint8_t> rgb;
    Samples out;
    Geometry g;
    QRect area;
    int pw = 0, ph = kScale;
    int64_t done = first - 1; // the last position read
    // Read up to `last` or the end of the file: only then is the result the whole range (a read error stops early:
    // what was read is used, but not cached under the whole range's name)
    bool complete = false, stale = false;
    for (int k = 0;; ++k) {
        const int64_t pos = first + int64_t(std::llround(k * step));
        if (pos > last) {
            complete = true;
            break;
        }
        if (pos <= done) continue; // the file skipped there
        if (cancel && cancel->load()) break;
        const auto pic = reader->picture(pos);
        if (!pic || !pic->frame) {
            // No picture at the end of the file (its length is rounded) is the end; elsewhere an error
            complete = endFrame > 0 && pos >= endFrame - int64_t(std::ceil(step)) - 1;
            break;
        }
        if (pic->position < pos && (endFrame <= 0 || pos >= endFrame)) { // past the end
            complete = true;
            break;
        }
        // Far behind the position inside the file: the file broke off (an earlier picture again); what follows is read
        // as far as it goes, but not cached as the whole range
        if (pic->position < pos - int64_t(std::ceil(2 * step)) - 1) stale = true;
        const AVFrame* f = pic->frame;
        // The game picture (bars cut off) as displayed, at the 1080p scale; only the kill feed's part is read
        const double sar = f->sample_aspect_ratio.num > 0 && f->sample_aspect_ratio.den > 0
                               ? av_q2d(f->sample_aspect_ratio)
                               : (info.sar.num > 0 && info.sar.den > 0 ? av_q2d(info.sar) : 1.0);
        const double cw = f->width * content.width(), chh = f->height * content.height();
        pw = std::max(2, int(std::lround(cw * sar * kScale / std::max(1.0, chh))));
        g = geometryOf(game, pw, ph);
        area = areaOf(g, pw, ph);
        if (area.width() < 16 || area.height() < 16) return fail(QStringLiteral("picture too small"));
        // The area in the frame's pixels
        const double kx = cw / pw, ky = chh / ph;
        AVFrame* cropped = av_frame_clone(f);
        if (!cropped) break;
        const std::unique_ptr<AVFrame, void (*)(AVFrame*)> keep(cropped, [](AVFrame* x) { av_frame_free(&x); });
        const long l = std::lround(content.left() * f->width + area.left() * kx);
        const long t = std::lround(content.top() * f->height + area.top() * ky);
        const long r = std::lround(content.left() * f->width + (area.right() + 1) * kx);
        const long b = std::lround(content.top() * f->height + (area.bottom() + 1) * ky);
        cropped->crop_left = size_t(std::clamp(l, 0L, long(f->width) - 2));
        cropped->crop_top = size_t(std::clamp(t, 0L, long(f->height) - 2));
        cropped->crop_right = size_t(std::clamp(long(f->width) - r, 0L, long(f->width) - long(cropped->crop_left) - 2));
        cropped->crop_bottom = size_t(std::clamp(long(f->height) - b, 0L, long(f->height) - long(cropped->crop_top) - 2));
        if (av_frame_apply_cropping(cropped, AV_FRAME_CROP_UNALIGNED) < 0) break;
        const int aw = area.width(), ah = area.height();
        sws.reset(sws_getCachedContext(sws.release(), cropped->width, cropped->height, AVPixelFormat(cropped->format), aw, ah,
                                   AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!sws) break;
        // Rows aligned and room after the last: swscale writes whole SIMD blocks
        const int stride = (aw * 3 + 63) & ~63;
        rgb.resize(size_t(stride) * size_t(ah + 1) + 64);
        uint8_t* planes[4] = {rgb.data(), nullptr, nullptr, nullptr};
        int strides[4] = {stride, 0, 0, 0};
        sws_scale(sws.get(), cropped->data, cropped->linesize, 0, cropped->height, planes, strides);
        View v;
        v.bits = rgb.data();
        v.stride = stride;
        v.ox = area.x();
        v.oy = area.y();
        v.w = aw;
        v.h = ah;
        v.pw = pw;
        v.ph = ph;
        Sample s = detectView(v, g);
        s.seconds = double(pic->position) / srcFps;
        if (out.isEmpty() || s.seconds > out.last().seconds + 1e-6) out << s;
        done = std::max(pos, int64_t(pic->position)); // positions the file skips
        if (progress) progress(int(std::min<int64_t>(999, (done - first + 1) * 1000 / (last - first + 1))));
    }
    sws.reset();
    reader.reset();
    if (cancel && cancel->load()) return fail(T("Abgebrochen."));
    if (out.isEmpty()) return fail(T("Kein Bild vom Clip"));
    if (progress) progress(1000);
    if (!complete || stale) return out; // a part only: not cached as the whole range

    QDir().mkpath(cacheDir());
    const QString target = QDir(cacheDir()).filePath(QString("%1_%2_%3%4")
                                                         .arg(base(file, content, game.id))
                                                         .arg(qint64(std::llround(from * 1000)))
                                                         .arg(qint64(std::llround(to * 1000)))
                                                         .arg(kSuffix));
    QSaveFile f(target);
    if (f.open(QIODevice::WriteOnly) && f.write(writeSamples(out)) >= 0) f.commit(); // a cache: failing to store is fine
    return out;
}

Reframe::Feed feedOf(const Clip& c, FrameRate rate, int fileLength, const Samples& samples, const QRectF& contentIn,
                     const GameHud::Game& game)
{
    const QRectF content = BarDetect::hasBars(contentIn) ? contentIn : QRectF(0, 0, 1, 1);
    Reframe::Feed feed;
    feed.measured = true;
    feed.fps = rate.fps() > 0 ? rate.fps() : 25.0;
    feed.lineHeight = game.lineHeight * content.height();
    const RetimeMap map(c, fileLength);
    const auto sample = [&](const Sample& s, double t) {
        Reframe::FeedSample x;
        x.frame = std::max(0.0, t);
        x.file = s.seconds;
        x.lines = s.lines;
        if (s.lines > 0) {
            x.left = content.left() + s.left * content.width();
            x.bottom = content.top() + s.bottom * content.height();
        }
        return x;
    };
    if (c.freeze) {
        // One picture: the sample closest to it (as ReframeAnalysis::targets)
        const double at = map.pictureFrame(c.stillFrame()) / feed.fps;
        const Sample* best = nullptr;
        for (const Sample& s : samples)
            if (!best || std::abs(s.seconds - at) < std::abs(best->seconds - at)) best = &s;
        if (best) feed.samples << sample(*best, 0);
        return feed;
    }
    for (const Sample& s : samples) {
        const double file = s.seconds * feed.fps;
        const double material = c.isRetimed() ? map.materialForPicture(file) : file;
        const double t = material - c.in;
        if (t < -0.5 || t >= c.length()) continue;
        feed.samples << sample(s, t);
    }
    std::stable_sort(feed.samples.begin(), feed.samples.end(),
                     [](const Reframe::FeedSample& a, const Reframe::FeedSample& b) { return a.frame < b.frame; });
    return feed;
}

QHash<int, Reframe::Feed> feeds(const QVector<Clip>& clips, const ProjectFormat& format, Reframe::Mode mode,
                                const QHash<QString, QRectF>& contents, const Reframe::KeepChoice& choice,
                                const std::function<int(const QString&)>& lengthOf, QStringList* errors,
                                const std::atomic<bool>* cancel, const std::function<void(int)>& progress)
{
    struct Job {
        const Clip* clip;
        const GameHud::Game* game;
        ReframeAnalysis::Need need;
        QRectF content;
        int length;
    };
    QVector<Job> jobs;
    const QSizeF frame(format.size());
    for (const Clip& c : clips) {
        const QRectF content = contents.value(c.mediaPath, QRectF(0, 0, 1, 1));
        const QVector<Reframe::Keep> keeps = Reframe::keepsOf(c, mode, frame, content, choice);
        if (std::none_of(keeps.begin(), keeps.end(), [](const Reframe::Keep& k) { return k.id == "killfeed"; })) continue;
        const GameHud::Game* game = choice.game.isEmpty() ? GameHud::guess(c.mediaPath) : GameHud::find(choice.game);
        if (!game || !canMeasure(*game)) continue;
        const int length = lengthOf ? lengthOf(c.mediaPath) : 0;
        const auto n = ReframeAnalysis::need(c, format.rate, length);
        if (!n) continue;
        jobs << Job{&c, game, *n, content, length};
    }
    QHash<int, Reframe::Feed> out;
    qint64 total = 0, done = 0;
    for (const Job& j : jobs) total += std::max(1, j.clip->length());
    for (const Job& j : jobs) {
        if (cancel && cancel->load()) return {};
        QString error;
        const qint64 before = done, weight = std::max(1, j.clip->length());
        const auto samples = measure(j.need.file, j.need.from, j.need.to, j.content, *j.game, &error, cancel, [&](int p) {
            if (progress) progress(int((before * 1000 + weight * p) / std::max<qint64>(1, total)));
        });
        done += weight;
        if (!samples) {
            if (cancel && cancel->load()) return {};
            if (errors) *errors << error;
            continue;
        }
        out.insert(j.clip->id, feedOf(*j.clip, format.rate, j.length, *samples, j.content, *j.game));
    }
    if (progress) progress(1000);
    return out;
}

void attach(QHash<int, QVector<Reframe::Keep>>& keeps, const QHash<int, Reframe::Feed>& feeds)
{
    for (auto it = keeps.begin(); it != keeps.end(); ++it) {
        const auto f = feeds.constFind(it.key());
        if (f == feeds.cend()) continue;
        for (Reframe::Keep& k : it.value())
            if (k.id == "killfeed") k.feed = *f;
    }
}

} // namespace KillFeedLines
