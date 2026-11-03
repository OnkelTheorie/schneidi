// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Story sections and notes as the edit ops and the timeline change them (0.17 package "story"): adding a section
// without overlaps, the automatic suggestion from the cuts, a teaser of a moment in front of the timeline, and how
// sections, notes and markers follow when time is inserted or cut out at the start. Pure functions on the model,
// no Qt beyond containers and strings: testable and the same on every platform. The checks that read the sections
// live in core/ExportCheckStory.cpp.

#include "core/Types.h"

#include <QString>
#include <QVector>

#include <functional>

namespace Story {

// Adds `s` to `sections` (sorted by from). Empty string = done; otherwise why not (to <= from, overlaps another
// section: the text names it)
QString add(QVector<StorySection>& sections, const StorySection& s);
// The index of the section that contains `frame` (from <= frame < to), -1 = none
int at(const QVector<StorySection>& sections, int frame);

// Suggested sections for a timeline of `length` frames with picture cuts at `cuts` (timeline frames, any order):
// hook = the opening up to the first cut after at least 1 s, at most `hookMax` frames; resolution = the last shot
// (at most the last quarter when the last shot is long, at least the last 15 %); build = everything between. A
// timeline too short for three parts gets a hook and a build only, or just a hook.
QVector<StorySection> suggest(int length, QVector<int> cuts, int fps, int hookMax);

// Time inserted at `frame` (length > 0) or cut out from `frame` (length < 0, [frame, frame - length) removed):
// sections, notes and markers after it move with the material; a section that spans the point grows or shrinks,
// one inside a removed range goes (as do its notes and markers there)
void shift(QVector<StorySection>& sections, QVector<TimelineNote>& notes, QVector<int>& markers, int frame, int length);

// A copy of [from, to) of the timeline placed in front of it (a teaser): every clip and subtitle cue in the range,
// cut to it, at 0; the timeline itself (clips, subtitles, sections, notes, markers) moves back by to - from. Locked
// tracks take part too (the copy is a new timeline). Returns false when the range holds no clip.
bool prependTeaser(Timeline& tl, int from, int to, const std::function<int()>& newClipId,
                   const std::function<int()>& newLinkId);

} // namespace Story
