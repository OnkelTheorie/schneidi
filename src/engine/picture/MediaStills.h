// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Single frames of media files through the own picture engine (0.15): thumbnails, trim view, Media Pool tiles, CLI
// frames of a file. A file is shown as a timeline of one clip (mediaTimeline), fitted into the frame as on the
// timeline at zoom 1 (black bars around another shape). No MLT here.

#include "core/ProjectFormat.h"
#include "core/Types.h"
#include "engine/picture/Renderer.h"

#include <QImage>
#include <QString>

#include <list>
#include <memory>
#include <mutex>
#include <utility>

namespace Picture {

// A media file as a timeline of one clip from its first frame on (no sound)
Timeline mediaTimeline(const QString& path);

// Keeps a renderer per file (the most recently used few: reading near the last frame is much cheaper than opening
// the file again). Thread-safe; meant for one worker thread.
class MediaStills {
public:
    // decodeSource: which file is read for a media file (a proxy if one exists); empty = the original
    MediaStills(const ProjectFormat& format, std::function<QString(const QString&)> decodeSource = {}, int keep = 6);
    // Frame `frame` (project rate) of `path` at `size` (RGBA8888, opaque); null = the file has no picture
    QImage frame(const QString& path, int frame, const QSize& size);
    const ProjectFormat& format() const { return m_format; }

private:
    ProjectFormat m_format;
    std::function<QString(const QString&)> m_decodeSource;
    int m_keep;
    std::mutex m_mutex;
    std::list<std::pair<QString, std::shared_ptr<Renderer>>> m_renderers; // front = most recent
};

} // namespace Picture
