// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Tracking for the Color page windows (position) and the tracker node of the compositing graph (one point, two points
// for size and rotation, four points of a flat surface for perspective): follows the picture inside a region from frame to frame. Own template matching (normalized cross
// correlation over the color channels, sub-pixel peak, slowly updated template) – no OpenCV.

#include "core/GradeWindows.h"
#include "core/ProjectFormat.h"
#include "core/Types.h"
#include "engine/video/Compose.h"

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
    // Takes in the picture at the current place into the template by `mix` (0..1): what it looks for changes (a
    // surface seen from another angle)
    void relearn(const Gray& frame, float mix);
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

// Four points of a flat surface (a sign, a screen, a wall) followed together: each frame a homography from where they
// were in the first picture to where they are now (corners of the surface: map them through it). Each frame all four
// are searched; a point that is lost or jumps away from what the other three say (its place against the frame's
// movement of the others, the reprojection error) is an outlier: it is put where the movement of the good points puts
// it (affine from three, size and turn from two). Fewer than two points found = lost (everything stays as it was).
class QuadTracker {
public:
    QuadTracker(const Gray& first, const QRectF regions[4]);
    bool valid() const;
    // false = fewer than two points found; radius as PointTracker::next
    bool next(const Gray& frame, int radius = 0);
    QPointF point(int i) const { return m_points[size_t(i)].center(); }
    QPointF start(int i) const { return m_start[size_t(i)]; }
    // Start points -> points now (pixels of the pictures)
    const Compose::Homography& homography() const { return m_h; }
    int outliers() const { return m_outliers; } // points the last frame did not use (0..4)
    double score() const;
    void setAdaptWhenUnsure(bool on)
    {
        for (PointTracker& p : m_points) p.setAdaptWhenUnsure(on);
    }
    // A point further than this from where the other three put it is an outlier (pixels; at least kMinError)
    static constexpr double kMinError = 2.0, kErrorShare = 0.1; // share of the template side
    // A point at least this good that the others confirm takes in its look of the frame by this share
    static constexpr double kLearn = 0.6, kLearnAlone = 0.8;
    static constexpr float kLearnMix = 0.2f;
    // Area of the four points against the last frame beyond this factor (either way) = lost
    static constexpr double kMaxAreaStep = 1.4;

private:
    std::vector<PointTracker> m_points;
    QPointF m_start[4];
    Compose::Homography m_h;
    int m_outliers = 0;
    double m_tolerance = kMinError;
    QPointF m_step[4]; // each point's movement in the last frame (where the next search starts)
    bool m_moved = false;
};

// How much a w x h square around (x, y) of a picture has to hold on to: the smaller eigenvalue of its structure
// tensor (Shi-Tomasi), gradients of values 0..1 per pixel. 0 = flat or a straight edge only (cannot be tracked)
double cornerStrength(const Gray& g, QPointF center, double side);

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
