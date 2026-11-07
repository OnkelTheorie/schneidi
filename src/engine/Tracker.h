// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Tracking for the Color page windows (position) and the tracker node of the compositing graph (one point, two points
// for size and rotation): follows the picture inside a region from frame to frame. Own template matching (normalized cross
// correlation over the color channels, sub-pixel peak, slowly updated template) – no OpenCV.

#include "core/GradeWindows.h"
#include "core/ProjectFormat.h"
#include "core/Types.h"

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

#include <algorithm>
#include <atomic>
#include <functional>
#include <vector>

namespace Tracker {

// Picture for matching: values 0..1, one plane per channel (gray: 1, color: R, G, B one after the other)
struct Gray {
    int width = 0, height = 0, channels = 1;
    std::vector<float> px;
    float at(int x, int y, int c = 0) const { return px[(size_t(c) * height + y) * width + x]; }
};
Gray toGray(const QImage& img);
// R, G and B planes: a red object on a busy background of the same brightness stays distinct
Gray toColor(const QImage& img);

// Follows one region from picture to picture (pixel coordinates of the pictures, all the same size)
class PointTracker {
public:
    // region: area to follow in `first` (its center is the tracked point)
    PointTracker(const Gray& first, const QRectF& region);
    // Finds the region in the next picture; false = lost (no good match), then the position stays.
    // radius = how far to search around the last position in pixels (0 = ~60 % of the template size; wider to find
    // a point again after it was hidden)
    bool next(const Gray& frame, int radius = 0);
    QPointF center() const { return m_center; }
    // Puts the point somewhere else without changing what it looks for (an outlier corrected by its partner)
    void moveTo(const QPointF& center) { m_center = center; }
    // Template update below the "good match" score: false = keep it (a partly hidden point must not learn the
    // occluder), true = follow faster (default: a background that changes)
    void setAdaptWhenUnsure(bool on) { m_adaptUnsure = on; }
    int templateWidth() const { return m_tw; }
    int templateHeight() const { return m_th; }
    double score() const { return m_score; } // 1 = identical, below ~0.5 = lost
    bool valid() const { return m_tw > 0; }

private:
    double ncc(const Gray& f, int x0, int y0) const; // template at top left (x0, y0)
    void takeTemplate(const Gray& f, double x0, double y0, float mix); // top left, may be sub-pixel
    int m_tw = 0, m_th = 0, m_channels = 1;
    // Template, each channel minus its weighted mean. Weights fall off towards the edge (Gaussian): the object sits
    // in the middle of the window, a still background at its border must not hold the match in place
    std::vector<float> m_t;
    std::vector<float> m_w;  // weight per template pixel
    std::vector<float> m_wt; // weight * template (cross term)
    double m_wSum = 0;
    double m_tNorm = 0;
    QPointF m_center;
    double m_score = 1;
    bool m_adaptUnsure = true;
};

// Two points followed together (a sign, a screen, a logo glued onto a surface): their middle, distance and direction
// give position, size and rotation. Each frame both points are searched; when one of them is lost or jumps away from
// what the other one says (distance or direction changed more than a frame can), it is an outlier: the frame takes
// the movement of the good point, keeps size and rotation and puts the bad point back where it should be.
class TwoPointTracker {
public:
    TwoPointTracker(const Gray& first, const QRectF& a, const QRectF& b);
    bool valid() const { return m_a.valid() && m_b.valid() && m_dist0 > 1e-6; }
    // false = both points lost (everything stays as it was); radius as PointTracker::next
    bool next(const Gray& frame, int radius = 0);
    QPointF a() const { return m_a.center(); }
    QPointF b() const { return m_b.center(); }
    QPointF center() const { return (a() + b()) / 2; }
    double scale() const { return m_scale; } // distance / distance in the first picture
    double angle() const { return m_angle; } // degrees clockwise (y down) against the first picture
    bool outlier() const { return m_outlier; } // the last frame used only one point
    double score() const { return std::min(m_a.score(), m_b.score()); }
    void setAdaptWhenUnsure(bool on)
    {
        m_a.setAdaptWhenUnsure(on);
        m_b.setAdaptWhenUnsure(on);
    }

    // Limits of one frame: size change (relative) and turn (degrees) beyond them = outlier
    static constexpr double kMaxScaleStep = 0.15, kMaxTurnStep = 12;

private:
    PointTracker m_a, m_b;
    double m_dist0 = 0, m_angle0 = 0;
    double m_scale = 1, m_angle = 0;
    bool m_outlier = false;
};

using Point = GradeWindows::PathPoint;

struct Result {
    QVector<Point> points; // in tracking order, without the start frame
    bool lost = false;     // stopped early: the picture no longer matched (lostAt = first frame not found)
    int lostAt = -1;
    bool canceled = false;
    QString error;         // nothing tracked (clip kind, no picture)
};

// Pictures of a clip alone for tracking: without its effects/transform (what the grade filter sees), at clip frames,
// so speed changes and reverse play are included. frame(t, maxEdge); not thread-safe (one caller at a time)
std::function<QImage(int, int)> clipFrames(const ProjectFormat& format, const Clip& c);
// The same cut to the clip's own picture: a picture of another aspect ratio without the bars it gets in the frame
// (what the compositing graph works on, so 0..1 positions there and here are the same). maxEdge counts for the frame
std::function<QImage(int, int)> clipPictures(const ProjectFormat& format, const Clip& c);
// Where clipPictures' picture lies in the frame, in project pixels (centred; the whole frame when it fills it)
QRectF pictureRect(const ProjectFormat& format, const Clip& c);

// Track window n of clip c from clip frame `from` to clip frame `to` (backwards when to < from), both inside the
// clip. Pictures: the clip alone without its effects/transform (what the grade filter sees), so speed changes and
// reverse play are included. progress(done, total) may run in another thread; cancel stops after the current frame.
Result trackWindow(const ProjectFormat& format, const Clip& c, int n, int from, int to,
                   const std::function<void(int, int)>& progress = {}, const std::atomic<bool>* cancel = nullptr);

} // namespace Tracker
