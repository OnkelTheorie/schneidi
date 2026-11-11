// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli: the player's own kill feed lines of games that frame them (CS2: a thin red outline), found by their
// shape instead of by how much of the colour there is
#include "cli/AnalysisDetail.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace Cli::detail {

namespace {

constexpr int kRate = 15; // samples per second, as colorEvents

// Sizes below are pixels of a 1080p picture: bigger recordings are sampled at that scale, smaller ones in their own
// size (scaled up, a 1 px outline would blur into a thick one) with the sizes scaled down. (highlights uses this from
// 1080p on: in a 1080p game scaled down to 720p the outlines of lines under each other run together.)

// A thin line (1-3 px): above and below it (left and right) the picture is clearly weaker somewhere within this
// many pixels. A filled area (a sign, the tint) is not; the edges of two lines under each other are 4 px apart
constexpr int kNear = 3;
constexpr double kStrong = 0.2;   // ... by this part of the colour's own strength
constexpr double kEdgeRow = 0.25; // a top or bottom edge covers at least this part of the area's width
constexpr double kSide = 0.5;     // the right edge shows in at least this part of the rows between them
constexpr int kSideReach = 4;     // the right edge within this many pixels of where the game draws it
constexpr int kDown = 4;          // a line stays or moves up; down only by this much (it is found a pixel lower)
constexpr int kGone = kRate;      // a line not seen for longer (1 s) is gone, not hidden by a flash or the hit tint
constexpr int kGap = 2;           // a line missing for more samples is only the same line with the same text
// A new line that looks like one gone up to 1 s before is that one again: the game takes a line away and shows it
// again a moment later (when the player dies it reorders the feed: back after 0.6 s). Not longer: with bots the same
// player is killed with the same weapon again 1.7 s after the old line went away (measured; a line shown again after
// 2.5 s is counted)
constexpr int kCameBack = kRate;
constexpr double kSameLine = 0.8; // ... when this part of their bright pixels (text, icons) match
constexpr int kBright = 170;      // luma of text and icons on the line
constexpr double kOtherText = 0.7; // a line in the next sample with less of its text in common is another line

// Team colours of the names (CS2: T yellow-orange, CT blue; icons are white, the frame red): a pixel of the text band
// that is clearly coloured and bright is warm (red and green well above blue: not the red frame) or cool (blue above
// red and green). Only the side of the hue counts, not the exact colour (it changes with the background shining through)
constexpr int kTeamBright = 110; // brightest channel of a name's pixel
constexpr int kTeamSat = 50;     // brightest minus darkest channel
constexpr int kTeamPixels = 4;   // a name side needs this many pixels of its colour, twice as many as of the other
constexpr int kTeamAbove = 40;  // a name's pixel is this much brighter (luma) than the middle of its line
constexpr int kTeamGap = 3;      // the frame's top edge may break for this many outline widths (`near`) going left

int teamOf(const uchar* p)
{
    const int r = p[0], g = p[1], b = p[2];
    const int hi = std::max({r, g, b}), lo = std::min({r, g, b});
    if (hi < kTeamBright || hi - lo < kTeamSat) return 0;
    if (b >= r + 40 && b >= g) return -1;
    if (r >= b + 50 && g >= b + 20 && g * 100 >= r * 55) return 1;
    return 0;
}

struct Found {
    int top, bottom;        // rows of the edges, in the sampled area
    double strength;        // how clearly it is a line (for picking one of neighbouring finds)
    std::vector<uchar> sig; // bright pixels inside (sigW x sigH): the line's text and icons
    // Team colour of the first name (the killer) and the last one (the victim): 1 warm, -1 cool, 0 not clear (white
    // text, only one name, cut off)
    int killer = 0, victim = 0;
};

struct Track {
    int y, first, last;
    std::vector<int> seen;       // samples where it was found
    std::vector<const Found*> at; // the find in each of them
    bool old;                    // on screen from the start: its kill happened before the recording
};

// Part of the bright pixels of two lines that match (the best of small shifts: found a pixel higher or lower)
double sameContent(const std::vector<uchar>& a, const std::vector<uchar>& b, int w, int h)
{
    double best = 0;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -3; dx <= 3; ++dx) {
            int both = 0, either = 0;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const int bx = x - dx, by = y - dy;
                    const bool in = a[size_t(y * w + x)];
                    const bool other = bx >= 0 && bx < w && by >= 0 && by < h && b[size_t(by * w + bx)];
                    both += in && other;
                    either += in || other;
                }
            if (either > 0) best = std::max(best, double(both) / either);
        }
    return best;
}

