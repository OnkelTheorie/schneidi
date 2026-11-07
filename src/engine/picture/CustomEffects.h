// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Custom picture effects (0.19): when schneidi lacks an effect, an AI (or a user) writes it.
// - "ffmpeg_filter": a libavfilter chain ("hue=s=0,edgedetect") on the clip's picture, frame by frame. Only filters
//   of an allowlist (per-frame colour/shape filters without files, commands or other inputs); the chain is checked
//   (parsed, each filter and option looked at, run once on a test picture) before it is stored. Needs libavfilter
//   (optional build dependency: SCHNEIDI_HAVE_AVFILTER), else "not available in this build".
// - "shader": a GLSL fragment function `vec4 effect(vec2 uv)` compiled at run time with Qt's shader baker (the `qsb`
//   program next to schneidi or in Qt's folders) and drawn on the graphics card (Gpu::Service). No CPU path: without
//   a GPU (or without qsb) the effect is skipped like an unknown one and `check`/`render` say so.
// Both work on 8-bit RGBA with straight alpha, rows without gaps, keep the picture size and are thread-safe.

#include <QColor>
#include <QString>
#include <QStringList>

#include <cstdint>

struct Clip;
struct EffectInstance;

namespace Picture::Custom {

inline constexpr const char* kFilterId = "ffmpeg_filter";
inline constexpr const char* kShaderId = "shader";
inline constexpr const char* kChainKey = "chain";
inline constexpr const char* kCodeKey = "code";

bool isCustom(const QString& effectId);
// Can this machine draw the effect now (false: why)? The filter needs libavfilter, the shader qsb and a GPU
bool canRender(const QString& effectId, QString* why = nullptr);
// Draws a custom effect instance of clip c at clip frame t (its values and keyframes) onto the clip's picture; false =
// not drawn (the reason is logged once per effect and reason; the picture stays as it was)
bool applyToClip(const Clip& c, const EffectInstance& inst, double t, double fps, uint8_t* rgba, int width, int height);

// ---- FFmpeg filter chain ----
bool filterAvailable();
// The filters a chain may use (sorted)
QStringList allowedFilters();
// Empty = the chain is fine (or empty); else why not, ready for the caller (unknown/forbidden filter or option, a loop in
// an expression, a picture much larger than the input, error FFmpeg reports when it runs on a test picture, longer than
// budgetMs for that picture)
QString checkFilter(const QString& chain, int budgetMs = 3000);
// Runs the chain on the picture at clip time `seconds` (the filters' t, so expressions animate); false = not drawn
// (error in *error)
bool applyFilter(const QString& chain, uint8_t* rgba, int width, int height, double seconds, QString* error = nullptr);

// ---- Shader ----
struct ShaderValues {
    double seconds = 0;  // clip time
    double frame = 0;    // clip frame
    double fps = 25;
    double p[8] = {};    // p1..p8
    QColor color1{255, 255, 255}, color2{0, 0, 0};
};
// The text a user shader is put into (uniforms, helpers; documented for `effects effect:shader`)
QString shaderHelp();
// Is a runtime compiler there (false: why)
bool shaderCompilerAvailable(QString* why = nullptr);
// The compiler and a graphics card for it (Gpu::enabled)
bool shaderRenderable(QString* why = nullptr);
// Empty = compiles; else the compiler's message with line numbers of the user's code
QString checkShader(const QString& code);
// Draws the shader over the picture on the GPU; false = not drawn (no GPU, compile error; in *error)
bool applyShader(const QString& code, const ShaderValues& values, uint8_t* rgba, int width, int height,
                 QString* error = nullptr);

} // namespace Picture::Custom
