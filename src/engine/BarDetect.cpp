// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/BarDetect.h"

#include "core/I18n.h"
#include "engine/video/Reader.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QImageReader>
#include <QMutex>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace BarDetect {

namespace {

constexpr int kVersion = 4; // bump when the measurement changes
constexpr int kW = 320, kH = 240;
constexpr int kSamples = 12;
constexpr int kBlack = 32;          // gray value still counted as black (limited-range black is 16, plus noise)
constexpr double kBrightShare = 0.04; // a line with fewer bright pixels than this in every sample is a bar
constexpr double kMinBar = 0.015;
constexpr double kMaxBar = 0.3;    // wider (a 4:3 game in 21:9 has 21 %) is a dark picture, not a bar
constexpr double kSymmetry = 0.02; // left and right bar (top and bottom) differ by at most this share
// A bar is flat: its inner lines as dark as its outer ones (a vignette or a dark edge of the picture gets brighter
// towards the middle; measured: a dark blue vignette rose by 4-5 gray values over the 15 dark columns it had), and
// each of its lines keeps its brightness over the samples (the picture's edge changes with the scene)
constexpr double kBarSlope = 2.5;  // median gray value: innermost minus outermost line, in every sample
constexpr double kBarDrift = 4;    // median gray value of a bar line: brightest minus darkest sample

QMutex g_mutex;
QHash<QString, QRectF> g_memory;

QString base(const QString& file)
{
    const QFileInfo fi(file);
    const QString id = QString("%1|%2|%3|%4")
                           .arg(kVersion)
                           .arg(fi.absoluteFilePath())
                           .arg(fi.size())
                           .arg(fi.lastModified().toMSecsSinceEpoch());
    return QString::fromLatin1(QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha1).toHex());
}

QString cacheFile(const QString& file) { return QDir(cacheDir()).filePath(base(file) + ".bars"); }

