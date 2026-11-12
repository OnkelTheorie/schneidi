// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/Tracker.h"

#include "core/GradeWindows.h"
#include "core/I18n.h"
#include "core/MediaAspect.h"
#include "engine/Snapshot.h"

#include <QPainter>

#include <algorithm>
#include <cmath>

namespace Tracker {

namespace {

constexpr int kPictureEdge = 640;    // longest side of the pictures fetched for tracking
constexpr int kTemplateMax = 48;     // longest template side after scaling (speed)
constexpr int kTemplateMin = 12;
constexpr double kLost = 0.45;       // correlation below = lost
constexpr double kUpdate = 0.8;      // correlation above = template follows slowly (light/shape changes)
constexpr float kMixSlow = 0.1f, kMixFast = 0.25f; // below kUpdate the look changes (other background): follow faster

} // namespace

Gray toGray(const QImage& img)
{
    Gray g;
    if (img.isNull()) return g;
    const QImage rgb = img.convertToFormat(QImage::Format_RGB32);
    g.width = rgb.width();
    g.height = rgb.height();
    g.px.resize(size_t(g.width) * g.height);
    for (int y = 0; y < g.height; ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(rgb.constScanLine(y));
        for (int x = 0; x < g.width; ++x)
            g.px[size_t(y) * g.width + x] =
                (0.2126f * qRed(line[x]) + 0.7152f * qGreen(line[x]) + 0.0722f * qBlue(line[x])) / 255.0f;
    }
    return g;
}

Gray toColor(const QImage& img)
{
    Gray g;
    if (img.isNull()) return g;
    const QImage rgb = img.convertToFormat(QImage::Format_RGB32);
    g.width = rgb.width();
    g.height = rgb.height();
    g.channels = 3;
    g.px.resize(size_t(g.width) * g.height * 3);
    const size_t plane = size_t(g.width) * g.height;
    for (int y = 0; y < g.height; ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(rgb.constScanLine(y));
        for (int x = 0; x < g.width; ++x) {
            const size_t i = size_t(y) * g.width + x;
            g.px[i] = qRed(line[x]) / 255.0f;
            g.px[plane + i] = qGreen(line[x]) / 255.0f;
            g.px[2 * plane + i] = qBlue(line[x]) / 255.0f;
        }
    }
    return g;
}

PointTracker::PointTracker(const Gray& first, const QRectF& region)
{
    if (first.width <= 0 || first.height <= 0) return;
    // Template = the region, kept inside the picture and at least kTemplateMin pixels
    const int tw = std::clamp(int(std::lround(region.width())), kTemplateMin, first.width);
    const int th = std::clamp(int(std::lround(region.height())), kTemplateMin, first.height);
    const int x0 = std::clamp(int(std::lround(region.center().x() - tw / 2.0)), 0, first.width - tw);
    const int y0 = std::clamp(int(std::lround(region.center().y() - th / 2.0)), 0, first.height - th);
    m_tw = tw;
    m_th = th;
    m_channels = first.channels;
    m_t.assign(size_t(tw) * th * m_channels, 0);
    m_w.resize(size_t(tw) * th);
    const double sx = 0.35 * tw, sy = 0.35 * th;
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
            const double u = (x + 0.5 - tw / 2.0) / sx, v = (y + 0.5 - th / 2.0) / sy;
            m_w[size_t(y) * tw + x] = float(std::exp(-0.5 * (u * u + v * v)));
        }
    m_wSum = 0;
    for (float w : m_w) m_wSum += w;
    takeTemplate(first, x0, y0, 1);
    m_center = QPointF(x0 + tw / 2.0, y0 + th / 2.0);
}

