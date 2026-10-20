#pragma once
// Internal helpers of TimelineBuilder, shared by its implementation files (not part of the API).
#include "core/Keyframes.h"
#include "core/TimelineOps.h"
#include "core/Types.h"
#include "engine/TimelineBuilder.h"

#include <QString>
#include <atomic>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <utility>

namespace Mlt {
class Service;
}

namespace TimelineBuilderDetail {

// Before/after switch of the builder that is currently building (preview only), see TimelineBuilder::setGradeBypass
extern thread_local std::shared_ptr<std::atomic<bool>> t_gradeBypass;

// ---- Clip (TimelineBuilderClip.cpp) ----

// Keyframes (core/Keyframes.h) as MLT animation of a filter value over a section: a = clip frame at the start of the
// section, len = its length. Set at the section bounds and at all keyframes of the parameters (MLT interpolates
// linearly in between); sections with an ease curve (or bake) frame by frame, so preview, export and Inspector have
// exactly the same values. false = none of the parameters is animated.
bool animate(const Clip& c, std::initializer_list<AnimParam> params, int a, int len,
             const std::function<void(int pos, double t)>& set, bool bake = false);
// Attach a filter to a section; keyframes count from the filter's in point (see docs/dev/mlt.md)
void attachTo(Mlt::Producer& cut, Mlt::Filter& f);
// Aspect ratio of the source picture as displayed (pixel aspect and rotation from the metadata), 0 = unknown
double sourceAspect(Mlt::Producer& cut);
// Request the source below a qtblend (distort) only as large as the output needs: requested size times maxZoom
// (qtblend otherwise fetches the full media size and lets Qt shrink it, which is slow and drops fine detail)
void attachSourceCap(Mlt::Producer& clip, double maxZoom);
// Effects, transform, volume, pan of a clip on a section; a/len: section (clip frame at the start, length) for keyframes
void decorate(Mlt::Profile& profile, Mlt::Producer& cut, const Clip& c, TrackKind kind, int a, int len);
// Title clip: transparent full frame + qtext filter; a/len: section for keyframes
Mlt::Producer* titleCut(Mlt::Profile& profile, const Clip& c, int a, int len);
// Apply the clip's fade handles to the section [from, to) (timeline frames)
void applyClipFades(Mlt::Profile& profile, Mlt::Producer& cut, const Clip& c, TrackKind kind, int from, int to);
// One subtitle (track style like a title), len frames long
Mlt::Producer* subtitleCut(Mlt::Profile& profile, const TitleStyle& t, const QString& text, int len);

// ---- Audio (TimelineBuilderAudio.cpp) ----

inline double mltLevel(double db) { return db <= kMinVolumeDb ? -200.0 : db; }
inline double mltPan(double pan) { return (pan + 100.0) / 200.0; }
// a/len: section for keyframes (len = 0: static value only, e.g. mixer)
void applyVolume(Mlt::Profile& profile, Mlt::Service& clip, const Clip& c, int a = 0, int len = 0);
void applyPan(Mlt::Profile& profile, Mlt::Service& clip, const Clip& c, int a = 0, int len = 0);
void setLimiter(Mlt::Filter& f, bool on, double ceilingDb);
void setTrackDsp(Mlt::Filter& f, const TrackEq& eq, const TrackDynamics& dynamics);
std::shared_ptr<Mlt::Filter> makeSubframeDelay(Mlt::Profile& profile, int subframe);
// Short ramp at a hard cut: first frame of the excerpt (rampIn) resp. its last one (rampOut)
void attachEdgeDeclick(Mlt::Profile& profile, Mlt::Producer& cut, bool rampIn, bool rampOut);
// Mixer strip (fader, pan, meter) on a playlist or tractor. limiter: master only (ceiling in dBFS, nullopt = none);
// track: audio tracks only, its EQ and compressor sit before the fader.
void attachStrip(Mlt::Profile& profile, Mlt::Service& s, double db, double pan, MixerHooks::Strip* hook,
                 std::optional<std::pair<bool, double>> limiter = {}, const Track* track = nullptr);

// ---- Transitions (TimelineBuilderTransitions.cpp) ----

// Fade video in/out over len frames via the alpha channel
void applyFade(Mlt::Profile& profile, Mlt::Producer& cut, int len, bool fadeIn);
// Audio transition: level of clip clipId in the transition's section as keyframes in dB per frame
void applyAudioTransition(Mlt::Profile& profile, Mlt::Producer& cut, const TimelineOps::TransitionSpan& s, int clipId);
// Video transition other than Cross Dissolve as a small tractor: a leaves, b enters; a missing side is empty
std::unique_ptr<Mlt::Tractor> styledTransition(Mlt::Profile& profile, Mlt::Producer* a, Mlt::Producer* b, int len,
                                               const TransitionStyle& st);

} // namespace TimelineBuilderDetail
