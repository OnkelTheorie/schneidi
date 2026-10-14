#pragma once

// Pure data model of the timeline - without Qt widgets and without MLT,
// so that engine and UI stay interchangeable independently of it.

#include "core/I18n.h"

#include <QColor>
#include <QHash>
#include <QFileInfo>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include <algorithm>
#include <array>
#include <memory>

enum class TrackKind { Video, Audio };

// Effect on a clip (Open FX like DaVinci); each kind at most once per clip, order = render order
struct EffectInstance {
    QString effectId;   // key in the EffectRegistry, e.g. "chromakey"
    QVariantMap params; // parameter values (missing ones = default from the registry)
    bool enabled = true;
    bool operator==(const EffectInstance&) const = default;
};

constexpr double kMinVolumeDb = -60.0; // very bottom = -inf (muted)
constexpr double kMaxVolumeDb = 12.0;
constexpr double kDefaultLimiterDb = -1.0; // ceiling of the master limiter (dBFS)

// Display as in DaVinci: "+1.5 dB", "0.0 dB", "-inf dB"
inline QString formatVolumeDb(double db)
{
    if (db <= kMinVolumeDb) return QStringLiteral("-∞ dB");
    return QString("%1%2 dB").arg(db > 0.05 ? "+" : "").arg(db, 0, 'f', 1);
}

// Transform/Crop/Composite as in the DaVinci Inspector (pixel values refer to the project format)
struct ClipTransform {
    double zoomX = 1.0, zoomY = 1.0;
    double posX = 0, posY = 0;  // pixels, Y positive upward (like DaVinci)
    double rotation = 0;        // Grad
    double cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0; // Pixel
    double opacity = 100;       // Prozent
    // Sections can be switched off individually (red dot in the Inspector); values are kept
    bool transformOn = true, cropOn = true, compositeOn = true;

    bool hasTransform() const
    {
        return transformOn && (zoomX != 1.0 || zoomY != 1.0 || posX != 0 || posY != 0 || rotation != 0);
    }
    bool hasCrop() const
    {
        return cropOn && (cropLeft != 0 || cropRight != 0 || cropTop != 0 || cropBottom != 0);
    }
    bool hasOpacity() const { return compositeOn && opacity != 100; }
    bool isIdentity() const { return !hasTransform() && !hasCrop() && !hasOpacity(); }
    bool operator==(const ClipTransform&) const = default;
};

// Title (text generator as in DaVinci): values in pixels of the project format
struct TitleStyle {
    QString text = QStringLiteral("Titel");
    QString font = QStringLiteral("Sans");
    double size = 80;          // font size (pixels)
    QColor color = Qt::white;
    bool bold = false, italic = false;
    int align = 1;             // 0 = left, 1 = center, 2 = right (lines within the text block)
    double posX = 0, posY = 0; // center of the text block from picture center, Y positive upward (like Transform)
    bool outlineOn = false;    // outline
    QColor outlineColor = Qt::black;
    double outlineWidth = 4;
    bool boxOn = false;        // background box behind the text
    QColor boxColor{0, 0, 0, 160};
    double boxPad = 20;

    bool operator==(const TitleStyle&) const = default;

    // Font for drawing: "Sans" (default, also stored that way in projects) does not exist on Windows
    QString fontFamily() const
    {
#ifdef Q_OS_WIN
        if (font.isEmpty() || font == QLatin1String("Sans")) return QStringLiteral("Segoe UI");
#endif
        return font;
    }

    QString firstLine() const
    {
        const QString line = text.section('\n', 0, 0).trimmed();
        return line.isEmpty() ? QStringLiteral("Text") : line;
    }
};