void PointTracker::takeTemplate(const Gray& f, double x0, double y0, float mix)
{
    // Sampled at the sub-pixel position of the match (bilinear): taking it at the whole-pixel peak would shift the
    // template a little with every update and the tracked point would drift
    const size_t n = size_t(m_tw) * m_th;
    std::vector<float> patch(n);
    double norm = 0;
    for (int c = 0; c < m_channels; ++c) {
        for (int y = 0; y < m_th; ++y)
            for (int x = 0; x < m_tw; ++x) {
                const double px = std::clamp(x0 + x, 0.0, f.width - 1.0), py = std::clamp(y0 + y, 0.0, f.height - 1.0);
                const int ix = std::min(int(px), std::max(0, f.width - 2)), iy = std::min(int(py), std::max(0, f.height - 2));
                const float fx = float(px - ix), fy = float(py - iy);
                const int ix1 = std::min(ix + 1, f.width - 1), iy1 = std::min(iy + 1, f.height - 1);
                patch[size_t(y) * m_tw + x] = (1 - fy) * ((1 - fx) * f.at(ix, iy, c) + fx * f.at(ix1, iy, c))
                                              + fy * ((1 - fx) * f.at(ix, iy1, c) + fx * f.at(ix1, iy1, c));
            }
        double mean = 0;
        for (size_t i = 0; i < n; ++i) mean += double(m_w[i]) * patch[i];
        mean /= m_wSum;
        float* t = &m_t[c * n];
        double tmean = 0;
        for (size_t i = 0; i < n; ++i) {
            t[i] = (1 - mix) * t[i] + mix * float(patch[i] - mean);
            tmean += double(m_w[i]) * t[i];
        }
        tmean /= m_wSum;
        for (size_t i = 0; i < n; ++i) {
            t[i] -= float(tmean); // keep zero (weighted) mean after mixing
            norm += double(m_w[i]) * t[i] * t[i];
        }
    }
    m_wt.resize(m_t.size());
    for (size_t i = 0; i < m_t.size(); ++i) m_wt[i] = m_w[i % n] * m_t[i];
    m_tNorm = std::sqrt(norm);
}

void PointTracker::relearn(const Gray& f, float mix)
{
    if (!valid() || f.width < m_tw || f.height < m_th || f.channels != m_channels) return;
    takeTemplate(f, m_center.x() - m_tw / 2.0, m_center.y() - m_th / 2.0, std::clamp(mix, 0.0f, 1.0f));
}

double PointTracker::ncc(const Gray& f, int x0, int y0) const
{
    // Weighted correlation; each channel of the template has zero weighted mean, so the picture's mean drops out of
    // the cross term
    double var = 0, cross = 0;
    for (int c = 0; c < m_channels; ++c) {
        double sum = 0, sum2 = 0;
        for (int y = 0; y < m_th; ++y) {
            const float* row = &f.px[(size_t(c) * f.height + y0 + y) * f.width + x0];
            const float* w = &m_w[size_t(y) * m_tw];
            const float* wt = &m_wt[(size_t(c) * m_th + y) * m_tw];
            // A row in float with four partial sums (vectorizes; at most 48 terms, so float is exact enough), the
            // rows in double
            float s[4] = {}, s2[4] = {}, cr[4] = {};
            int x = 0;
            for (; x + 4 <= m_tw; x += 4)
                for (int k = 0; k < 4; ++k) {
                    const float wi = w[x + k] * row[x + k];
                    s[k] += wi;
                    s2[k] += wi * row[x + k];
                    cr[k] += wt[x + k] * row[x + k];
                }
            for (; x < m_tw; ++x) {
                const float wi = w[x] * row[x];
                s[0] += wi;
                s2[0] += wi * row[x];
                cr[0] += wt[x] * row[x];
            }
            sum += double(s[0] + s[1]) + double(s[2] + s[3]);
            sum2 += double(s2[0] + s2[1]) + double(s2[2] + s2[3]);
            cross += double(cr[0] + cr[1]) + double(cr[2] + cr[3]);
        }
        var += sum2 - sum * sum / m_wSum;
    }
    if (var <= 1e-9 || m_tNorm <= 1e-9) return 0; // flat area: nothing to match
    return cross / (std::sqrt(var) * m_tNorm);
}

