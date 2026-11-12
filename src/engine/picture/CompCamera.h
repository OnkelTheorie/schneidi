// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The shared camera of a compositing graph (0.22, 2.5D parallax). A node "camera" has no picture, only values; layers
// (transform, media, text, text3d) stand at a distance "z" from it and name it in their value "camera" (its id, read like
// a tracker through `follow`; 0 = the graph's camera: one camera per graph is the rule, so a depth is enough).
//
// Space (pixels of the output picture, x right, y down, z away from the viewer): the camera at rest looks straight at
// the picture plane from the distance D = H / 2 / tan(fov / 2) (the frame fills its field of view). A layer at z stands
// at z * D and is z times as big as its picture, so at rest every layer looks exactly as without a camera, whatever its
// z. When the camera moves, near layers (small z) move further than far ones: a pan by p pixels moves a layer at z by
// p / z. z = 0 (the default) = the layer does not belong to the space (the camera leaves it alone: a logo, a caption).
//
// Camera values (all animatable): pan_x / pan_y (where the camera moves, in picture widths / heights at the picture
// plane z = 1; positive = right / down, the picture moves the other way), push (moves forward, as a share of D: 0.5 =
// the picture plane twice as big; near layers grow more), zoom (focal length: everything bigger alike, no parallax),
// tilt_x (looks up, degrees), tilt_y (turns right), roll (turns clockwise), fov (vertical field of view, degrees),
// drift_x / drift_y / drift_push (a steady move per second from the clip's start: a slow pan without keyframes; the
// push eases off towards kDriftPushCap, so a long clip never goes through the picture plane; drift values are steady,
// keyframes on them are not integrated - the CLI refuses them), drift_offset (seconds of drift already gone at the
// clip's start: a head trim adds to it),
// follow (a tracker: its 2D motion read as pan, push and roll, so layers at z = 1 go exactly with the tracked
// picture and other depths get parallax; no 3D camera solve).
//
// The camera a layer names: its value `camera` when set, else the graph's camera (one camera per graph)
// The map of a flat layer is a homography (Compose::Homography). Rendering is the same as the tilt of a transform:
// one projective draw (CPU) or the projective layer pass (card).

#include "engine/picture/CompNodes.h"
#include "engine/video/Compose.h"

#include <QSize>
#include <QString>

#include <functional>

namespace Picture::Comp {

// The push a drift of perSecond gives after `seconds`: perSecond * seconds at first, easing towards +-kDriftPushCap
// (before 0: straight on)
inline constexpr double kDriftPushCap = 0.9;
double driftPush(double perSecond, double seconds);
// Whether h keeps the source rectangle sw x sh in front (w > 0 at its four corners): else nothing is drawn (CPU and
// card alike - a corner behind the camera would mirror)
bool inFront(const Compose::Homography& h, int sw, int sh);
// The size change of h at (x, y) (square root of the Jacobian's determinant): below 1/2 the card's sampling aliases
double scaleAt(const Compose::Homography& h, double x, double y);

struct Camera {
    double panX = 0, panY = 0; // pixels at the picture plane (camera position)
    double push = 0;           // share of D the camera moved forward
    double zoom = 1;
    double tiltX = 0, tiltY = 0, roll = 0; // degrees
    double fov = 40;
};

// The camera of node `id` at `seconds` from the clip's start, values read through numOf(id, key) (a missing node: the
// kind's defaults, so check typeOf first), its tracker through numOf(tracker, key) by the follow rule
Camera cameraAt(const std::function<double(int, const QString&)>& numOf, int id, QSize size, double seconds);
// The map of a layer at depth z (> 0) of a picture of `size` through the camera. false = a corner at or behind the
// camera (nothing to draw)
bool cameraMap(const Camera& cam, double z, QSize size, Compose::Homography* out);
// No change at all (the camera at rest): within 1e-9
bool isIdentity(const Compose::Homography& h);

// The camera map of the node in ctx (its values "z" and "camera"; the camera node's type through ctx.typeOf).
// Returns false when the node does not stand in a camera's space or the camera is at rest (*h untouched, nothing to
// do); *hidden = true when it does but lies behind the camera (draw nothing)
bool layerCamera(const NodeCtx& ctx, Compose::Homography* h, bool* hidden);
// The same from the values: node `self` at depth z naming camera node `camera` (0 = graphCamera), typeOf/numOf as
// NodeCtx's, `seconds` from the clip's start (the card's planner, CompGpu)
bool layerCamera(int self, double z, double camera, int graphCamera, const std::function<QString(int)>& typeOf,
                 const std::function<double(int, const QString&)>& numOf, QSize size, double seconds,
                 Compose::Homography* h, bool* hidden);
// A layer's finished picture moved by its camera (media, text, text3d; the transform draws it in one go itself): a
// new picture from the pool, the content area mapped along. Nothing to do: unchanged
void applyCamera(NodeCtx& ctx, Buf& out);
// Whether the engine moves a kind's finished picture by its camera (applyCamera after eval): every kind that takes a
// camera except the transform, which draws through it itself
bool finishesWithCamera(const NodeType& k);

// The layer values (z, camera) added to the kinds that take a camera (core/CompGraph.h: Comp::takesCamera)
QVector<EffectParam> cameraLayerParams();

} // namespace Picture::Comp