// Transition style (video only; audio is always a crossfade) and position relative to the cut as in the DaVinci Inspector
// Luma = gradient wipe with grayscale image (bundled or from the effects folder, TransitionStyle::luma)
enum class TransitionType { CrossDissolve, DipToColor, WipeRight, WipeLeft, WipeDown, WipeUp, Luma };
enum class TransitionAlign { Center, Start, End }; // Center on Edit / Start on Edit / End on Edit
// Audio transition like DaVinci: level curve of both sides (in the middle -3 / -6 / -9 dB per side)
enum class AudioCurve { Plus3dB, Zero, Minus3dB };
struct TransitionStyle {
    TransitionType type = TransitionType::CrossDissolve;
    TransitionAlign align = TransitionAlign::Center;
    QColor color = Qt::black;       // Dip to color: color in between ("Dip to Color Dissolve")
    double softness = 0;            // wipe/gradient wipe: softness of the edge (0..100 %)
    double border = 0;              // wipe: border width (pixels in the project format, 0 = no border)
    QColor borderColor = Qt::white; // wipe: border color
    AudioCurve audio = AudioCurve::Plus3dB; // audio tracks only
    QString luma;                   // gradient wipe: image (file or bundled ":/lumas/<id>")
    bool invert = false;            // gradient wipe: bright areas first

    bool isWipe() const { return type >= TransitionType::WipeRight && type <= TransitionType::WipeUp; }
    bool isLuma() const { return type == TransitionType::Luma; }
    bool operator==(const TransitionStyle&) const = default;
};
// Order = display in menus/Inspector; id = key in the project file
struct TransitionTypeInfo { TransitionType type; const char* id; const char* name; };
inline constexpr TransitionTypeInfo kTransitionTypes[] = {
    {TransitionType::CrossDissolve, "cross_dissolve", "Cross Dissolve"},
    {TransitionType::DipToColor, "dip_color", N_("Abblende über Farbe")},
    {TransitionType::WipeRight, "wipe_right", N_("Wischblende nach rechts")},
    {TransitionType::WipeLeft, "wipe_left", N_("Wischblende nach links")},
    {TransitionType::WipeDown, "wipe_down", N_("Wischblende nach unten")},
    {TransitionType::WipeUp, "wipe_up", N_("Wischblende nach oben")},
    {TransitionType::Luma, "luma", N_("Verlaufsblende")},
};
// Audio transition styles (names as in the DaVinci Inspector, English in the German DaVinci too)
struct AudioCurveInfo { AudioCurve curve; const char* id; const char* name; };
inline constexpr AudioCurveInfo kAudioCurves[] = {
    {AudioCurve::Plus3dB, "plus3", "Cross Fade +3 dB"},
    {AudioCurve::Zero, "zero", "Cross Fade 0 dB"},
    {AudioCurve::Minus3dB, "minus3", "Cross Fade -3 dB"},
};
inline const AudioCurveInfo& audioCurveInfo(AudioCurve c)
{
    for (const auto& i : kAudioCurves)
        if (i.curve == c) return i;
    return kAudioCurves[0];
}

inline const TransitionTypeInfo& transitionTypeInfo(TransitionType t)
{
    for (const auto& i : kTransitionTypes)
        if (i.type == t) return i;
    return kTransitionTypes[0];
}

// Compound: clip whose content is its own timeline (sequence) - compound clip or nested timeline
// as in DaVinci; in/out count in frames of that timeline
enum class ClipKind { Media, Title, Compound };

