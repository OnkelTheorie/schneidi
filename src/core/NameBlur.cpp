// SPDX-License-Identifier: AGPL-3.0-only
#include "core/NameBlur.h"

#include <algorithm>
#include <cmath>

namespace NameBlur {

namespace {

struct TemplateName { Template t; const char* id; };
constexpr TemplateName kTemplates[] = {{Template::Cs2, "cs2"}, {Template::Valorant, "valorant"}, {Template::All, "all"}};

QRectF area(double x, double y, double w, double h) { return QRectF(x, y, w, h); }

// Text box with the zone it belongs to
struct Found {
    QRectF r;
    int sample;
};

QRectF padded(const QRectF& r, double padding)
{
    const double p = r.height() * padding;
    // a little more to the sides: the first and last letter of a name must not peek out
    return r.adjusted(-p * 1.5, -p, p * 1.5, p).intersected(QRectF(0, 0, 1, 1));
}

// The same text line in another sample: about the same place and size
bool same(const QRectF& a, const QRectF& b)
{
    const double h = std::max(a.height(), b.height());
    return std::abs(a.center().y() - b.center().y()) < 0.5 * h && std::abs(a.height() - b.height()) < 0.35 * h
           && std::abs(a.center().x() - b.center().x()) < std::max(h, 0.25 * std::max(a.width(), b.width()));
}

// Off key at the place of an on rectangle (zero size at its center: nothing grows out of a corner)
Key off(int frame, const QRectF& at)
{
    return Key{frame, false, QRectF(at.center(), QSizeF(0, 0))};
}

// Keys from an on/off rectangle per sample: a shrink to nothing on the frame after the last on sample and from
// nothing on the frame before the first, so a mask never slides in from where it was last
QVector<Key> keysOf(const QVector<int>& frames, const QVector<QRectF>& rects)
{
    QVector<Key> keys;
    QRectF last;
    for (int i = 0; i < frames.size(); ++i) {
        const bool on = !rects[i].isEmpty();
        const bool wasOn = !keys.isEmpty() && keys.last().on;
        if (on) {
            if (!wasOn && frames[i] > 0 && (keys.isEmpty() || keys.last().frame < frames[i] - 1))
                keys << off(frames[i] - 1, rects[i]);
            keys << Key{frames[i], true, rects[i]};
            last = rects[i];
        } else if (wasOn) {
            const int f = std::min(frames[i], keys.last().frame + 1);
            keys << off(f, last);
        }
    }
    return keys;
}

bool linearFits(const Key& a, const Key& b, const Key& m)
{
    if (a.on != m.on || b.on != m.on) return false;
    const double f = double(m.frame - a.frame) / std::max(1, b.frame - a.frame);
    const auto lerp = [f](double x, double y) { return x + (y - x) * f; };
    constexpr double eps = 0.002;
    return std::abs(lerp(a.rect.center().x(), b.rect.center().x()) - m.rect.center().x()) < eps
           && std::abs(lerp(a.rect.center().y(), b.rect.center().y()) - m.rect.center().y()) < eps
           && std::abs(lerp(a.rect.width(), b.rect.width()) - m.rect.width()) < eps
           && std::abs(lerp(a.rect.height(), b.rect.height()) - m.rect.height()) < eps;
}

// Leaves out keys that linear interpolation of their neighbours gives anyway (a kill feed standing still)
QVector<Key> simplify(const QVector<Key>& keys)
{
    if (keys.size() < 3) return keys;
    QVector<Key> out{keys.first()};
    for (int i = 1; i + 1 < keys.size(); ++i)
        if (!linearFits(out.last(), keys[i + 1], keys[i])) out << keys[i];
    out << keys.last();
    return out;
}

struct Track {
    QVector<int> samples; // sample indexes, ascending
    QVector<QRectF> boxes;
    int first() const { return samples.first(); }
    int last() const { return samples.last(); }
};

} // namespace

const char* templateId(Template t)
{
    for (const auto& x : kTemplates)
        if (x.t == t) return x.id;
    return kTemplates[0].id;
}

bool templateFromId(const QString& id, Template* t)
{
    for (const auto& x : kTemplates)
        if (id == QLatin1String(x.id)) {
            *t = x.t;
            return true;
        }
    return false;
}

QStringList templateIds()
{
    QStringList out;
    for (const auto& x : kTemplates) out << x.id;
    return out;
}

Layout layout(Template t)
{
    Layout l;
    const QRectF crosshair = area(0.47, 0.45, 0.06, 0.1);
    switch (t) {
    case Template::Cs2:
        // Default CS2 HUD in 16:9. Names: above the team avatars at the top, the kill feed top right, chat bottom
        // left, the spectated player bottom middle, pick-ups on the right ("… von <name>"), name tags in the view.
        l.exclude = {area(0, 0, 0.17, 0.33),        // radar with the place name under it
                     area(0.32, 0.025, 0.36, 0.16), // score, timer, money and weapons under the avatars
                     area(0.4, 0.72, 0.2, 0.09),    // round messages in the middle ("bomb planted")
                     crosshair,
                     area(0, 0.9, 0.38, 0.1),       // money, health, armor
                     area(0.6, 0.9, 0.4, 0.1)};     // ammo, weapon name
        l.include = {{"team", area(0.32, 0, 0.36, 0.025), false},
                     {"killfeed", area(0.6, 0.04, 0.4, 0.31), false},
                     {"chat", area(0, 0.6, 0.42, 0.3), false},
                     // the own/spectated name left of the avatar: pink on a pink plate is often missed, it is always there
                     {"player", area(0.385, 0.94, 0.085, 0.045), false, true},
                     {"items", area(0.8, 0.66, 0.2, 0.24), false},
                     {"view", area(0, 0.03, 1, 0.87), true}};
        break;
    case Template::Valorant:
        // Default Valorant HUD in 16:9: kill feed top right, chat bottom left, name tags in the view
        l.exclude = {area(0, 0, 0.25, 0.38),        // minimap
                     area(0.3, 0, 0.4, 0.12),       // score, timer, agent portraits
                     crosshair,
                     area(0.25, 0.85, 0.5, 0.15),   // health, abilities
                     area(0.75, 0.85, 0.25, 0.15)}; // ammo
        l.include = {{"killfeed", area(0.68, 0.06, 0.32, 0.32), false},
                     {"chat", area(0, 0.55, 0.36, 0.3), false},
                     {"view", area(0, 0.1, 1, 0.75), true}};
        break;
    case Template::All:
        l.include = {{"all", area(0, 0, 1, 1), true}};
        break;
    }
    return l;
}

QVector<Mask> plan(const QVector<Sample>& samples, Template t, int length, const Options& o, int* dropped)
{
    if (dropped) *dropped = 0;
    const Layout l = layout(t);
    const int n = samples.size();
    // Text per zone and sample
    QVector<QVector<QVector<QRectF>>> inZone(l.include.size(), QVector<QVector<QRectF>>(n));
    for (int i = 0; i < n; ++i)
        for (const QRectF& b : samples[i].boxes) {
            const QPointF c = b.center();
            if (std::any_of(l.exclude.begin(), l.exclude.end(), [&](const QRectF& e) { return e.contains(c); })) continue;
            for (int z = 0; z < l.include.size(); ++z)
                if (l.include[z].area.contains(c)) {
                    inZone[z][i] << b;
                    break;
                }
        }
    const auto clampFrame = [length](int f) { return std::clamp(f, 0, std::max(0, length - 1)); };
    QVector<int> frames;
    for (const Sample& s : samples) frames << clampFrame(s.frame);

    QVector<Mask> out;
    QVector<Track> tracks;
    for (int z = 0; z < l.include.size(); ++z) {
        const Zone& zone = l.include[z];
        const auto& per = inZone[z];
        if (zone.always) {
            if (n == 0) continue;
            Mask m;
            m.zone = zone.name;
            m.keys = {Key{0, true, zone.area}};
            out << m;
            continue;
        }
        if (!zone.moving) {
            // Kept when it stands at the same place on most samples around it (HUD text; the 3D picture behind moves)
            const int half = std::max(1, int(std::lround(o.samplesPerSecond / 2)));
            QVector<QRectF> rects(n);
            for (int i = 0; i < n; ++i)
                for (const QRectF& b : per[i]) {
                    // the half second before or the one after (text that just appeared or is about to go)
                    const auto steady = [&](int from, int to) {
                        int seen = 0, total = 0;
                        for (int j = std::max(0, from); j <= std::min(n - 1, to); ++j) {
                            if (j == i) continue;
                            ++total;
                            seen += std::any_of(per[j].begin(), per[j].end(), [&](const QRectF& o2) { return same(b, o2); });
                        }
                        return total > 0 && seen >= o.steadyShare * total;
                    };
                    if (steady(i - half, i) || steady(i, i + half)) rects[i] |= padded(b, o.padding);
                }
            // One sample earlier and later as well: text appears between two samples
            QVector<QRectF> wide(n);
            for (int i = 0; i < n; ++i)
                for (int j = std::max(0, i - 1); j <= std::min(n - 1, i + 1); ++j)
                    if (!rects[j].isEmpty()) wide[i] = wide[i].isEmpty() ? rects[j] : (wide[i] | rects[j]);
            if (std::all_of(wide.begin(), wide.end(), [](const QRectF& r) { return r.isEmpty(); })) continue;
            Mask m;
            m.zone = zone.name;
            m.keys = simplify(keysOf(frames, wide));
            out << m;
            continue;
        }
        // Moving text: tracks from sample to sample (nearest box of a similar size, gaps up to 3 samples)
        QVector<Track> open;
        for (int i = 0; i < n; ++i) {
            QVector<bool> used(open.size(), false);
            for (const QRectF& b : per[i]) {
                int best = -1;
                double bestD = 1e9;
                for (int k = 0; k < open.size(); ++k) {
                    const Track& tr = open[k];
                    if (used[k] || tr.last() < i - 3) continue;
                    const QRectF& p = tr.boxes.last();
                    const double hr = b.height() / std::max(1e-6, p.height());
                    if (hr < 0.5 || hr > 2) continue;
                    const double dx = std::abs(b.center().x() - p.center().x());
                    const double dy = std::abs(b.center().y() - p.center().y());
                    if (dx > std::max(p.width(), 0.06) || dy > std::max(3 * p.height(), 0.04)) continue;
                    const double d = dx + dy;
                    if (d < bestD) {
                        bestD = d;
                        best = k;
                    }
                }
                if (best < 0) {
                    open << Track{{i}, {b}};
                    used << true;
                } else {
                    open[best].samples << i;
                    open[best].boxes << b;
                    used[best] = true;
                }
            }
        }
        // Long enough, found on most samples of its time, without jumping in size (a texture flickers and changes)
        const int minSamples = std::max(3, int(std::lround(o.minTrackSeconds * o.samplesPerSecond)));
        for (const Track& tr : open) {
            if (tr.samples.size() < minSamples || tr.samples.size() < 0.6 * (tr.last() - tr.first() + 1)) continue;
            QVector<double> change;
            for (int k = 1; k < tr.boxes.size(); ++k)
                change << std::abs(tr.boxes[k].width() - tr.boxes[k - 1].width()) / std::max(1e-6, tr.boxes[k - 1].width());
            std::nth_element(change.begin(), change.begin() + change.size() / 2, change.end());
            if (change[change.size() / 2] <= 0.2) tracks << tr;
        }
    }

    // Tracks share windows when they do not overlap in time (longest first: what stays longest matters most)
    std::sort(tracks.begin(), tracks.end(), [](const Track& a, const Track& b) {
        return a.samples.size() != b.samples.size() ? a.samples.size() > b.samples.size() : a.first() < b.first();
    });
    const int lanesMax = std::max(0, o.maxMasks - int(out.size()));
    QVector<QVector<const Track*>> lanes;
    for (const Track& tr : tracks) {
        // a sample before and after it is covered as well (seen between two samples)
        const int a = std::max(0, tr.first() - 1), b = std::min(n - 1, tr.last() + 1);
        bool placed = false;
        for (auto& lane : lanes) {
            const bool free = std::none_of(lane.begin(), lane.end(), [&](const Track* o2) {
                return a <= std::min(n - 1, o2->last() + 2) && b >= std::max(0, o2->first() - 2);
            });
            if (free) {
                lane << &tr;
                placed = true;
                break;
            }
        }
        if (!placed && lanes.size() < lanesMax) {
            lanes << QVector<const Track*>{&tr};
            placed = true;
        }
        if (!placed && dropped) ++*dropped;
    }
    for (auto& lane : lanes) {
        std::sort(lane.begin(), lane.end(), [](const Track* x, const Track* y) { return x->first() < y->first(); });
        QVector<QRectF> rects(n);
        for (const Track* tr : lane) {
            for (int k = 0; k < tr->samples.size(); ++k) {
                rects[tr->samples[k]] = padded(tr->boxes[k], o.padding);
                // gaps inside the track: in between, like the keyframes would
                if (k > 0)
                    for (int g = tr->samples[k - 1] + 1; g < tr->samples[k]; ++g) {
                        const double f = double(g - tr->samples[k - 1]) / (tr->samples[k] - tr->samples[k - 1]);
                        const QRectF& p = rects[tr->samples[k - 1]];
                        const QRectF& q = rects[tr->samples[k]];
                        rects[g] = QRectF(p.x() + (q.x() - p.x()) * f, p.y() + (q.y() - p.y()) * f,
                                          p.width() + (q.width() - p.width()) * f, p.height() + (q.height() - p.height()) * f);
                    }
            }
            if (tr->first() > 0 && rects[tr->first() - 1].isEmpty()) rects[tr->first() - 1] = rects[tr->first()];
            if (tr->last() + 1 < n && rects[tr->last() + 1].isEmpty()) rects[tr->last() + 1] = rects[tr->last()];
        }
        Mask m;
        m.zone = QStringLiteral("view");
        for (int z = 0; z < l.include.size(); ++z)
            if (l.include[z].moving) m.zone = l.include[z].name;
        m.moving = true;
        m.keys = simplify(keysOf(frames, rects));
        out << m;
    }
    return out;
}

QRectF rectAt(const Mask& m, int t)
{
    if (m.keys.isEmpty()) return {};
    const auto it = std::lower_bound(m.keys.begin(), m.keys.end(), t, [](const Key& k, int f) { return k.frame < f; });
    const Key* b = it == m.keys.end() ? &m.keys.last() : &*it;
    const Key* a = it == m.keys.begin() ? b : &*(it - 1);
    if (it != m.keys.end() && it->frame == t) a = b;
    const double f = a == b ? 0 : double(t - a->frame) / (b->frame - a->frame);
    const auto lerp = [f](double x, double y) { return x + (y - x) * f; };
    const double w = lerp(a->rect.width(), b->rect.width()), h = lerp(a->rect.height(), b->rect.height());
    if (w < 1e-4 || h < 1e-4) return {};
    const QPointF c(lerp(a->rect.center().x(), b->rect.center().x()), lerp(a->rect.center().y(), b->rect.center().y()));
    return QRectF(c.x() - w / 2, c.y() - h / 2, w, h);
}

} // namespace NameBlur
