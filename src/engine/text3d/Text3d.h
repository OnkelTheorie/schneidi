// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Real 3D text (0.19): the letters' outlines (Text::letters, a QPainterPath) extruded into a mesh and drawn by a small
// CPU rasterizer of its own (perspective camera, z-buffer, one directional light + ambient, supersampled edges).
//
// Mesh: object coordinates are the picture's pixels (x right, y down) and z into the picture: the front cap lies at
// z = 0, the back cap at z = depth. Caps are cut into trapezoids by a sweep over the outline (any fill rule, holes as
// in "o"/"A", overlapping contours), so the cap's area equals the outline's exactly. Walls join the caps along every
// contour; a bevel insets the caps and joins them to the walls by a 45° band (front and back).
//
// Rendering: rows of the picture over threads (Compose::forRows) in bands, each band with its own small sample buffer
// (no full-size buffers). Lighting per vertex (Gouraud), two-sided (a normal facing away from the camera is turned
// toward it). Deterministic: the same input gives the same pixels, whatever the thread count.

#include <QColor>
#include <QPainterPath>
#include <QPointF>
#include <QRect>

#include <cstdint>
#include <vector>

namespace Text3d {

struct Mesh {
    enum Part : uint8_t { Front, Back, Side };
    // Triangle soup: 3 vertices per triangle (pos/nrm: xyz per vertex), part per triangle
    std::vector<float> pos, nrm;
    std::vector<uint8_t> part;
    int triangles() const { return int(part.size()); }
    bool empty() const { return part.empty(); }
};

// Extrudes the filled area of `path` (its fill rule) by `depth` pixels, the caps inset by `bevel` (clamped to half
// the depth; 0 = sharp edges). Curves are flattened by Qt (about half a pixel). depth 0 = a flat card (both caps).
Mesh extrude(const QPainterPath& path, double depth, double bevel);

// Area of the triangles of one part, projected on the picture plane (tests: front cap area = outline area)
double area(const Mesh& m, Mesh::Part part);

// Camera, light and material
struct View {
    double rotX = 0, rotY = 0, rotZ = 0; // degrees: tilt (top away), turn (right side away), roll (clockwise)
    QPointF pivot;  // object point (x, y; z = middle of the depth) the mesh turns around
    QPointF at;     // where the pivot appears on the picture (pixels)
    double depth = 0; // the mesh's depth (pivot z = depth / 2)
    double fov = 35;  // vertical field of view of the camera (degrees, 1..150): smaller = flatter perspective
    double distance = 1; // camera distance: 1 = the front cap at its 2D size when not turned, 2 = half as large
    double lightAngle = -90; // where the light comes from in the picture plane (degrees clockwise, -90 = from the top)
    double lightHeight = 45; // its height over the picture plane (90 = from the camera, 0 = grazing)
    QColor front = Qt::white, side{140, 140, 140}, back; // back invalid = front
    double ambient = 0.35; // 0..1: light everywhere
    double shine = 0.3;    // 0..1: strength of the highlight
    double metal = 0;      // 0..1: the highlight in the material colour instead of white
    int aa = 3;            // supersampling per axis (1..4): 3 = 9 samples per pixel
};

// Draws the mesh over an RGBA picture of w x h (straight alpha). Returns the rectangle it may have changed (empty =
// nothing visible)
QRect render(const Mesh& m, const View& v, uint8_t* rgba, int w, int h);

} // namespace Text3d