// Keyframes as in DaVinci: one list per clip and animatable parameter. Without keyframes the static value applies
// (Clip::transform, Clip::title, Clip::volumeDb ...). Functions for it in core/Keyframes.h.
enum class AnimParam {
    ZoomX, ZoomY, PosX, PosY, Rotation,               // Transform
    CropLeft, CropRight, CropTop, CropBottom,         // Crop
    Opacity,                                          // Composite
    TitleSize, TitlePosX, TitlePosY, TitleColor,      // title (color as ARGB number)
    Volume, Pan,                                      // Audio
    FxBrightness, FxContrast, FxSaturation, FxTemp, FxTint, // Effect color correction (values in EffectInstance::params)
    FxBlur,                                           // effect Gaussian blur
    // Color page (effect "grade", DaVinci Primaries): wheels Lift/Gamma/Gain/Offset each Master (Y) + R/G/B,
    // plus contrast, pivot, saturation, temperature, tint, exposure
    GradeLiftY, GradeLiftR, GradeLiftG, GradeLiftB,
    GradeGammaY, GradeGammaR, GradeGammaG, GradeGammaB,
    GradeGainY, GradeGainR, GradeGainG, GradeGainB,
    GradeOffsetY, GradeOffsetR, GradeOffsetG, GradeOffsetB,
    GradeContrast, GradePivot, GradeSaturation, GradeTemp, GradeTint, GradeExposure,
    Count
};
// Interpolation at a keyframe (right-click on the diamond as in DaVinci):
// Ease In = arrive slowly, Ease Out = start slowly, Bezier = free with handles (curve editor)
enum class KeyEase { Linear, EaseIn, EaseOut, EaseInOut, Bezier };
struct Keyframe {
    int frame = 0;      // source frame (like Clip::in) -> trimming at the start leaves the keyframes at the source
    double value = 0;
    KeyEase ease = KeyEase::Linear;
    // Bezier only: handles relative to the keyframe (frames, value); in: dt <= 0, out: dt >= 0.
    // Shortened to the neighboring section when computing (slope stays), see core/Keyframes.cpp.
    double inDt = 0, inDv = 0, outDt = 0, outDv = 0;
    bool operator==(const Keyframe&) const = default;
};
using KeyTrack = QVector<Keyframe>; // sorted by frame, each frame at most once

// Speed ramp like DaVinci "Retime Controls": a speed point splits the clip, from it on its own speed applies.
// The position counts in file frames of the source in playback direction (reversed: 0 = file end) -> trimming/splitting
// leaves the points at their source position. Conversion material <-> source: RetimeMap (core/Retime.h).
struct SpeedPoint {
    double source = 0;  // source position (> 0)
    double speed = 1.0; // speed from here to the next point (> 0)
    int smooth = 0;     // soft transition: length in frames, centered around the point (0 = hard change)
    bool operator==(const SpeedPoint&) const = default;
};

// Project::edit detects edits without change via operator== (snapshot comparison). The comparisons are
// `= default` and capture new fields automatically; only Timeline compares by hand (without `nested`).
struct Clip {
    int id = 0;
    ClipKind kind = ClipKind::Media;
    QString mediaPath; // empty for titles (no media reference)
    TitleStyle title;  // only for kind == Title
    int sequenceId = 0; // only for kind == Compound: sequence (Project::sequence) whose content the clip shows
    int start = 0;  // position in the timeline (frames)
    int in = 0;     // first frame in the source material
    int out = 0;    // last frame in the source material (inclusive, as in MLT)
    int linkId = 0; // 0 = free; same linkId = linked video+audio
    // Audio only: fine position after `start` in 1/100 frame (0..99, like sub-frame in DaVinci Fairlight). Cuts and
    // edges stay on whole frames, only the audio (and its display) shifts right by the fraction.
    int subframe = 0;
    double volumeDb = 0.0; // clip volume (audio only); <= kMinVolumeDb = muted
    double pan = 0.0;      // audio only: -100 = left, 0 = center, +100 = right
    int audioStream = 0;   // audio only: which audio stream of the file (0 = first, see MediaInfo::audioStreams)
    bool enabled = true;   // disabled (key D) = invisible/muted, but stays in place
    ClipTransform transform; // video only
    QVector<EffectInstance> effects;
    // Transition at start/end (frames, 0 = none). If the neighbor is directly adjacent and has one at the
    // opposite edge too: Cross Dissolve centered on the cut (needs handles),
    // otherwise fade in/out from black or silence. Effective length: TimelineOps::transitions().
    int transIn = 0, transOut = 0;
    // A fade out/in that moved to its neighbor when closing a gap stays standalone (like DaVinci:
    // two transitions at the cut instead of a dissolve). Only set while both edges are adjacent and have a transition
    // (TimelineOps::keepFadesApart), otherwise false. See TimelineOps::isDissolve.
    bool transInAlone = false, transOutAlone = false;
    // Style/alignment for it; for a dissolve both edges (transOut left, transIn right) carry the same value
    TransitionStyle transInStyle, transOutStyle;
    // Fade handles at the top of the clip (frames, like DaVinci): video fades via transparency to the track below,
    // audio via volume. Independent of transitions; limited to the clip length when rendering.
    int fadeIn = 0, fadeOut = 0;
    // Keyframes per parameter (empty = static value), see core/Keyframes.h
    QMap<AnimParam, KeyTrack> keys;
    // Speed like DaVinci "Change Clip Speed" (Ctrl+R). in/out/keyframes count in frames of the converted
    // material (MLT timewarp): at 50 % the source is twice as long, reversed frame 0 starts at the file end.
    double speed = 1.0;     // 1.0 = 100 %, always > 0
    bool reverse = false;   // play backwards
    bool freeze = false;    // freeze frame: every frame shows stillFrame() (audio muted), can be stretched to any length
    // Freeze frame: frame shown (converted material like `in`), fixed when switched on so that splitting/trimming
    // (shifts `in`) does not change the picture. -1 = `in` (older project files)
    int freezeFrame = -1;
    int stillFrame() const { return freezeFrame >= 0 ? freezeFrame : in; }
    bool keepPitch = true;  // keep pitch (pitch correction)
    // Speed ramp: further sections (sorted by source); `speed` applies up to the first point. Empty = constant.
    QVector<SpeedPoint> ramp;
    // Render cache clip output (DaVinci "Render Cache Clip Output"): preview plays a pre-rendered file
    // with all effects, see engine/RenderCache.h
    bool renderCache = false;

