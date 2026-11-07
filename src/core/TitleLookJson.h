// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// A title's look (TitleLook: fill, 2.5D edge, shadow, glow; 0.19) as JSON. The project file and the CLI op `title`
// share one form, keys next to the other title fields:
//   fill: "solid" | "gradient" | "texture"
//   gradient: {stops: [{at, color}, …] (2..4), angle}   texture: {path, scale, x, y}
//   edge3d: {on, depth, angle, color, light}   shadow: {on, x, y, soft, color, opacity}   glow: {on, radius, color, strength}
//   text3d: {on, depth, bevel, rot_x, rot_y, rot_z, fov, distance, light_angle, light_height, side, shine, metal}
// Written only when a group differs from the default (older versions then ignore nothing they would need).
// The CLI also takes short forms: gradient as a list of colours (evenly spaced) or stops, texture as a path, edge3d /
// shadow / glow as true/false; giving gradient or texture without fill switches the fill to it, a group object
// without "on" switches it on.

#include "core/Types.h"

#include <QJsonObject>
#include <QStringList>

namespace TitleLookJson {

// The keys read and written (fill, gradient, texture, edge3d, shadow, glow, text3d)
QStringList keys();
// Adds the groups that differ from the default to a title object
void write(QJsonObject& o, const TitleLook& l);
// Reads the keys present in o over l. lenient (project file): bad values keep the old one; else (CLI) the first bad
// value is reported in *err and false returned. *any = whether o had one of the keys
bool read(const QJsonObject& o, TitleLook& l, bool lenient, QString* err = nullptr, bool* any = nullptr);

} // namespace TitleLookJson