bool PointTracker::next(const Gray& f, int radius)
{
    if (!valid() || f.width < m_tw || f.height < m_th || f.channels != m_channels) return false;
    // Search around the last position; fast movements up to ~60 % of the template size per frame
    const int r = radius > 0 ? radius : std::max(8, int(0.6 * std::max(m_tw, m_th)));
    const int cx = int(std::lround(m_center.x() - m_tw / 2.0)), cy = int(std::lround(m_center.y() - m_th / 2.0));
    const int xa = std::max(0, cx - r), xb = std::min(f.width - m_tw, cx + r);
    const int ya = std::max(0, cy - r), yb = std::min(f.height - m_th, cy + r);
    if (xa > xb || ya > yb) return false;
    const int sw = xb - xa + 1, sh = yb - ya + 1;
    std::vector<double> scores(size_t(sw) * sh);
    double best = -2;
    int bx = 0, by = 0;
    for (int y = ya; y <= yb; ++y)
        for (int x = xa; x <= xb; ++x) {
            const double s = ncc(f, x, y);
            scores[size_t(y - ya) * sw + (x - xa)] = s;
            if (s > best) {
                best = s;
                bx = x;
                by = y;
            }
        }
    m_score = best;
    if (best < kLost) return false;
    // Sub-pixel: parabola through the neighbors of the peak
    const auto score = [&](int x, int y) { return scores[size_t(y - ya) * sw + (x - xa)]; };
    const auto vertex = [](double l, double c, double r) {
        const double d = l - 2 * c + r;
        return std::abs(d) > 1e-12 ? std::clamp(0.5 * (l - r) / d, -0.5, 0.5) : 0.0;
    };
    double dx = 0, dy = 0;
    if (bx > xa && bx < xb) dx = vertex(score(bx - 1, by), best, score(bx + 1, by));
    if (by > ya && by < yb) dy = vertex(score(bx, by - 1), best, score(bx, by + 1));
    m_center = QPointF(bx + dx + m_tw / 2.0, by + dy + m_th / 2.0);
    if (best > kUpdate || m_adaptUnsure) takeTemplate(f, bx + dx, by + dy, best > kUpdate ? kMixSlow : kMixFast);
    return true;
}

TwoPointTracker::TwoPointTracker(const Gray& first, const QRectF& a, const QRectF& b) : m_a(first, a), m_b(first, b)
{
    const QPointF d = m_b.center() - m_a.center();
    m_dist0 = std::hypot(d.x(), d.y());
    m_angle0 = std::atan2(d.y(), d.x());
}

bool TwoPointTracker::next(const Gray& f, int radius)
{
    if (!valid()) return false;
    const QPointF pa = m_a.center(), pb = m_b.center();
    const bool okA = m_a.next(f, radius), okB = m_b.next(f, radius);
    m_outlier = false;
    if (!okA && !okB) {
        m_a.moveTo(pa);
        m_b.moveTo(pb);
        return false;
    }
    const auto measure = [this](QPointF a, QPointF b, double* scale, double* angle) {
        const QPointF d = b - a;
        *scale = std::hypot(d.x(), d.y()) / m_dist0;
        double turn = (std::atan2(d.y(), d.x()) - m_angle0) * 180 / M_PI;
        // Continue from the last angle (no jump at +-180)
        turn -= 360 * std::round((turn - m_angle) / 360);
        *angle = turn;
    };
    double scale = m_scale, angle = m_angle;
    bool good = okA && okB;
    if (good) {
        measure(m_a.center(), m_b.center(), &scale, &angle);
        good = std::abs(scale / m_scale - 1) <= kMaxScaleStep && std::abs(angle - m_angle) <= kMaxTurnStep;
    }
    if (good) {
        m_scale = scale;
        m_angle = angle;
        return true;
    }
    // Outlier: the better point leads, the other one follows it with size and rotation kept
    m_outlier = true;
    const bool leadA = okA && (!okB || m_a.score() >= m_b.score());
    if (leadA) m_b.moveTo(pb + (m_a.center() - pa));
    else m_a.moveTo(pa + (m_b.center() - pb));
    return true;
}