    bool isRetimed() const { return speed != 1.0 || reverse || freeze || !ramp.isEmpty(); }
    bool hasRamp() const { return !ramp.isEmpty() && !freeze; }
    // Length of the converted material from the file length (<= 0 = unlimited), see core/Retime.cpp
    int retimedLength(int fileLength) const;

    int length() const { return out - in + 1; }
    int end() const { return start + length(); } // exclusive
    double pos() const { return start + subframe / 100.0; } // with fine position (drawing)
    double endPos() const { return end() + subframe / 100.0; }
    bool operator==(const Clip&) const = default;
    // Title: every frame identical, trimmable to any length (in may also become negative)
    bool isTitle() const { return kind == ClipKind::Title; }
    bool isCompound() const { return kind == ClipKind::Compound; }
    // Name in timeline/Inspector: file name or first text line; compound clips carry the name of their sequence
    // (Project::clipName returns it, only a placeholder here)
    QString displayName() const
    {
        if (isCompound()) return T("Compound Clip");
        return isTitle() ? title.firstLine() : QFileInfo(mediaPath).fileName();
    }
};

// Track colors as in the DaVinci menu "Change Track Color" (id = key in the project file)
struct TrackColorInfo { const char* id; const char* name; QRgb rgb; };
inline constexpr TrackColorInfo kTrackColors[] = {
    {"orange", N_("Orange"), 0xffeb6e01},   {"apricot", N_("Aprikose"), 0xffffa833},
    {"yellow", N_("Gelb"), 0xffe2a902},     {"lime", N_("Limette"), 0xff9fc613},
    {"olive", N_("Oliv"), 0xff5f9a1f},      {"green", N_("Grün"), 0xff448f65},
    {"teal", N_("Petrol"), 0xff00989a},     {"navy", N_("Marineblau"), 0xff15628e},
    {"blue", N_("Blau"), 0xff4a83c8},       {"purple", N_("Lila"), 0xff9a71c7},
    {"violet", N_("Violett"), 0xffd0569e},  {"pink", N_("Rosa"), 0xffe9a0c3},
    {"tan", N_("Hellbraun"), 0xffb9af97},   {"beige", N_("Beige"), 0xffc4a06a},
    {"brown", N_("Braun"), 0xff996633},     {"chocolate", N_("Schokolade"), 0xff8c5a3f},
};
inline const TrackColorInfo* trackColorInfo(const QString& id)
{
    for (const auto& i : kTrackColors)
        if (id == QLatin1String(i.id)) return &i;
    return nullptr;
}

