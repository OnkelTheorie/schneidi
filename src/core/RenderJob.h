// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Deliver page like DaVinci: render settings, presets (built-in + own) and render queue.
// Data + JSON only; rendering happens in engine/RenderQueue (via the Exporter).

#include "core/ProjectFormat.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSize>
#include <QString>
#include <QVector>

// What a format writes: a video file, audio only, an animated GIF (rendered losslessly first, then converted with
// ffmpeg's palette filters) or an image sequence (one file per frame in its own folder)
enum class RenderKind { Video, Audio, Gif, ImageSequence };

// Output format = container + codecs (like DaVinci's format/codec pairs, slimmed down)
struct RenderFormatInfo {
    const char* id;         // stored in project/presets
    const char* label;      // display (N_ marks where translated)
    const char* extension;  // file extension
    const char* videoCodec; // "" = audio only
    const char* audioCodec; // "" = no sound; PCM: bit depth comes from RenderSettings::audioBits (ending le/be stays)
    const char* pixFmt;     // pixel format of the video
    RenderKind kind;
    int crf[3];             // CRF of the quality levels high/medium/small; 0 = fixed quality (ProRes, DNxHR, images)
    bool webQuality;        // quality "web" (bit rate cap) available (H.264/H.265)
    bool hardware;          // hardware encoders of the graphics card selectable (H.264/H.265)
    const char* options;    // further encoder settings for MLT's avformat consumer, "key=value key=value"

    bool hasQuality() const { return crf[0] > 0; }
};
const QVector<RenderFormatInfo>& renderFormats();

// Video encoder for H.264/H.265: software (libx264/libx265) or a hardware encoder of the graphics card.
// Auto = the first hardware encoder that works on this computer, else software (engine/HardwareEncoders).
enum class VideoEncoder { Auto = 0, Software, Nvenc, Vaapi, Qsv };
QString videoEncoderId(VideoEncoder e); // "auto", "software", "nvenc", "vaapi", "qsv" (JSON, CLI)
VideoEncoder videoEncoderFromId(const QString& id, VideoEncoder fallback, bool* ok = nullptr);
QString videoEncoderLabel(VideoEncoder e); // display name (translated)
const RenderFormatInfo& renderFormat(const QString& id); // unknown -> H.264

struct RenderSettings {
    QString format = "h264";
    int shortSide = 0; // resolution: 0 = timeline, otherwise shorter picture edge in the timeline's aspect ratio
    QSize size;        // fixed output size (e.g. portrait 1080 x 1920); takes precedence over shortSide
    // 0 = high, 1 = medium, 2 = small, 3 = web: high with a bit rate cap for upload sites (like DaVinci's
    // "Restrict to"); only H.264/H.265
    static constexpr int kQualityWeb = 3;
    int quality = 0;
    int audioBitrateK = 320; // AAC/MP3 only
    int audioBits = 24;      // bit depth for PCM (16, 24, 32 = float) and Apple Lossless (16, 24)
    int rateNum = 0, rateDen = 1; // frame rate of the output, 0 = like the timeline
    // Export subtitles like DaVinci (visible subtitle track): none, burn into the picture, as an SRT file alongside
    enum Subtitles { NoSubtitles = 0, BurnSubtitles = 1, SrtFile = 2 };
    int subtitles = NoSubtitles;
    // New settings default to Auto; saved settings without "encoder" (older versions) stay Software (fromJson)
    VideoEncoder encoder = VideoEncoder::Auto;
    // Smart render (H.264/H.265): copy unchanged stretches of the source instead of encoding them (engine/SmartRender)
    bool smartRender = true;

    bool audioOnly() const;
    bool hasAudio() const;          // GIF and image sequences have no sound
    RenderKind kind() const;
    bool hasEncoderChoice() const; // H.264/H.265: software or hardware encoder
    bool hasAudioBitrate() const;   // AAC/MP3
    QVector<int> audioBitChoices() const; // selectable bit depths (empty = not adjustable)
    QString audioCodec() const;     // FFmpeg codec with the chosen bit depth, e.g. pcm_f32le
    QString audioSampleFormat() const; // Apple Lossless: s16p/s32p, FLAC: s16/s32, otherwise empty
    FrameRate outputRate(FrameRate timeline) const;
    QSize outputSize(QSize timeline) const; // even dimensions
    int crf() const;
    // Bit rate cap in kbit/s for the output (0 = none): quality "web", about 1.3x YouTube's recommendation
    int maxBitrateK(QSize output, FrameRate rate) const;
    QString qualityLabel() const;
    // Short description for the queue, e.g. "H.264 (MP4) · 1920 × 1080 · High"
    QString summary(QSize output) const;

