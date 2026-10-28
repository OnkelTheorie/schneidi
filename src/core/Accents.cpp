// SPDX-License-Identifier: AGPL-3.0-only
#include "core/Accents.h"

#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "core/Keyframes.h"

#include <QSet>

#include <algorithm>
#include <cmath>
#include <functional>

namespace Accents {

const QVector<Preset>& presets()
{
    // Strengths measured on a 1080x1920 gameplay edit: +1.2 stops read as "mild", ±30 px as "visible but mild"
    static const QVector<Preset> list{
        {"punch", N_("Zoom-Punch"), "zoom punch: 15 % bigger on the hit, lands back in 0.25 s", {0.15, 0, 0, 0.25}},
        {"flash", N_("Blitz"), "exposure flash: +2 stops on the hit, gone in 0.2 s", {0, 2.0, 0, 0.2}},
        {"shake", N_("Erschütterung"),
         "camera shake: jumps of up to 3 % of the frame that die down in 0.25 s (on a clip that just fills the frame "
         "the edge shows: zoom it a little, combine with a punch, or use fit:'blur')",
         {0, 0, 0.03, 0.25}},
        // Users found flash + shake on every kill too much (three times): the everyday hit is mild, the flash belongs
        // to the one big moment ("big") or comes when asked for
        {"hit", N_("Treffer"), "kill hit, mild: punch 10 % + a light shake 1 %, 0.3 s (no flash)", {0.10, 0, 0.01, 0.3}},
        {"big", N_("Großer Treffer"),
         "the one big moment (ace, clutch): punch 12 % + flash +1.5 stops + shake 2 %, 0.3 s - once per video, not on "
         "every kill",
         {0.12, 1.5, 0.02, 0.3}},
        {"drop", N_("Drop"), "music drop: big punch 25 % + flash +2.5 stops, fades in 0.5 s", {0.25, 2.5, 0, 0.5}},
        {"pulse", N_("Puls"), "beat pulse: a small 5 % zoom bump in 0.15 s, for many beats in a row", {0.05, 0, 0, 0.15}},
    };
    return list;
}

const Preset* find(const QString& id)
{
    for (const Preset& p : presets())
        if (id == QLatin1String(p.id)) return &p;
    return nullptr;
}

namespace {

// One hit in clip frames: rises from t0 to t, fades until `end` (may lie beyond the clip)
struct Hit {
    int t0, t, end;
};

// Share of the hit's strength at clip frame x: linear rise, then falling fast and landing slowly - the curve
// between a keyframe and the next one with Ease In (Keys: "arrive slowly"), so the written keyframes draw it exactly
double envelope(const Hit& h, double x)
{
    if (x < h.t0 || x >= h.end) return 0;
    if (x < h.t) return (x - h.t0) / double(h.t - h.t0);
    return 1 - std::sin((x - h.t) / double(h.end - h.t) * M_PI / 2);
}

// Shake: offsets (share of the amplitude) every `step` frames from the hit on, dying down, in a fixed pattern so
// that the same edit always looks the same; linear in between
constexpr double kShakeX[] = {1.0, -0.8, 0.9, -0.6, 0.75, -0.5, 0.6, -0.4};
constexpr double kShakeY[] = {-0.6, 0.9, -0.7, 1.0, -0.5, 0.65, -0.45, 0.5};

struct ShakePoint {
    int t;
    double x, y;
};

QVector<ShakePoint> shakePoints(const Hit& h, int step)
{
    QVector<ShakePoint> out{{h.t0, 0, 0}};
    const int n = std::max(2, (h.end - h.t) / step);
    for (int k = 0; k < n; ++k) {
        const int t = h.t + k * step;
        if (t >= h.end) break;
        const double a = 1.0 - double(k) / n;
        out.append({t, a * kShakeX[k % 8], a * kShakeY[k % 8]});
    }
    out.append({h.end, 0, 0});
    return out;
}

ShakePoint shakeAt(const QVector<ShakePoint>& pts, double x)
{
    if (pts.isEmpty() || x <= pts.first().t || x >= pts.last().t) return {0, 0, 0};
    for (int i = 1; i < pts.size(); ++i) {
        if (x > pts[i].t) continue;
        const ShakePoint& a = pts[i - 1];
        const ShakePoint& b = pts[i];
        const double u = b.t > a.t ? (x - a.t) / double(b.t - a.t) : 1.0;
        return {0, a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u};
    }
    return {0, 0, 0};
}

// Keyframes of p at `times` (clip frames, inside the clip): value(base value there, t); keyframes the clip had inside a
// hit move with it. New keyframes in `landing` get Ease In (the slow end of a punch/flash).
void write(Clip& c, const Clip& before, AnimParam p, QVector<int> times, const QVector<Hit>& hits,
           const std::function<double(double, int)>& value, const QSet<int>& landing)
{
    for (int k : Keys::keyTimes(before, {p}))
        if (std::any_of(hits.begin(), hits.end(), [&](const Hit& h) { return k > h.t0 && k < h.end; })) times << k;
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    double lo = 0, hi = 0;
    Keys::range(p, &lo, &hi);
    for (int t : times) {
        if (t < 0 || t >= c.length()) continue;
        double v = value(Keys::valueAt(before, p, t), t);
        if (lo < hi) v = std::clamp(v, lo, hi);
        const bool existed = Keys::keyAt(before, p, t);
        Keys::setKey(c, p, t, v);
        if (!existed && landing.contains(t)) Keys::setEase(c, {t}, KeyEase::EaseIn, {p});
    }
}

// The hits at clip frames `at` that lie inside a clip of `length` frames, sorted
QVector<Hit> hitsIn(const QVector<int>& at, int length, int rise, int fade)
{
    QVector<Hit> hits;
    for (int t : at)
        if (t >= 0 && t < length) hits.append({std::max(0, t - rise), t, t + fade});
    std::sort(hits.begin(), hits.end(), [](const Hit& x, const Hit& y) { return x.t < y.t; });
    return hits;
}

// Keyframe times of the hits: start, peak and landing (cut at the clip's last frame); the landings into `landing`
QVector<int> keyTimes(const QVector<Hit>& hits, int length, QSet<int>* landing)
{
    QVector<int> times;
    for (const Hit& h : hits) {
        const int end = std::min(h.end, length - 1);
        times << h.t0 << h.t << end;
        landing->insert(end);
    }
    return times;
}

double sum(const QVector<Hit>& hits, int t, double amount)
{
    double s = 0;
    for (const Hit& h : hits) s += amount * envelope(h, t);
    return s;
}

} // namespace

QVector<int> flashHits(QVector<int> hits, double fps)
{
    std::sort(hits.begin(), hits.end());
    hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
    const int gap = std::max(1, int(std::lround(std::max(1.0, fps) * kFlashGap)));
    QVector<int> out;
    for (int t : hits)
        if (out.isEmpty() || t - out.last() >= gap) out << t;
    return out;
}

int apply(Clip& c, const QVector<int>& hitTimes, const Amounts& a, double fps, QSizeF frame)
{
    if (!a.any() || c.length() <= 0) return 0;
    fps = std::max(1.0, fps);
    const int rise = std::max(1, int(std::lround(fps * 0.03)));
    const int fade = std::max(2, int(std::lround(std::max(0.0, a.length) * fps)));
    const QVector<Hit> hits = hitsIn(hitTimes, c.length(), rise, fade);
    if (hits.isEmpty()) return 0;
    const Clip before = c;
    QSet<int> landing;
    const QVector<int> times = keyTimes(hits, c.length(), &landing);
    if (a.punch != 0)
        for (AnimParam p : {AnimParam::ZoomX, AnimParam::ZoomY})
            write(c, before, p, times, hits, [&](double base, int t) { return base * (1 + sum(hits, t, a.punch)); },
                  landing);
    // Spaced on all hits, also those outside this clip: a burst across a cut flashes once too
    const QVector<Hit> flashes = a.flash != 0 ? hitsIn(flashHits(hitTimes, fps), c.length(), rise, fade) : QVector<Hit>{};
    if (!flashes.isEmpty()) {
        if (!EffectRegistry::has(c, "grade")) EffectRegistry::add(c, "grade");
        QSet<int> flashLanding;
        write(c, before, AnimParam::GradeExposure, keyTimes(flashes, c.length(), &flashLanding), flashes,
              [&](double base, int t) { return base + sum(flashes, t, a.flash); }, flashLanding);
    }
    if (a.shake != 0) {
        const int step = std::max(1, int(std::lround(fps / 30)));
        const double amplitude = a.shake * std::min(frame.width(), frame.height());
        QVector<QVector<ShakePoint>> points;
        QVector<int> shakeTimes;
        for (const Hit& h : hits) {
            points << shakePoints(h, step);
            for (const ShakePoint& s : points.last()) shakeTimes << std::min(s.t, c.length() - 1);
        }
        const auto offset = [&](int t, bool y) {
            double s = 0;
            for (const QVector<ShakePoint>& pts : points) {
                const ShakePoint o = shakeAt(pts, t);
                s += y ? o.y : o.x;
            }
            return s * amplitude;
        };
        write(c, before, AnimParam::PosX, shakeTimes, hits, [&](double base, int t) { return base + offset(t, false); }, {});
        write(c, before, AnimParam::PosY, shakeTimes, hits, [&](double base, int t) { return base + offset(t, true); }, {});
    }
    return int(hits.size());
}

} // namespace Accents