// EQ per audio track like Fairlight: 6 bands (high pass, low shelf, 2 bells, high shelf, low pass), the kind is fixed
// per band. Default flat: filters off, the other bands at 0 dB (DSP and response in core/Equalizer.h)
struct EqBand {
    enum class Kind { HighPass, LowShelf, Bell, HighShelf, LowPass };
    Kind kind = Kind::Bell;
    bool on = true;
    double freq = 1000; // Hz: corner (filters, shelves) or center (bell)
    double gainDb = 0;  // shelves and bells only
    double q = 1.0;
    bool operator==(const EqBand&) const = default;
};
struct TrackEq {
    bool on = false;
    std::array<EqBand, 6> bands{{
        {EqBand::Kind::HighPass, false, 30, 0, 0.707},
        {EqBand::Kind::LowShelf, true, 100, 0, 0.707},
        {EqBand::Kind::Bell, true, 500, 0, 1.0},
        {EqBand::Kind::Bell, true, 2000, 0, 1.0},
        {EqBand::Kind::HighShelf, true, 8000, 0, 0.707},
        {EqBand::Kind::LowPass, false, 18000, 0, 0.707},
    }};
    bool operator==(const TrackEq&) const = default;
};

// Compressor per audio track (Fairlight "Dynamics"): stereo-linked, after the EQ and before the fader
// (DSP in core/Dynamics.h)
struct TrackDynamics {
    bool on = false;
    double thresholdDb = -20;
    double ratio = 4;
    double attackMs = 10;
    double releaseMs = 150;
    double kneeDb = 6;
    double makeupDb = 0;
    bool operator==(const TrackDynamics&) const = default;
};

struct Track {
    TrackKind kind = TrackKind::Video;
    QString name;  // assigned by the user (double-click in the track header); empty = default "Video 1"/"Audio 1"
    QString color; // track color (id from kTrackColors); empty = default (video blue, audio green)
    bool locked = false; // locked (lock in the track header): clips on it cannot be selected/changed
    QVector<Clip> clips; // always sorted by start, without overlap
    bool muted = false;
    bool hidden = false;
    // Mixer (audio only): track fader, pan (-100..+100), solo
    double volumeDb = 0.0;
    double pan = 0.0;
    bool solo = false;
    // Audio only: EQ -> compressor -> fader/pan
    TrackEq eq;
    TrackDynamics dynamics;
    bool operator==(const Track&) const = default;
};

// Subtitles like DaVinci: own subtitle tracks (ST1, ST2 ...) above the video tracks. One entry = text from
// start to end (exclusive, timeline frames); font/color/position apply to the whole track (DaVinci "Track Style").
struct SubtitleCue {
    int id = 0; // from the same counter as clip IDs (so the selection can tell clips and subtitles apart)
    int start = 0;
    int end = 0; // exclusive
    QString text;
    int length() const { return end - start; }
    bool operator==(const SubtitleCue&) const = default;
};

// Style of a subtitle track: like a title, but posY = distance of the text's bottom edge from the bottom of the picture
// (pixels in the project format), lines wrap at 90 % of the picture width. Default for 1080 pixel picture height,
// new tracks convert it to the project format (subtitleDefaultStyle in core/Subtitles.h).
inline TitleStyle subtitleBaseStyle()
{
    TitleStyle s;
    s.text.clear();
    s.size = 48;
    s.posY = 60;
    s.outlineWidth = 3; // outline off: quickly looks too thick in the shrunken preview
    s.boxOn = true;     // readable on any picture: semi-transparent box
    s.boxColor = QColor(0, 0, 0, 170);
    s.boxPad = 10;
    return s;
}

