// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Tracking for the Color page windows (like DaVinci's window tracker, position only): follows the picture inside a
// window from frame to frame and returns the window center per frame. Own template matching (normalized cross
// correlation over the color channels, sub-pixel peak, slowly updated template) – no OpenCV.

#include "core/GradeWindows.h"
#include "core/ProjectFormat.h"
#include "core/Types.h"

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

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
    // Finds the region in the next picture; false = lost (no good match), then the position stays
    bool next(const Gray& frame);
    QPointF center() const { return m_center; }
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
};

using Point = GradeWindows::PathPoint;

struct Result {
    QVector<Point> points; // in tracking order, without the start frame
    bool lost = false;     // stopped early: the picture no longer matched (lostAt = first frame not found)
    int lostAt = -1;
    bool canceled = false;
    QString error;         // nothing tracked (clip kind, no picture)
};

// Track window n of clip c from clip frame `from` to clip frame `to` (backwards when to < from), both inside the
// clip. Pictures: the clip alone without its effects/transform (what the grade filter sees), so speed changes and
// reverse play are included. progress(done, total) may run in another thread; cancel stops after the current frame.
Result trackWindow(const ProjectFormat& format, const Clip& c, int n, int from, int to,
                   const std::function<void(int, int)>& progress = {}, const std::atomic<bool>* cancel = nullptr);

} // namespace Tracker