// Gray kW x kH pictures -> content rect
QRectF measure(const std::vector<std::vector<uint8_t>>& pics)
{
    if (pics.empty()) return {0, 0, 1, 1};
    // Per sample and line: share of bright pixels; a line is a bar if it stays below kBrightShare in every sample
    std::vector<bool> barCol(kW, true), barRow(kH, true);
    bool anyBright = false;
    for (const auto& p : pics) {
        for (int x = 0; x < kW; ++x) {
            int bright = 0;
            for (int y = 0; y < kH; ++y) bright += p[size_t(y) * kW + size_t(x)] > kBlack;
            if (bright >= kBrightShare * kH) barCol[size_t(x)] = false, anyBright = true;
        }
    }
    if (!anyBright) return {0, 0, 1, 1}; // dark all through: nothing to tell bars from
    int left = 0, right = 0;
    while (left < kW && barCol[size_t(left)]) ++left;
    while (right < kW - left && barCol[size_t(kW - 1 - right)]) ++right;
    if (left + right >= kW * 9 / 10) return {0, 0, 1, 1};
    // Flat bars only (n lines from the outer line `outer` inwards by `step`, `column` = columns)
    // Median gray value of a line: an overlay in the bar (a webcam, a logo; less than half of it) does not count
    std::vector<uint8_t> line;
    const auto lineLevel = [&](const std::vector<uint8_t>& p, bool column, int i, int from, int to) {
        line.clear();
        for (int k = from; k < to; ++k) line.push_back(column ? p[size_t(k) * kW + size_t(i)] : p[size_t(i) * kW + size_t(k)]);
        if (line.empty()) return 0.0;
        std::nth_element(line.begin(), line.begin() + std::ptrdiff_t(line.size() / 2), line.end());
        return double(line[line.size() / 2]);
    };
    const auto flat = [&](bool column, int n, int outer, int step, int from, int to) {
        if (n <= 0) return true;
        // The innermost bar line may be mixed with the picture by the shrinking: one further out
        const int inner = outer + step * std::max(0, n - 2);
        for (int k = 0; k < n; ++k) {
            double lo = 255, hi = 0;
            for (const auto& p : pics) {
                const double m = lineLevel(p, column, outer + step * k, from, to);
                lo = std::min(lo, m), hi = std::max(hi, m);
            }
            if (hi - lo > kBarDrift) return false;
        }
        for (const auto& p : pics)
            if (lineLevel(p, column, inner, from, to) - lineLevel(p, column, outer, from, to) > kBarSlope) return false;
        return true;
    };
    if (!flat(true, left, 0, 1, 0, kH) || !flat(true, right, kW - 1, -1, 0, kH)) left = right = 0;
    // Rows only over the content columns (the bars beside would hide a letterbox)
    for (const auto& p : pics)
        for (int y = 0; y < kH; ++y) {
            int bright = 0;
            for (int x = left; x < kW - right; ++x) bright += p[size_t(y) * kW + size_t(x)] > kBlack;
            if (bright >= kBrightShare * (kW - left - right)) barRow[size_t(y)] = false;
        }
    int top = 0, bottom = 0;
    while (top < kH && barRow[size_t(top)]) ++top;
    while (bottom < kH - top && barRow[size_t(kH - 1 - bottom)]) ++bottom;
    if (top + bottom >= kH * 9 / 10) top = bottom = 0;
    if (!flat(false, top, 0, 1, left, kW - right) || !flat(false, bottom, kH - 1, -1, left, kW - right)) top = bottom = 0;
    // The first line of the picture may be mixed with the bar (the shrinking averages a bar edge inside a line):
    // clearly darker than the line after it in the samples where that one is bright, it goes too
    const auto mean = [&](const std::vector<uint8_t>& p, bool column, int i) {
        double sum = 0;
        const int n = column ? kH : kW;
        for (int k = 0; k < n; ++k) sum += column ? p[size_t(k) * kW + size_t(i)] : p[size_t(i) * kW + size_t(k)];
        return sum / n;
    };
    const auto mixed = [&](bool column, int line, int next) {
        double a = 0, b = 0;
        for (const auto& p : pics) {
            const double m = mean(p, column, next);
            if (m > kBlack) a += mean(p, column, line), b += m;
        }
        return b > 0 && a < 0.8 * b;
    };
    // Thin edges are an encoder's padding, not bars
    const auto edge = [&](int n, int side, bool column, int line, int next) {
        if (n < kMinBar * side) return 0.0;
        return double(n + (mixed(column, line, next) ? 1 : 0)) / side;
    };
    double l = edge(left, kW, true, left, left + 1), r = edge(right, kW, true, kW - 1 - right, kW - 2 - right);
    double t = edge(top, kH, false, top, top + 1), b = edge(bottom, kH, false, kH - 1 - bottom, kH - 2 - bottom);
    // Bars are centred (a picture fitted into the frame); black on one side only is the picture itself (a dark
    // scene, a small bright object on a black ground)
    if (std::abs(l - r) > kSymmetry || l > kMaxBar) l = r = 0;
    if (std::abs(t - b) > kSymmetry || t > kMaxBar) t = b = 0;
    return QRectF(l, t, std::max(0.05, 1 - l - r), std::max(0.05, 1 - t - b));
}

std::optional<QRectF> readCache(const QString& path)
{
    QFile f(cacheFile(path));
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QList<QByteArray> v = f.readAll().trimmed().split(' ');
    if (v.size() != 4) return std::nullopt;
    double n[4];
    for (int i = 0; i < 4; ++i) {
        bool ok = false;
        n[i] = v[i].toDouble(&ok);
        if (!ok) return std::nullopt;
    }
    return QRectF(n[0], n[1], n[2], n[3]);
}

} // namespace