struct SubtitleTrack {
    QString name;          // empty = default "Untertitel 1"
    bool enabled = true;   // visible in the viewer/when burning in; like DaVinci only one track at a time
    bool locked = false;
    TitleStyle style = subtitleBaseStyle();
    QVector<SubtitleCue> cues; // sorted by start, without overlap
    bool operator==(const SubtitleTrack&) const = default;
};

struct TrackRef {
    TrackKind kind = TrackKind::Video;
    int index = 0; // V1 = Video/0, A1 = Audio/0
    bool operator==(const TrackRef&) const = default;
};

// Abbreviation as in the DaVinci track header ("V1", "A2") and displayed track name (own or "Video 1"/"Audio 1")
inline QString trackShortName(TrackRef r)
{
    return QString("%1%2").arg(r.kind == TrackKind::Video ? "V" : "A").arg(r.index + 1);
}
inline QString subtitleTrackDisplayName(const SubtitleTrack& t, int index)
{
    return t.name.isEmpty() ? T("Untertitel %1").arg(index + 1) : t.name;
}
inline QString trackDisplayName(const Track& t, TrackRef r)
{
    if (!t.name.isEmpty()) return t.name;
    return (r.kind == TrackKind::Video ? T("Video %1") : T("Audio %1")).arg(r.index + 1);
}

struct Timeline;
// Content of the sequences (id -> timeline) for nested clips when rendering
using NestedTimelines = QHash<int, Timeline>;

struct Timeline {
    QVector<Track> video; // [0] = V1 (lowest track)
    QVector<Track> audio; // [0] = A1
    QVector<SubtitleTrack> subtitles; // [0] = ST1 (lowest subtitle track, directly above the video tracks)
    QVector<int> markers; // timeline markers (frames, sorted)
    int markIn = -1;      // in/out point (I/O), -1 = not set; out is the last frame in the range
    int markOut = -1;
    double masterVolumeDb = 0.0; // master fader in the mixer
    // Limiter on the master (mixer button "LIM"): keeps peaks below the ceiling, preview and export the same.
    // Off by default like DaVinci (bus dynamics are off there; otherwise it hides the overload warning).
    bool masterLimiter = false;
    double masterLimiterDb = kDefaultLimiterDb;
    // For rendering only (Project::renderTimeline): content of all sequences that compound clips refer to.
    // Never saved and not part of undo; the truth is the sequences in the Project.
    std::shared_ptr<const NestedTimelines> nested;

    QVector<Track>& tracks(TrackKind k) { return k == TrackKind::Video ? video : audio; }
    const QVector<Track>& tracks(TrackKind k) const { return k == TrackKind::Video ? video : audio; }
    Track& track(TrackRef r) { return tracks(r.kind)[r.index]; }
    const Track& track(TrackRef r) const { return tracks(r.kind)[r.index]; }
    // Same cut (without `nested`, which only serves rendering).
    // By hand: enter new fields here, otherwise changes to them do not end up in undo
    bool operator==(const Timeline& o) const
    {
        return video == o.video && audio == o.audio && subtitles == o.subtitles && markers == o.markers
               && markIn == o.markIn && markOut == o.markOut && masterVolumeDb == o.masterVolumeDb
               && masterLimiter == o.masterLimiter && masterLimiterDb == o.masterLimiterDb;
    }
};

// Sequence as in DaVinci: own timeline in the project (visible in the Media Pool) or content of a compound clip.
// All share project format, media and clip ids.
struct Sequence {
    int id = 0;
    QString name;
    bool compound = false; // compound clip (not in the timeline selection, unless currently opened)
    int bin = 0;           // bin in the Media Pool (0 = Master)
    Timeline timeline;
};

// An audio stream of a file (OBS writes e.g. desktop audio and microphone separately into one file)
struct AudioStreamInfo {
    int channels = 2;
    QString title; // stream title from the file (empty = none)
    bool operator==(const AudioStreamInfo&) const = default;
};