namespace {

// The affine map taking a0 a1 a2 onto b0 b1 b2 applied to p; false when the a's lie on a line
bool affineFrom3(const QPointF a[3], const QPointF b[3], QPointF p, QPointF* out)
{
    // p = a0 + s (a1 - a0) + u (a2 - a0) -> b0 + s (b1 - b0) + u (b2 - b0)
    const QPointF e1 = a[1] - a[0], e2 = a[2] - a[0], d = p - a[0];
    const double det = e1.x() * e2.y() - e1.y() * e2.x();
    if (std::abs(det) < 1e-9) return false;
    const double sv = (d.x() * e2.y() - d.y() * e2.x()) / det, uv = (e1.x() * d.y() - e1.y() * d.x()) / det;
    *out = b[0] + sv * (b[1] - b[0]) + uv * (b[2] - b[0]);
    return true;
}

// The similarity (shift, size, turn) taking a0 a1 onto b0 b1 applied to p
QPointF similarityFrom2(QPointF a0, QPointF a1, QPointF b0, QPointF b1, QPointF p)
{
    const QPointF da = a1 - a0, db = b1 - b0;
    const double la = da.x() * da.x() + da.y() * da.y();
    if (la < 1e-12) return p + (b0 - a0);
    // Complex division db / da
    const double re = (db.x() * da.x() + db.y() * da.y()) / la, im = (db.y() * da.x() - db.x() * da.y()) / la;
    const QPointF d = p - a0;
    return b0 + QPointF(re * d.x() - im * d.y(), im * d.x() + re * d.y());
}

} // namespace

QuadTracker::QuadTracker(const Gray& first, const QRectF regions[4])
{
    m_points.reserve(4);
    double side = 0;
    for (int i = 0; i < 4; ++i) {
        m_points.emplace_back(first, regions[i]);
        m_start[i] = m_points.back().center();
        side = std::max({side, double(m_points.back().templateWidth()), double(m_points.back().templateHeight())});
    }
    m_tolerance = std::max(kMinError, kErrorShare * side);
}

bool QuadTracker::valid() const
{
    if (m_points.size() != 4) return false;
    for (const PointTracker& p : m_points)
        if (!p.valid()) return false;
    double from[8], to[8];
    for (int i = 0; i < 4; ++i) from[2 * i] = to[2 * i] = m_start[i].x(), from[2 * i + 1] = to[2 * i + 1] = m_start[i].y();
    Compose::Homography h;
    return Compose::homography(from, to, &h); // a quad (no three on a line, not crossed)
}

double QuadTracker::score() const
{
    double s = 1;
    for (const PointTracker& p : m_points) s = std::min(s, p.score());
    return s;
}

bool QuadTracker::next(const Gray& f, int radius)
{
    if (m_points.size() != 4) return false;
    QPointF prev[4], now[4];
    bool good[4];
    int n = 0;
    for (int i = 0; i < 4; ++i) {
        prev[i] = m_points[size_t(i)].center();
        // Searched where the last frame's movement carries it (a camera moving fast: the search window stays on it)
        if (m_moved) m_points[size_t(i)].moveTo(prev[i] + m_step[i]);
        good[i] = m_points[size_t(i)].next(f, radius);
        now[i] = m_points[size_t(i)].center();
        n += good[i];
    }
    // All four found: each one against where the frame's movement of the other three puts it. One point off moves the
    // others' predictions as much as its own (any three fit an affine map), so the error says only that one is off:
    // which one is the point moving most unlike the others (against the median movement)
    if (n == 4) {
        double worst = 0;
        for (int i = 0; i < 4; ++i) {
            QPointF a[3], b[3], pred;
            for (int j = 0, k = 0; j < 4; ++j)
                if (j != i) a[k] = prev[j], b[k++] = now[j];
            if (affineFrom3(a, b, prev[i], &pred)) worst = std::max(worst, std::hypot(now[i].x() - pred.x(), now[i].y() - pred.y()));
        }
        if (worst > m_tolerance) {
            double mx[4], my[4];
            for (int i = 0; i < 4; ++i) mx[i] = now[i].x() - prev[i].x(), my[i] = now[i].y() - prev[i].y();
            std::sort(mx, mx + 4);
            std::sort(my, my + 4);
            const QPointF median((mx[1] + mx[2]) / 2, (my[1] + my[2]) / 2);
            int out = 0;
            double far = -1;
            for (int i = 0; i < 4; ++i) {
                const QPointF d = now[i] - prev[i] - median;
                // Ties (a symmetric move): the weaker match
                const double v = std::hypot(d.x(), d.y()) + 1e-3 * (1 - m_points[size_t(i)].score());
                if (v > far) {
                    far = v;
                    out = i;
                }
            }
            good[out] = false;
            n = 3;
        }
    }
    m_outliers = 4 - n;
    // The four agree: the surface turns and grows under the templates (a camera walking up to a sign), so the points
    // the others confirm learn their new look faster than a lone point may (it could learn what hides it). A match
    // already weak (something in front) learns nothing
    for (int i = 0; i < 4; ++i)
        if (good[i] && n >= 3 && m_points[size_t(i)].score() >= kLearn) m_points[size_t(i)].relearn(f, kLearnMix);
    if (n < 2) {
        for (int i = 0; i < 4; ++i) m_points[size_t(i)].moveTo(prev[i]);
        m_moved = false; // lost: no movement to go on from
        return false;
    }
    // The points not used follow the good ones
    if (n < 4) {
        int idx[4], k = 0;
        for (int i = 0; i < 4; ++i)
            if (good[i]) idx[k++] = i;
        for (int i = 0; i < 4; ++i) {
            if (good[i]) continue;
            QPointF p;
            bool ok = false;
            if (n == 3) {
                const QPointF a[3] = {prev[idx[0]], prev[idx[1]], prev[idx[2]]};
                const QPointF b[3] = {now[idx[0]], now[idx[1]], now[idx[2]]};
                ok = affineFrom3(a, b, prev[i], &p);
            }
            if (!ok) p = similarityFrom2(prev[idx[0]], prev[idx[1]], now[idx[0]], now[idx[1]], prev[i]);
            m_points[size_t(i)].moveTo(p);
        }
    }
    double from[8], to[8];
    for (int i = 0; i < 4; ++i) {
        from[2 * i] = m_start[i].x(), from[2 * i + 1] = m_start[i].y();
        to[2 * i] = point(i).x(), to[2 * i + 1] = point(i).y();
        m_step[i] = point(i) - prev[i];
    }
    m_moved = true;
    Compose::Homography h;
    if (Compose::homography(from, to, &h)) m_h = h; // folded (a bad frame): the last one stays
    return true;
}