QString cacheDir() { return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/bars"; }

bool hasBars(const QRectF& c) { return c.left() > 1e-4 || c.top() > 1e-4 || c.right() < 1 - 1e-4 || c.bottom() < 1 - 1e-4; }

std::optional<QRectF> cached(const QString& path)
{
    const QString key = base(path);
    {
        QMutexLocker lock(&g_mutex);
        if (const auto it = g_memory.constFind(key); it != g_memory.cend()) return *it;
    }
    const auto r = readCache(path);
    if (r) {
        QMutexLocker lock(&g_mutex);
        g_memory.insert(key, *r);
    }
    return r;
}

std::optional<QRectF> content(const QString& path, QString* error, const std::atomic<bool>* cancel)
{
    if (auto c = cached(path)) return c;
    auto fail = [&](const QString& msg) -> std::optional<QRectF> {
        if (error) *error = msg;
        return std::nullopt;
    };
    if (!QFileInfo::exists(path)) return fail(T("Datei nicht gefunden: %1").arg(path));
    std::vector<std::vector<uint8_t>> pics;
    Video::Reader::Options ro;
    ro.hardware = false;
    ro.behind = 0;
    std::unique_ptr<Video::Reader> reader = Video::Reader::open(path, ro);
    if (reader) {
        reader->setFrameRate(AVRational{25, 1});
        const Video::Info& info = reader->info();
        const double fps = info.frameRate.num > 0 && info.frameRate.den > 0 ? av_q2d(info.frameRate) : 25.0;
        const int64_t frames = info.duration > 0 ? int64_t(double(info.duration) / AV_TIME_BASE * fps) : 0;
        SwsContext* sws = nullptr;
        QSet<int64_t> seen;
        for (int i = 0; i < kSamples; ++i) {
            if (cancel && cancel->load()) break;
            // Spread over the file, away from its first and last moments (fades, a black start)
            const int64_t pos = frames > 1 ? int64_t((0.05 + 0.9 * (i + 0.5) / kSamples) * double(frames - 1)) : 0;
            // Any picture near there will do: the key frame a seek lands on (no decoding up to the exact one). The
            // same picture again (key frames far apart): skipped once there are enough, else decoded up to the
            // position (a short file with a single key frame)
            Video::Reader::Fetch fetch;
            fetch.nearest = true;
            auto pic = reader->picture(pos, fetch);
            if (pic && seen.contains(pic->position)) {
                if (seen.size() >= kSamples / 2) continue;
                pic = reader->picture(pos);
            }
            if (pic) seen.insert(pic->position);
            if (!pic || !pic->frame) {
                if (frames <= 1) break;
                continue;
            }
            const AVFrame* f = pic->frame;
            sws = sws_getCachedContext(sws, f->width, f->height, AVPixelFormat(f->format), kW, kH, AV_PIX_FMT_GRAY8,
                                       SWS_AREA, nullptr, nullptr, nullptr);
            if (!sws) break;
            std::vector<uint8_t> gray(size_t(kW) * kH);
            uint8_t* planes[4] = {gray.data(), nullptr, nullptr, nullptr};
            int strides[4] = {kW, 0, 0, 0};
            sws_scale(sws, f->data, f->linesize, 0, f->height, planes, strides);
            pics.push_back(std::move(gray));
            if (frames <= 1) break; // a still picture
        }
        sws_freeContext(sws);
    } else {
        QImageReader ir(path);
        ir.setAutoTransform(true);
        const QImage img = ir.read();
        if (img.isNull()) return fail(T("Datei lässt sich nicht öffnen: %1").arg(path));
        const QImage g = img.convertToFormat(QImage::Format_Grayscale8).scaled(kW, kH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        std::vector<uint8_t> gray(size_t(kW) * kH);
        for (int y = 0; y < kH; ++y) std::copy_n(g.constScanLine(y), kW, gray.begin() + std::ptrdiff_t(y) * kW);
        pics.push_back(std::move(gray));
    }
    if (cancel && cancel->load()) return fail(T("Abgebrochen."));
    if (pics.empty()) return fail(T("Kein Bild vom Clip"));
    const QRectF out = measure(pics);
    {
        QMutexLocker lock(&g_mutex);
        g_memory.insert(base(path), out);
    }
    QDir().mkpath(cacheDir());
    QSaveFile f(cacheFile(path));
    if (f.open(QIODevice::WriteOnly)
        && f.write(QByteArray::number(out.x(), 'f', 5) + ' ' + QByteArray::number(out.y(), 'f', 5) + ' '
                   + QByteArray::number(out.width(), 'f', 5) + ' ' + QByteArray::number(out.height(), 'f', 5) + '\n')
               >= 0)
        f.commit(); // a cache: failing to store is fine
    return out;
}

} // namespace BarDetect