// Speech-to-text of one audio stream of a media file (whisper, Transcript panel): words with times in ms of the
// source file, so the transcript follows the clips through every edit (core/TranscriptCut)
struct TranscriptWord {
    qint64 from = 0, to = 0; // ms; `to` reaches up to the next word (whisper counts pauses to the word before)
    QString text;            // without leading space, punctuation attached ("Americans,")
    bool operator==(const TranscriptWord&) const = default;
};
struct MediaTranscript {
    int audioStream = 0;
    QString language; // "de", "en", …
    QVector<TranscriptWord> words;
    bool operator==(const MediaTranscript&) const = default;
};

struct MediaInfo {
    QString path;
    QString name;
    int length = 0; // frames in the project frame rate
    bool hasVideo = false;
    bool hasAudio = false;
    bool isImage = false;
    // Source in/out (I/O in the source viewer, remembered per clip in the Media Pool like DaVinci), -1 = not set;
    // frames in the source material, out is the last frame in the range
    int markIn = -1;
    int markOut = -1;
    // Media Pool organization (like DaVinci): bin (0 = Master), clip color (id from kTrackColors, empty = none;
    // also applies to the clips in the timeline) and flags (ids from kFlagColors, several possible)
    int bin = 0;
    QString clipColor;
    QStringList flags;
    // Audio streams in file order (at the end because of aggregate initialization; empty for older project files: then 1 stream if hasAudio)
    QVector<AudioStreamInfo> audioStreams;
    int audioStreamCount() const { return hasAudio ? std::max<int>(1, audioStreams.size()) : 0; }
    // Display name of a stream: title from the file, otherwise "Track N"
    QString audioStreamName(int stream) const
    {
        const QString t = stream < audioStreams.size() ? audioStreams[stream].title : QString();
        return t.isEmpty() ? T("Spur %1").arg(stream + 1) : t;
    }
    // Track label for a stream placed on its own audio track: the file's stream title, but only for multi-stream
    // files with a real title (otherwise empty = the track keeps its default "Audio N")
    QString audioStreamTrackLabel(int stream) const
    {
        if (audioStreamCount() < 2 || stream >= audioStreams.size()) return {};
        return audioStreams[stream].title.trimmed();
    }
    QVector<MediaTranscript> transcripts; // at most one per audio stream
    const MediaTranscript* transcript(int stream) const
    {
        for (const MediaTranscript& t : transcripts)
            if (t.audioStream == stream) return &t;
        return nullptr;
    }
};

// Bin (folder) in the Media Pool; Master (id 0) is not in the list, parent 0 = directly under Master
struct MediaBin {
    int id = 0;
    int parent = 0;
    QString name;
    bool operator==(const MediaBin&) const = default;
};

// Flag colors like DaVinci (Media Pool/Timeline right-click -> Flags)
struct FlagColorInfo { const char* id; const char* name; QRgb rgb; };
inline constexpr FlagColorInfo kFlagColors[] = {
    {"blue", N_("Blau"), 0xff3f8fe8},        {"cyan", N_("Cyan"), 0xff2cc6d9},
    {"green", N_("Grün"), 0xff4fb04a},       {"yellow", N_("Gelb"), 0xffe8c42a},
    {"red", N_("Rot"), 0xffe23b3b},          {"pink", N_("Pink"), 0xffe86fb2},
    {"purple", N_("Lila"), 0xff8e5bd0},      {"fuchsia", N_("Fuchsia"), 0xffc23ab4},
    {"rose", N_("Rosé"), 0xffe89a9a},        {"lavender", N_("Lavendel"), 0xffa99be0},
    {"sky", N_("Himmelblau"), 0xff8cc8f0},   {"mint", N_("Minze"), 0xff8ee0b0},
    {"lemon", N_("Zitrone"), 0xffe8e87a},    {"sand", N_("Sand"), 0xffc8a878},
    {"cocoa", N_("Kakao"), 0xff8a6448},      {"cream", N_("Creme"), 0xfff0e6c8},
};
inline const FlagColorInfo* flagColorInfo(const QString& id)
{
    for (const auto& i : kFlagColors)
        if (id == QLatin1String(i.id)) return &i;
    return nullptr;
}