double cornerStrength(const Gray& g, QPointF center, double side)
{
    const int half = std::max(2, int(std::lround(side / 2)));
    const int cx = int(std::lround(center.x())), cy = int(std::lround(center.y()));
    double a = 0, b = 0, c = 0;
    int n = 0;
    for (int y = std::max(1, cy - half); y <= std::min(g.height - 2, cy + half); ++y)
        for (int x = std::max(1, cx - half); x <= std::min(g.width - 2, cx + half); ++x) {
            double gx = 0, gy = 0;
            for (int ch = 0; ch < g.channels; ++ch) {
                gx += (g.at(x + 1, y, ch) - g.at(x - 1, y, ch)) / 2;
                gy += (g.at(x, y + 1, ch) - g.at(x, y - 1, ch)) / 2;
            }
            gx /= g.channels;
            gy /= g.channels;
            a += gx * gx;
            b += gx * gy;
            c += gy * gy;
            ++n;
        }
    if (n == 0) return 0;
    a /= n, b /= n, c /= n;
    return (a + c) / 2 - std::sqrt((a - c) * (a - c) / 4 + b * b);
}

std::function<QImage(int, int)> clipFrames(const ProjectFormat& format, const Clip& c)
{
    // The clip alone, without what the Color page and Inspector do to it, starting at frame 0
    Clip plain = c;
    plain.start = 0;
    plain.effects.clear();
    plain.keys.clear();
    plain.transform = ClipTransform{};
    plain.transIn = plain.transOut = plain.fadeIn = plain.fadeOut = 0;
    plain.renderCache = false;
    plain.enabled = true;
    plain.linkId = 0;
    Timeline tl;
    tl.video.resize(1);
    tl.video[0].clips << plain;
    std::shared_ptr<Snapshot::Source> source = Snapshot::Source::timeline(format, tl);
    return [source](int frame, int maxEdge) { return source->frame(frame, maxEdge); };
}

QRectF pictureRect(const ProjectFormat& format, const Clip& c)
{
    const QSizeF frame(format.width, format.height);
    const QSizeF fit = MediaAspect::fit(MediaAspect::ofClip(c), frame);
    return QRectF((frame.width() - fit.width()) / 2, (frame.height() - fit.height()) / 2, fit.width(), fit.height());
}

