// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// check.h plus the helpers that render frames or read media files through the engine. Only the tests that use them
// include this one (or the engine headers directly), so a change in Engine.h/Renderer.h/Snapshot.h rebuilds those.

#include "check.h"
#include "engine/Engine.h"
#include "engine/Snapshot.h"
#include "engine/picture/Renderer.h"

namespace Check {

// Frame `pos` of a timeline as the export renders it (RGBA, straight alpha), at `size` (empty = project size);
// transparent = no black background where nothing covers the frame; subtitles = the visible subtitle track burnt in.
// Null image = failed.
inline QImage renderFrame(const Timeline& tl, const ProjectFormat& f, int pos, QSize size = {}, bool transparent = false,
                          bool subtitles = true)
{
    Picture::Options o;
    o.format = f;
    o.mode = Picture::Mode::Export;
    o.transparent = transparent;
    o.subtitles = subtitles;
    if (!size.isEmpty()) o.output = size;
    Picture::Renderer r(tl, o);
    return r.render(pos).toImage();
}

// Frame `pos` of a media file (a written export) in the shape of `f` (RGB32); null = unreadable
inline QImage mediaFrame(const QString& path, const ProjectFormat& f, int pos) { return Snapshot::media(f, path, pos); }
// Length of a media file in frames of `f`
inline int mediaLength(const QString& path, const ProjectFormat& f) { return Engine::probe(f, path).length; }

} // namespace Check
