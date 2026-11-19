// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Safe-area guide lines over a picture (`frames` guides:true): action safe (93 % of the frame) and title safe (90 %,
// EBU R 95), and in a vertical picture the zones TikTok, Reels and Shorts draw their own UI over (the same zones
// `check` warns about: ExportCheck::Options safeTop/safeBottom/safeRight). Pure painting, no project.

#include <QImage>

struct ProjectFormat;
namespace ExportCheck { struct Options; }

namespace SafeGuides {

constexpr double kActionSafe = 0.93; // share of width and height
constexpr double kTitleSafe = 0.90;

// Draws the guides onto `img` (the whole frame of format `f`, at any size): thin lines, the app zones lightly tinted
void draw(QImage& img, const ProjectFormat& f, const ExportCheck::Options& o);

} // namespace SafeGuides
