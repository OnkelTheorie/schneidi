#pragma once
// Shared parts of the commands that look at the material (Analysis.cpp, AnalysisAudio.cpp, AnalysisVideo.cpp)

#include "cli/CommandsDetail.h"
#include "engine/Profiles.h"

#include <QRect>
#include <QSize>

namespace Cli::detail {

// Media file + the frame rate frames count in (project format, or the file's own rate)
struct MediaSource {
    ProjectFormat format;
    MediaInfo info;
    ClipFormat clip; // the file's own format (detected when no project gives the frame rate)
};

// `file` (+ optional `project` for its frame rate); throws if it cannot be read
MediaSource openMedia(const QJsonObject& a);

// "x,y,w,h" in pixels of the full picture, or fractions of it when all four are <= 1 ("0.75,0,0.25,0.3")
QRect cropRect(const QString& spec, QSize full);

// One file (`file`) or several (`files`, one result each, looked at in parallel up to `parallel` at once)
QJsonObject perFile(const QJsonObject& a, Context& ctx, QJsonObject (*one)(const QJsonObject&, Context&),
                    int parallel = 6);

} // namespace Cli::detail