    QJsonObject toJson() const;
    static RenderSettings fromJson(const QJsonObject& o);
    bool operator==(const RenderSettings& o) const;
    bool operator!=(const RenderSettings& o) const { return !(*this == o); }
};

// Image sequences: frames go into a folder named like the file, "<folder>/<name>/<name>_00001.png"
// (path = "<folder>/<name>.png" as for every other format; numbering starts at 1 like ffmpeg)
QString imageSequenceDir(const QString& path);
QString imageSequencePattern(const QString& path); // "…/name/name_%05d.png" for the avformat consumer
QString firstOutputFile(const QString& path, const RenderSettings& s); // the file itself, for sequences the first frame

// Frame rates offered for export only (GIF, smaller files) in addition to kFrameRates
inline constexpr FrameRate kExportOnlyRates[] = {{10, 1}, {12, 1}, {15, 1}};
FrameRate nearestExportRate(double fps);

// Output size with the timeline's aspect ratio and a given shorter edge (even dimensions)
QSize sizeForShortSide(QSize timeline, int shortSide);

struct RenderPreset {
    QString name;
    RenderSettings settings;
    bool builtin = false;
};

namespace RenderPresets {
QVector<RenderPreset> builtins();
// Own presets: JSON in QStandardPaths::AppConfigLocation ("render-presets.json")
QString userFile();
QVector<RenderPreset> loadUser();
bool saveUser(const QVector<RenderPreset>& presets);
QVector<RenderPreset> all(); // built-in, then own
} // namespace RenderPresets

enum class RenderStatus { Queued, Rendering, Done, Failed, Canceled };

// A job of the render queue (as in DaVinci: settings and range are captured when adding,
// the timeline is rendered as it is at render time)
struct RenderJob {
    int id = 0;
    QString path;   // target file (absolute)
    QString preset; // name of the preset when adding (display), empty = own settings
    RenderSettings settings;
    QSize size;          // output size (empty for audio only)
    bool inOut = false;  // In/Out range instead of whole timeline
    int from = 0, to = -1; // range (frames, inclusive), to < 0 = until the end
    int sequence = 0;    // timeline (sequence id) when adding, 0 = the currently opened one (old files)
    RenderStatus status = RenderStatus::Queued;
    QString message;     // error text or note (e.g. hardware encoder fell back to software)
    QString encoderUsed; // FFmpeg video codec of the last render, e.g. "h264_nvenc" (empty = not rendered yet)
    qint64 renderMs = 0; // duration of the last render

    bool operator==(const RenderJob& o) const;
};

namespace RenderQueueJson {
QJsonArray toJson(const QVector<RenderJob>& jobs);
// "rendering" (program quit during rendering) becomes "waiting" again
QVector<RenderJob> fromJson(const QJsonArray& arr);
} // namespace RenderQueueJson

QString renderStatusText(const RenderJob& job); // "Wartet", "Fertig (0:12)", "Fehler: ..."

// Order of the queue (dragging jobs in the queue): the jobs `ids` move in front of the job `beforeId` (0 = to the end),
// in the order they have in the queue. `fixedId` (the job being rendered) never moves; a target among the moved jobs
// means the next job after it that stays. The queue renders waiting jobs from top to bottom (engine/RenderQueue).
QVector<RenderJob> moveRenderJobs(const QVector<RenderJob>& jobs, const QVector<int>& ids, int beforeId, int fixedId = 0);

// File name (without extension) for an export of the timeline `name`, valid on Windows, macOS and Linux and readable:
// ": " becomes " - " (other colons, / and \ a "-"), " an "'", < > | ? * a "_"; control characters are spaces, runs of
// spaces one; no dot, space, "_" or "-" at either end (nothing hidden, no "Teil 2_"), no reserved Windows device name
// (CON, NUL, COM1 … get a "_"), at most 200 characters. Empty -> "Export".
QString renderFileBaseName(const QString& name);
