// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Every file (and watched folder) a project names, in ONE place: templates copy and redirect them
// (core/TemplateLibrary.h forEachFile), the work folder of a server refuses those outside it (cli/Sandbox.h
// checkProject). A NEW field of the project that holds a file path goes into forEach(), or one of them misses it
// (files of compositing nodes: Comp::fileKeys in core/CompGraph.cpp).

#include <QString>

#include <functional>

struct ProjectData;

namespace ProjectRefs {

enum class Kind {
    ClipMedia,      // the media file of a clip
    TitleTexture,   // the texture picture of a title clip or of a subtitle track's style
    TransitionWipe, // the wipe image of a clip's transition in or out
    EffectFile,     // a file parameter of a clip effect (EffectParam::Path: a LUT)
    CompFile,       // a file of a compositing node (Comp::fileKeys: media, LUT, text texture)
    BrandFile,      // logo, intro or outro of the project's own brand kit
    PoolMedia,      // a Media Pool item
    WatchFolder,    // a watched folder (a folder, not a file)
};

// Calls fn for every path of the project that is set and no LUT, wipe or look bundled with the program (":/luts/<name>",
// ":/lumas/<name>"; only for effect, wipe and compositing node files), in all timelines; fn may change the path in place (the opened timeline's copy in
// `sequences` follows `timeline` afterwards). Not paths: font names, the watch log (a history of names, never opened),
// the render queue's targets (written by the app's Deliver page only).
void forEach(ProjectData& d, const std::function<void(QString& path, Kind kind)>& fn);

} // namespace ProjectRefs
