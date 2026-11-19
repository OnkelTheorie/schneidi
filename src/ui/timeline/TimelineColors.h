// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Colors of timeline content, shared by the detail timeline (TimelineView), the overview strip (TimelineOverview)
// and pages that draw a timeline of their own (one look everywhere).

#include "app/Theme.h"
#include "core/Project.h"
#include "core/Types.h"

#include <QColor>

namespace TimelineColors {

// Clip color of the track: own track color, otherwise invalid (= default per clip kind)
inline QColor trackColor(const Track& t)
{
    const TrackColorInfo* info = trackColorInfo(t.color);
    return info ? QColor::fromRgba(info->rgb) : QColor();
}

// Color of a clip: clip color of the Media Pool clip before the track color, otherwise invalid (= default).
// project may be null (no Media Pool colors then).
inline QColor clipColor(const Project* project, const Clip& c, const Track* t)
{
    if (project && !c.isTitle())
        if (const MediaInfo* m = project->mediaInfo(c.mediaPath))
            if (const TrackColorInfo* info = trackColorInfo(m->clipColor)) return QColor::fromRgba(info->rgb);
    return t ? trackColor(*t) : QColor();
}

// The base color a clip is drawn in: `color` (from clipColor) if valid, else the default per clip kind
// (title purple, compound ocher, video blue, audio green); a disabled clip is gray
inline QColor clipBaseColor(const Clip& c, TrackKind kind, const QColor& color)
{
    if (!c.enabled) return Theme::controlOff;
    return color.isValid()                ? color
           : c.isTitle()                  ? Theme::titleClip
           : c.isCompound()               ? Theme::compoundClip
           : kind == TrackKind::Video     ? Theme::videoClip
                                          : Theme::audioClip;
}

// Color of a story section by its kind (from the theme)
inline QColor kindColor(StoryKind k)
{
    switch (k) {
    case StoryKind::Hook: return Theme::primary;
    case StoryKind::Build: return Theme::videoClip;
    case StoryKind::Question: return Theme::meterMid;
    case StoryKind::Turn: return Theme::titleClip;
    case StoryKind::Resolution: return Theme::audioClip;
    case StoryKind::Cta: return Theme::secondary;
    case StoryKind::Free: break;
    }
    return Theme::controlOff;
}

} // namespace TimelineColors
