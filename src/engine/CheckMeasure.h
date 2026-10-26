#pragma once
// What the check before export measures beyond the loudness of the mix, for the judgements in core/ExportCheck.h:
// each audio track alone (music against game sound or speech), a few small frames of the start (the hook of a
// vertical video) and the time stamps of the recordings (stutter). `check` (cli/AnalysisCheck.cpp) and the Deliver
// page call the same functions. Blocking; progress(0..1) returning false cancels.

#include "core/ExportCheck.h"

#include <functional>
#include <optional>

namespace CheckMeasure {

using Progress = std::function<bool(double)>;

// Music against the tracks with events: the tracks ExportCheck::soundRole calls music or undecided and the tracks
// playing together with them, each mixed alone (the others muted, without the master limiter) over the stretch where
// they overlap. renderTl = Project::renderTimeline. hasPicture: a media file with a picture. nullopt = canceled.
std::optional<QVector<ExportCheck::Issue>> mix(const ProjectFormat& f, const Timeline& renderTl, int from, int to,
                                               const std::function<bool(const QString& path)>& hasPicture,
                                               const ExportCheck::Options& o = {}, const Progress& progress = {});

// Mean change between neighbouring frames (gray, longer edge 96 pixels, averaged down from 384; only pixels that are
// not black in both, so black bars do not count), in % of full scale; nullopt = a frame could not be rendered
std::optional<double> motion(const ProjectFormat& f, const Timeline& renderTl, const QVector<int>& frames);

// The hook of a short vertical video (ExportCheck::wantsHook; nothing for other timelines)
QVector<ExportCheck::Issue> hook(const ProjectFormat& f, const Timeline& renderTl, int from, int to,
                                 const ExportCheck::Options& o = {});

// Stutter in the recordings the video clips of [from, to) use (engine/FrameGaps, cached). fileLength = a file's
// length in project frames (reversed clips). nullopt = canceled.
std::optional<QVector<ExportCheck::Issue>> stutter(const ProjectFormat& f, const Timeline& tl, int from, int to,
                                                   const std::function<int(const QString& path)>& fileLength,
                                                   const Progress& progress = {});

} // namespace CheckMeasure