std::function<QImage(int, int)> clipPictures(const ProjectFormat& format, const Clip& c)
{
    const auto frames = clipFrames(format, c);
    const QRectF r = pictureRect(format, c);
    const bool whole = r.width() >= format.width - 0.5 && r.height() >= format.height - 0.5;
    if (whole || format.width <= 0 || format.height <= 0) return frames;
    const QRectF part(r.x() / format.width, r.y() / format.height, r.width() / format.width, r.height() / format.height);
    return [frames, part](int frame, int maxEdge) {
        const QImage img = frames(frame, maxEdge);
        if (img.isNull()) return img;
        // Sub-pixel exact (a whole-pixel cut is off by up to a pixel and stretches the picture a little)
        const QRectF from(part.x() * img.width(), part.y() * img.height(), part.width() * img.width(),
                          part.height() * img.height());
        const QSize size(std::max(1, int(std::lround(from.width()))), std::max(1, int(std::lround(from.height()))));
        if (from.width() < 1 || from.height() < 1) return QImage();
        QImage out(size, QImage::Format_RGB32);
        out.fill(Qt::black);
        QPainter p(&out);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(QRectF(QPointF(0, 0), QSizeF(size)), img, from);
        p.end();
        return out;
    };
}

Result trackWindow(const ProjectFormat& format, const Clip& c, int n, int from, int to,
                   const std::function<void(int, int)>& progress, const std::atomic<bool>* cancel)
{
    Result res;
    if (!GradeWindows::exists(c, n)) {
        res.error = T("Der Clip hat kein Fenster %1").arg(n);
        return res;
    }
    if (c.isNested() || c.isTitle()) {
        res.error = T("Tracking geht nur bei Video-Clips (nicht bei Titeln, Verbund- und Multicam-Clips)");
        return res;
    }
    from = std::clamp(from, 0, c.length() - 1);
    to = std::clamp(to, 0, c.length() - 1);
    if (from == to) return res;

    const auto source = clipFrames(format, c);
    const QImage first = source(from, kPictureEdge);
    if (first.isNull()) {
        res.error = T("Kein Bild vom Clip");
        return res;
    }

    const GradeWindows::Geometry g = GradeWindows::geometryAt(c, n, from);
    const double W = first.width(), H = first.height();
    // Window area (bounding box of the rotated shape) in picture pixels; large windows are tracked on a smaller picture
    const double rad = g.rotation * M_PI / 180;
    const double ww = g.width / 100 * W, wh = g.height / 100 * H;
    const double bw = std::abs(ww * std::cos(rad)) + std::abs(wh * std::sin(rad));
    const double bh = std::abs(ww * std::sin(rad)) + std::abs(wh * std::cos(rad));
    const double scale = std::min(1.0, kTemplateMax / std::max({bw, bh, 1.0}));
    const auto gray = [&](const QImage& img) {
        return toColor(scale < 1 ? img.scaled(std::max(1, int(std::lround(W * scale))), std::max(1, int(std::lround(H * scale))),
                                             Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                                : img);
    };
    const QPointF start(g.x / 100 * W * scale, g.y / 100 * H * scale);
    PointTracker tracker(gray(first), QRectF(start.x() - bw * scale / 2, start.y() - bh * scale / 2, bw * scale, bh * scale));
    if (!tracker.valid()) {
        res.error = T("Kein Bild vom Clip");
        return res;
    }
    const QPointF origin = tracker.center(); // the window moves by what the template moves
    const int step = to > from ? 1 : -1, total = std::abs(to - from);
    for (int f = from + step, done = 1; done <= total; f += step, ++done) {
        if (cancel && cancel->load()) {
            res.canceled = true;
            break;
        }
        const QImage img = source(f, kPictureEdge);
        if (img.isNull() || !tracker.next(gray(img))) {
            res.lost = true;
            res.lostAt = f;
            break;
        }
        const QPointF d = tracker.center() - origin;
        res.points << Point{f, g.x + d.x() / (W * scale) * 100, g.y + d.y() / (H * scale) * 100};
        if (progress) progress(done, total);
    }
    return res;
}

} // namespace Tracker