// How much of the smaller text of two lines lies in the other (best of small shifts); -1 when one of them shows too
// little of its text to tell
double textShare(const std::vector<uchar>& a, const std::vector<uchar>& b, int w, int h)
{
    const int na = int(std::count(a.begin(), a.end(), uchar(1))), nb = int(std::count(b.begin(), b.end(), uchar(1)));
    if (std::min(na, nb) < std::max(8, w * h / 100)) return -1;
    int best = 0;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -3; dx <= 3; ++dx) {
            int both = 0;
            for (int y = std::max(0, dy); y < std::min(h, h + dy); ++y)
                for (int x = std::max(0, dx); x < std::min(w, w + dx); ++x)
                    both += a[size_t(y * w + x)] && b[size_t((y - dy) * w + (x - dx))];
            best = std::max(best, both);
        }
    return double(best) / std::min(na, nb);
}

// Clearly another text: both show enough of it and little of the smaller one lies in the other. A line fading in or
// out, or under a flash, keeps (part of) its text: not another one
bool otherText(const std::vector<uchar>& a, const std::vector<uchar>& b, int w, int h)
{
    const double share = textShare(a, b, w, h);
    return share >= 0 && share < kOtherText;
}

} // namespace

// The lines are found in every sample by their outline: a top and a bottom edge `lineHeight` apart and the right
// edge at `rightEdge`, each a thin line of the colour (clearly more of it than the picture a few pixels to either
// side). Measured that way the game picture behind does not count: a red sign or the red tint of a hit fill an
// area instead of drawing a thin line, and on a bright or dark background the semi-transparent outline is as thin
// as before. Lines are followed from sample to sample (they stay or move up when an old one goes away; a flash or
// the tint can hide them for a moment); one that nothing explains is new. A line the game took away and showed
// again (it reorders the feed when the player dies) has the same text as the one that went: not new.
ColorEvents framedLineEvents(const MediaSource& m, const QRectF& region, const QColor& color, double lineHeight,
                             double rightEdge, double lineLeft, const std::function<void(double)>& progress)
{
    const ProjectFormat f = pictureFormat(m);
    const QSize full(std::max(2, f.width), std::max(2, f.height));
    const double q = std::min(1.0, 1080.0 / full.height()); // picture pixels -> sampled pixels
    const double u = full.height() * q / 1080;              // 1080p pixels -> sampled pixels
    const auto scaled = [u](int size, int least) { return std::max(least, int(std::lround(size * u))); };
    // At least 2: a 1 px outline drawn at a lower resolution is often 2 px thick
    const int near = scaled(kNear, 2), reach = scaled(kSideReach, 2), down = scaled(kDown, 1), inset = scaled(3, 1);
    // The region and a margin for the neighbours of its outermost pixels and the right edge, in picture pixels
    const double pad = (near + reach + 1) / q;
    // From `lineLeft` on: the names at the left of long lines (the killer's team colour)
    const double left = std::min(region.x(), lineLeft);
    const QRectF want(left * full.width() - pad, region.y() * full.height() - pad,
                      std::max(region.right(), rightEdge) * full.width() - left * full.width() + 2 * pad,
                      region.height() * full.height() + 2 * pad);
    const QRect r = want.toAlignedRect().intersected(QRect(QPoint(0, 0), full));
    if (r.width() < 8 || r.height() < 8) fail("BAD_ARGUMENT", "the kill feed region is outside the picture");
    const int w = std::max(2, int(std::lround(r.width() * q)) & ~1), h = std::max(2, int(std::lround(r.height() * q)) & ~1);
    const double sx = double(w) / r.width(), sy = double(h) / r.height(); // picture pixels -> sampled pixels
    const int x0 = std::max(near, int(std::lround((region.x() * full.width() - r.x()) * sx)));
    const int edge = int(std::lround((rightEdge * full.width() - r.x()) * sx));
    const int x1 = std::min(w - near, edge - near); // the edge rows end before the right edge
    const int y0 = std::max(near, int(std::lround((region.y() * full.height() - r.y()) * sy)));
    const int y1 = std::min(h - near, int(std::lround((region.bottom() * full.height() - r.y()) * sy)));
    const int span = int(std::lround(lineHeight * full.height() * sy)); // top edge to bottom edge
    if (x1 - x0 < 8 || y1 - y0 < span + 2) fail("BAD_ARGUMENT", "the kill feed region is too small for a line");

    // Strength of the colour: how far a pixel goes from grey in the colour's direction (the line drawn over a
    // bright or dark background keeps its hue, not its exact colour)
    const double gc = (color.red() + color.green() + color.blue()) / 3.0;
    double ur = color.red() - gc, ug = color.green() - gc, ub = color.blue() - gc;
    const double length = std::sqrt(ur * ur + ug * ug + ub * ub);
    if (length < 30) fail("BAD_ARGUMENT", "the kill feed colour is too grey to find framed lines by");
    ur /= length;
    ug /= length;
    ub /= length;
    const float strong = float(kStrong * length);
    const int sigX0 = x0, sigW = std::max(1, edge - reach - x0);
    const int sigY0 = int(std::lround(span * 0.2)), sigH = std::max(1, int(std::lround(span * 0.6)));

    std::vector<std::vector<Found>> found;
    std::vector<char> have;
    double most = 0;
    std::mutex mutex;
    const QRectF part(double(r.x()) / full.width(), double(r.y()) / full.height(), double(r.width()) / full.width(),
                      double(r.height()) / full.height());
    const QString filter =
        QString("scale=%1:%2,crop=%3:%4:%5:%6")
            .arg(int(std::lround(full.width() * sx)))
            .arg(int(std::lround(full.height() * sy)))
            .arg(w)
            .arg(h)
            .arg(int(std::lround(r.x() * sx)))
            .arg(int(std::lround(r.y() * sy)));
    pictureSamples(
        m, part, QSize(w, h), kRate, filter,
        [&](int sample, const uchar* p) {
            const uchar* const rgb = p;
            std::vector<float> s(size_t(w) * h);
            std::vector<uchar> bright(size_t(w) * h);
            for (int i = 0; i < w * h; ++i, p += 3) {
                s[size_t(i)] = float(std::max(0.0, p[0] * ur + p[1] * ug + p[2] * ub));
                bright[size_t(i)] = 299 * p[0] + 587 * p[1] + 114 * p[2] >= kBright * 1000;
            }
            const auto at = [&](int x, int y) { return s[size_t(y * w + x)]; };
            // On a thin line: the weakest pixel within `near` on each side (steps dx, dy) is clearly weaker
            const auto thin = [&](int x, int y, int dx, int dy) {
                float before = at(x - dx, y - dy), after = at(x + dx, y + dy);
                for (int d = 2; d <= near; ++d) {
                    before = std::min(before, at(x - d * dx, y - d * dy));
                    after = std::min(after, at(x + d * dx, y + d * dy));
                }
                return at(x, y) - std::max(before, after) >= strong;
            };
            // Rows: how much of the width is a thin horizontal line of the colour
            std::vector<int> rows(size_t(h), 0);
            for (int y = near; y < h - near; ++y)
                for (int x = x0; x < x1; ++x) rows[size_t(y)] += thin(x, y, 0, 1);
            // The right edge: a thin vertical line near where the game draws it
            std::vector<char> side(size_t(h), 0);
            for (int y = 0; y < h; ++y)
                for (int x = std::max(near, edge - reach); x < std::min(w - near, edge + reach) && !side[size_t(y)]; ++x)
                    side[size_t(y)] = thin(x, y, 1, 0);
            const int need = int(std::ceil(kEdgeRow * (x1 - x0)));
            // The team colours of a line's first and last name: the line's left end where its top or bottom edge
            // ends, then the coloured pixels of the text band (the middle of the line) in each column up to the right
            // edge; the first name is the first two line heights of coloured text, the last name the last two
            const auto teams = [&](Found& l) {
                int from = x0;
                for (int x = x0, miss = 0; x >= near && miss <= kTeamGap * near; --x) {
                    const bool on = thin(x, l.top, 0, 1) || thin(x, l.bottom, 0, 1)
                                    || (l.top + 1 < h - near && thin(x, l.top + 1, 0, 1))
                                    || (l.bottom - 1 >= near && thin(x, l.bottom - 1, 0, 1));
                    if (on) {
                        from = x;
                        miss = 0;
                    } else {
                        ++miss;
                    }
                }
                if (from <= near) return; // the line goes on past the sampled area: its first name is not there
                // The edge rows may run on into the game picture (a roof edge, a cable): the line ends at its left
                // side, the leftmost thin vertical line of the colour between its edges
                int leftSide = -1;
                for (int x = std::max(near, from - reach); x <= x0 && leftSide < 0; ++x) {
                    int sides = 0;
                    for (int k = l.top + inset; k <= l.bottom - inset; ++k) sides += thin(x, k, 1, 0);
                    if (sides >= kSide * std::max(1, l.bottom - l.top - 2 * inset + 1)) leftSide = x;
                }
                if (leftSide < 0) return;
                from = leftSide;
                const int a = from + near + 1, b = edge - reach;
                const int ya = l.top + int(std::lround(span * 0.25)), yb = std::min(h, l.top + int(std::lround(span * 0.8)));
                if (b - a < 4 * span) return;
                // Names stand out of the line's background: over a bright picture (the sky) the see-through box
                // takes its colour, which is no name
                const auto luma = [&](int x, int y) {
                    const uchar* q = rgb + size_t(y * w + x) * 3;
                    return (299 * q[0] + 587 * q[1] + 114 * q[2]) / 1000;
                };
                std::vector<int> band;
                band.reserve(size_t(b - a) * std::max(0, yb - ya));
                for (int y = ya; y < yb; ++y)
                    for (int x = a; x < b; ++x) band.push_back(luma(x, y));
                if (band.empty()) return;
                std::nth_element(band.begin(), band.begin() + band.size() / 2, band.end());
                const int above = band[band.size() / 2] + kTeamAbove;
                std::vector<int> warm(size_t(b - a), 0), cool(size_t(b - a), 0);
                int first = -1, last = -1;
                for (int x = a; x < b; ++x) {
                    for (int y = ya; y < yb; ++y) {
                        const int t = luma(x, y) >= above ? teamOf(rgb + size_t(y * w + x) * 3) : 0;
                        warm[size_t(x - a)] += t > 0;
                        cool[size_t(x - a)] += t < 0;
                    }
                    if (warm[size_t(x - a)] + cool[size_t(x - a)] > 0) {
                        if (first < 0) first = x - a;
                        last = x - a;
                    }
                }
                const int width = 2 * span;
                if (first < 0 || last - first < 2 * width) return; // one name only (a suicide) or none
                const auto side = [&](int x, int y) {
                    int ww = 0, cc = 0;
                    for (int k = x; k < y; ++k) {
                        ww += warm[size_t(k)];
                        cc += cool[size_t(k)];
                    }
                    return ww >= kTeamPixels && ww >= 2 * cc ? 1 : cc >= kTeamPixels && cc >= 2 * ww ? -1 : 0;
                };
                l.killer = side(first, first + width);
                l.victim = side(last + 1 - width, last + 1);
            };
            std::vector<Found> lines;
            for (int y = y0; y + span + 1 < y1 + near; ++y) {
                if (rows[size_t(y)] < need) continue;
                int b = y + span;
                for (const int k : {y + span - 1, y + span + 1})
                    if (k < h && rows[size_t(k)] > rows[size_t(b)]) b = k;
                if (b >= h || rows[size_t(b)] < need) continue;
                int sides = 0;
                for (int k = y + inset; k <= b - inset; ++k) sides += side[size_t(k)];
                const double sideShare = double(sides) / std::max(1, b - y - 2 * inset + 1);
                if (sideShare < kSide) continue;
                lines.push_back({y, b, double(rows[size_t(y)] + rows[size_t(b)]) / (2 * (x1 - x0)) + sideShare, {}});
            }
            // Lines do not overlap (they are a few pixels apart): of finds that do the clearest. The others are the
            // same line a row lower (a 2 px edge) or made of two lines' edges (caught while the lines move up)
            std::sort(lines.begin(), lines.end(), [](const Found& a, const Found& b) { return a.strength > b.strength; });
            std::vector<Found> keep;
            for (Found& l : lines)
                if (std::none_of(keep.begin(), keep.end(), [&](const Found& k) { return l.top <= k.bottom && l.bottom >= k.top; }))
                    keep.push_back(std::move(l));
            std::sort(keep.begin(), keep.end(), [](const Found& a, const Found& b) { return a.top < b.top; });
            int pixels = 0;
            for (Found& l : keep) {
                l.sig.assign(size_t(sigW) * sigH, 0);
                for (int y = 0; y < sigH; ++y)
                    for (int x = 0; x < sigW; ++x)
                        if (const int py = l.top + sigY0 + y, px = sigX0 + x; py < h && px < w)
                            l.sig[size_t(y * sigW + x)] = bright[size_t(py * w + px)];
                pixels += rows[size_t(l.top)] + rows[size_t(l.bottom)];
                teams(l);
            }
            std::lock_guard lock(mutex);
            if (size_t(sample) >= found.size()) {
                found.resize(size_t(sample) + 1);
                have.resize(size_t(sample) + 1, 0);
            }
            found[size_t(sample)] = std::move(keep);
            have[size_t(sample)] = 1;
            most = std::max(most, pixels / (q * q) / (double(full.width()) * full.height()));
        },
        progress);
    // In time order up to the first missing sample (a read error inside the file)
    size_t samples = 0;
    while (samples < have.size() && have[samples]) ++samples;
    found.resize(samples);

    // Follow the lines: the finds of a sample matched to the lines seen up to a second before, in order (lines keep
    // their order), as many as possible, then with the least movement
    std::vector<Track> tracks;
    for (int t = 0; t < int(samples); ++t) {
        const std::vector<Found>& now = found[size_t(t)];
        std::vector<int> live; // tracks, top to bottom
        for (int i = 0; i < int(tracks.size()); ++i)
            if (t - tracks[size_t(i)].last <= kGone) live.push_back(i);
        std::sort(live.begin(), live.end(), [&](int a, int b) { return tracks[size_t(a)].y < tracks[size_t(b)].y; });
        const int nf = int(now.size()), nl = int(live.size());
        // A find fits a track: up by a line (and a bit) or not moved, not down. After a gap (the line was hidden by a
        // flash or the tint, or it went away) only with the same text: a new line often takes the place of one that
        // just went. Without a gap not with clearly another text: a new line can show in the very next sample where
        // the old one was (it went away, or the one below moved up into its place)
        std::vector<char> fit(size_t(nf) * std::max(1, nl), 0);
        for (int i = 0; i < nf; ++i)
            for (int j = 0; j < nl; ++j) {
                const Track& tr = tracks[size_t(live[size_t(j)])];
                const int y = now[size_t(i)].top;
                const std::vector<uchar>& sig = now[size_t(i)].sig;
                fit[size_t(i * nl + j)] = y >= tr.y - span * 4 / 3 && y <= tr.y + down
                                          && (t - tr.last <= kGap ? !otherText(sig, tr.at.back()->sig, sigW, sigH)
                                                                  : sameContent(sig, tr.at.back()->sig, sigW, sigH) >= kSameLine);
            }
        const auto fits = [&](int i, int j) { return fit[size_t(i * nl + j)] != 0; };
        // best[i][j]: matches of finds i.. with tracks j.. (count, movement)
        std::vector<std::pair<int, int>> best(size_t(nf + 1) * (nl + 1), {0, 0});
        const auto cell = [&](int i, int j) -> std::pair<int, int>& { return best[size_t(i * (nl + 1) + j)]; };
        const auto better = [](std::pair<int, int> a, std::pair<int, int> b) {
            return a.first != b.first ? a.first > b.first : a.second < b.second;
        };
        for (int i = nf - 1; i >= 0; --i)
            for (int j = nl - 1; j >= 0; --j) {
                std::pair<int, int> v = better(cell(i + 1, j), cell(i, j + 1)) ? cell(i + 1, j) : cell(i, j + 1);
                if (fits(i, j)) {
                    const std::pair<int, int> a{cell(i + 1, j + 1).first + 1,
                                                cell(i + 1, j + 1).second
                                                    + std::abs(now[size_t(i)].top - tracks[size_t(live[size_t(j)])].y)};
                    if (better(a, v)) v = a;
                }
                cell(i, j) = v;
            }
        std::vector<char> matched(size_t(nf), 0);
        for (int i = 0, j = 0; i < nf && j < nl;) {
            if (fits(i, j)
                && cell(i, j) == std::pair<int, int>{cell(i + 1, j + 1).first + 1,
                                                     cell(i + 1, j + 1).second
                                                         + std::abs(now[size_t(i)].top - tracks[size_t(live[size_t(j)])].y)}) {
                Track& tr = tracks[size_t(live[size_t(j)])];
                tr.y = now[size_t(i)].top;
                tr.last = t;
                tr.seen.push_back(t);
                tr.at.push_back(&now[size_t(i)]);
                matched[size_t(i)] = 1;
                ++i;
                ++j;
            } else if (cell(i, j) == cell(i + 1, j)) {
                ++i;
            } else {
                ++j;
            }
        }
        for (int i = 0; i < nf; ++i)
            if (!matched[size_t(i)]) tracks.push_back({now[size_t(i)].top, t, t, {t}, {&now[size_t(i)]}, t <= 1});
    }

    // New lines: not there from the start, found in 3 of their first 4 samples (not a flicker of the background),
    // and not a line that went away and came back
    // A teamkill: the victim's name in the killer's team colour in most of the line's first samples where both are
    // clear (it fades in; a flash or the tint may hide the colours for a moment). 1 warm, -1 cool, 0 not one
    const auto sameTeam = [](const Track& tr) {
        int same = 0, other = 0, warm = 0;
        for (size_t k = 0; k < std::min<size_t>(tr.at.size(), 15); ++k)
            if (const Found& x = *tr.at[k]; x.killer != 0 && x.victim != 0) {
                (x.killer == x.victim ? same : other)++;
                warm += x.killer == x.victim && x.killer > 0;
            }
        return same >= 3 && same > 2 * other ? (2 * warm > same ? 1 : -1) : 0;
    };
    // Free for all (deathmatch: the player comes back in either team's colours, every other player is an enemy):
    // same-colour lines of both colours, or more of them than a team game tolerates (it kicks a player after a few
    // teamkills). Then the colours say nothing about teams
    int warmSame = 0, coolSame = 0;
    for (const Track& tr : tracks)
        if (const int c = sameTeam(tr); c != 0) (c > 0 ? warmSame : coolSame)++;
    const bool teams = !(warmSame > 0 && coolSame > 0) && warmSame + coolSame < 3;

    std::vector<std::pair<int, bool>> fresh; // first sample, teamkill
    for (const Track& tr : tracks) {
        if (tr.old) continue;
        if (std::count_if(tr.seen.begin(), tr.seen.end(), [&](int k) { return k < tr.first + 4; }) < 3) continue;
        const Found& mine = *tr.at[std::min<size_t>(2, tr.at.size() - 1)]; // whole, no longer fading in
        // How alike the gone line looked in its last half second (it fades out, maybe under the tint: the clearest)
        const auto lastLooks = [&](const Track& g) {
            double most = 0;
            for (size_t k = g.at.size() > 8 ? g.at.size() - 8 : 0; k < g.at.size(); ++k)
                most = std::max(most, sameContent(mine.sig, g.at[k]->sig, sigW, sigH));
            return most;
        };
        const bool cameBack = std::any_of(tracks.begin(), tracks.end(), [&](const Track& g) {
            return &g != &tr && g.last < tr.first && tr.first - g.last <= kCameBack
                   && lastLooks(g) >= kSameLine;
        });
        if (!cameBack) fresh.push_back({tr.first, teams && sameTeam(tr) != 0});
    }
    std::sort(fresh.begin(), fresh.end());

    ColorEvents out;
    out.color = color;
    out.most = most;
    const auto frameOf = [&](int sample) {
        return std::min(m.info.length - 1, int(std::lround(double(sample) / kRate * f.rate.fps())));
    };
    // The outline of a 1080p line: two edges across the region and the right edge
    const double area = (2.0 * (x1 - x0) + span) / (q * q) / (double(full.width()) * full.height());
    for (const auto& [t, teamkill] : fresh) out.events << ColorEvent{frameOf(t), true, area, frameOf(t), teamkill};
    return out;
}

} // namespace Cli::detail
