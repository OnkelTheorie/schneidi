// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Analysis and mattes of the cutout node (compositing graph, 0.19): a person in front of anything, without a green
// screen. The model (engine/PersonMatte) runs once over a clip range on request (op cutout_node, later the app), never
// per frame during playback. Its mattes are kept small (kMatteShort pixels on the short side, 8 bit), smoothed over
// time (no flicker) and stored compressed in one cache file per analysis (CacheLocation/cutout/<id>.matte); the node
// records the id (its `cache` value, Editor::cutoutCompNode). Per frame the node only reads its matte and refines it to
// the full picture with a guided filter that follows the picture's own edges (refineMatte).
//
// Pure parts for tests: matteSize, smoothing, grow/blur, refine, the cache file; cutoutFrames takes any frame
// callback and any Segmenter (a stand-in function instead of a model).

#include "core/ProjectFormat.h"
#include "core/Types.h"
#include "engine/PersonMatte.h"

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace Picture::Comp {

inline constexpr const char* kCutoutType = "cutout";
inline constexpr const char* kCutoutCacheKey = "cache"; // the node's value naming its analysis
inline constexpr int kMatteShort = 256;                 // short side of a stored matte (at most the picture's)
inline constexpr int kSmoothRadius = 2;                 // frames on each side the smoothing looks at
inline constexpr int kSmoothTolerance = 96; // a neighbour differing this much (0..255) does not count (motion)

// Size of a stored matte for a picture of `picture` size: its aspect, short side kMatteShort (never bigger)
QSize matteSize(QSize picture, int shortSide = kMatteShort);

// Mattes of frames from, from + 1, … (each w * h bytes, 255 = person)
struct MatteSeq {
    int w = 0, h = 0, from = 0;
    std::vector<std::vector<uint8_t>> frames;
};

// ---- Pure helpers ----
// Bilinear resize of one plane (sw x sh -> dw x dh)
std::vector<uint8_t> resizePlane(const uint8_t* src, int sw, int sh, int dw, int dh);
// One smoothed frame: the frame at `center` of `window` (pointers to w * h planes, in time order) averaged with its
// neighbours, each by a triangle weight, fully while it differs up to tolerance / 2, fading out up to `tolerance` - a
// flicker goes, a real movement (an edge passing: 0 against 255) stays sharp
void smoothFrame(const std::vector<const uint8_t*>& window, int center, size_t pixels, uint8_t* out,
                 int tolerance = kSmoothTolerance);
// All frames smoothed (radius frames on each side)
void smoothMattes(MatteSeq& m, int radius = kSmoothRadius, int tolerance = kSmoothTolerance);
// Grows (px > 0) or shrinks (px < 0) a matte by px pixels (fractions blend), square neighbourhood
void growMatte(std::vector<uint8_t>& m, int w, int h, double px);
// Softens a matte: two box blurs of radius px (rounded to whole pixels)
void blurMatte(std::vector<uint8_t>& m, int w, int h, double px);
// The low matte (lw x lh) at the full picture's size (w x h): a guided filter (He et al.: fast guided filter, the
// linear coefficients computed on the small picture, applied to the big one) with the picture's luma as the guide,
// so the edge sits where the picture has its edge; rgba = the picture (w * h * 4, straight alpha). rgba nullptr =
// plain bilinear. radius in low pixels, eps for luma 0..1
void refineMatte(const uint8_t* low, int lw, int lh, const uint8_t* rgba, int w, int h, uint8_t* out, int radius = 4,
                 float eps = 1e-3f);

// ---- Cache file (CacheLocation/cutout/<id>.matte): header, then each frame qCompress'ed ----
QString cutoutCacheDir();
QString cutoutCachePath(const QString& id);
bool writeMattes(const QString& path, int w, int h, int from, const std::vector<QByteArray>& compressedFrames,
                 QString* error = nullptr);
bool writeMattes(const QString& path, const MatteSeq& m, QString* error = nullptr);
std::optional<MatteSeq> readMattes(const QString& path, QString* error = nullptr);
// Removes an analysis' file (a clip's cache cleanup); true when it is gone
bool removeCutoutCache(const QString& id);

// A loaded cache file: frames stay compressed until asked for
class MatteFile {
public:
    static std::shared_ptr<const MatteFile> load(const QString& path, QString* error = nullptr);
    int w = 0, h = 0, from = 0;
    int count() const { return int(m_frames.size()); }
    bool has(int frame) const { return frame >= from && frame < from + count(); }
    // The matte of clip frame `frame` (w * h); empty when it is not in the file
    std::vector<uint8_t> matte(int frame) const;

private:
    friend std::optional<MatteSeq> readMattes(const QString&, QString*);
    std::vector<QByteArray> m_frames;
};
// The file of analysis `id`, loaded once and kept in memory (a few at a time; a changed file is read again);
// nullptr = no such analysis. Thread-safe
std::shared_ptr<const MatteFile> mattesOf(const QString& id);

// ---- Analysis ----
struct CutoutRun {
    QString cacheId;      // the analysis' id (the node's `cache` value)
    int from = 0, to = 0; // clip frames analysed
    int frames = 0;
    double msPerFrame = 0; // model + smoothing + compression, per frame
    bool canceled = false;
    QString error;         // nothing analysed (translated)
};

// The id of an analysis of node `node` of clip c over from..to by `way` (what makes the mattes): the same analysis
// of the same clip gets the same id (a new run replaces its file)
QString cutoutCacheId(const Clip& c, int node, int from, int to, const QString& way);

// Mattes of frames from..to (from <= to) of frame(t) (any size, all the same) by s, written as analysis `id`.
// progress(done, total) after every frame; cancel stops (nothing written)
CutoutRun cutoutFrames(const std::function<QImage(int frame)>& frame, int from, int to,
                       const PersonMatte::Segmenter& s, const QString& id,
                       const std::function<void(int, int)>& progress = {}, const std::atomic<bool>* cancel = nullptr);

// Cutout node `node` of clip c's "comp" graph over clip frames from..to (swapped when to < from): the clip's own
// picture without bars and without its effects (as the tracker sees it). s = the installed model when not given
CutoutRun cutoutNode(const ProjectFormat& format, const Clip& c, int node, int from, int to,
                     const std::function<void(int, int)>& progress = {}, const std::atomic<bool>* cancel = nullptr,
                     const PersonMatte::Segmenter* s = nullptr);

// Why a cutout node with this `cache` value shows nothing (translated); empty = its analysis is there
QString cutoutHint(const QString& cacheId);

} // namespace Picture::Comp
