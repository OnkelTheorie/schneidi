#pragma once

// Reines Datenmodell der Timeline – ohne Qt-Widgets und ohne MLT,
// damit Engine und UI unabhängig davon austauschbar bleiben.

#include "core/I18n.h"

#include <QColor>
#include <QFileInfo>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include <algorithm>

enum class TrackKind { Video, Audio };

// Effekt an einem Clip (Open FX wie DaVinci); jede Art höchstens einmal pro Clip, Reihenfolge = Renderreihenfolge
struct EffectInstance {
    QString effectId;   // Schlüssel in der EffectRegistry, z. B. "chromakey"
    QVariantMap params; // Parameter-Werte (fehlende = Default aus der Registry)
    bool enabled = true;
    bool operator==(const EffectInstance& o) const
    {
        return effectId == o.effectId && params == o.params && enabled == o.enabled;
    }
};

constexpr double kMinVolumeDb = -60.0; // ganz unten = -∞ (stumm)
constexpr double kMaxVolumeDb = 12.0;

// Anzeige wie in DaVinci: "+1.5 dB", "0.0 dB", "-∞ dB"
inline QString formatVolumeDb(double db)
{
    if (db <= kMinVolumeDb) return QStringLiteral("-∞ dB");
    return QString("%1%2 dB").arg(db > 0.05 ? "+" : "").arg(db, 0, 'f', 1);
}

// Transform/Crop/Composite wie im DaVinci-Inspector (Pixelwerte beziehen sich aufs Projektformat)
struct ClipTransform {
    double zoomX = 1.0, zoomY = 1.0;
    double posX = 0, posY = 0;  // Pixel, Y nach oben positiv (wie DaVinci)
    double rotation = 0;        // Grad
    double cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0; // Pixel
    double opacity = 100;       // Prozent
    // Bereiche einzeln abschaltbar (roter Punkt im Inspector); Werte bleiben erhalten
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
};

// Titel (Text-Generator wie in DaVinci): Werte in Pixeln des Projektformats
struct TitleStyle {
    QString text = QStringLiteral("Titel");
    QString font = QStringLiteral("Sans");
    double size = 80;          // Schriftgröße (Pixel)
    QColor color = Qt::white;
    bool bold = false, italic = false;
    int align = 1;             // 0 = links, 1 = Mitte, 2 = rechts (Zeilen innerhalb des Textblocks)
    double posX = 0, posY = 0; // Mitte des Textblocks ab Bildmitte, Y nach oben positiv (wie Transform)
    bool outlineOn = false;    // Umrandung
    QColor outlineColor = Qt::black;
    double outlineWidth = 4;
    bool boxOn = false;        // Hintergrundbox hinter dem Text
    QColor boxColor{0, 0, 0, 160};
    double boxPad = 20;

    QString firstLine() const
    {
        const QString line = text.section('\n', 0, 0).trimmed();
        return line.isEmpty() ? QStringLiteral("Text") : line;
    }
};

// Übergangsart (nur Video; Audio ist immer ein Crossfade) und Lage zum Schnitt wie im DaVinci-Inspector
enum class TransitionType { CrossDissolve, DipToColor, WipeRight, WipeLeft, WipeDown, WipeUp };
enum class TransitionAlign { Center, Start, End }; // Center on Edit / Start on Edit / End on Edit
// Audio-Übergang wie DaVinci: Pegelkurve beider Seiten (in der Mitte -3 / -6 / -9 dB je Seite)
enum class AudioCurve { Plus3dB, Zero, Minus3dB };
struct TransitionStyle {
    TransitionType type = TransitionType::CrossDissolve;
    TransitionAlign align = TransitionAlign::Center;
    QColor color = Qt::black;       // Abblende: Farbe dazwischen ("Dip to Color Dissolve")
    double softness = 0;            // Wischblende: Weichheit der Kante (0..100 %)
    double border = 0;              // Wischblende: Randbreite (Pixel im Projektformat, 0 = kein Rand)
    QColor borderColor = Qt::white; // Wischblende: Randfarbe
    AudioCurve audio = AudioCurve::Plus3dB; // nur Audiospuren

    bool isWipe() const { return type >= TransitionType::WipeRight; }
    bool operator==(const TransitionStyle& o) const
    {
        return type == o.type && align == o.align && color == o.color && softness == o.softness
               && border == o.border && borderColor == o.borderColor && audio == o.audio;
    }
    bool operator!=(const TransitionStyle& o) const { return !(*this == o); }
};
// Reihenfolge = Anzeige in Menüs/Inspector; id = Schlüssel in der Projektdatei
struct TransitionTypeInfo { TransitionType type; const char* id; const char* name; };
inline constexpr TransitionTypeInfo kTransitionTypes[] = {
    {TransitionType::CrossDissolve, "cross_dissolve", "Cross Dissolve"},
    {TransitionType::DipToColor, "dip_color", N_("Abblende über Farbe")},
    {TransitionType::WipeRight, "wipe_right", N_("Wischblende nach rechts")},
    {TransitionType::WipeLeft, "wipe_left", N_("Wischblende nach links")},
    {TransitionType::WipeDown, "wipe_down", N_("Wischblende nach unten")},
    {TransitionType::WipeUp, "wipe_up", N_("Wischblende nach oben")},
};
// Audio-Übergangsarten (Namen wie im DaVinci-Inspector, auch im deutschen DaVinci englisch)
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

enum class ClipKind { Media, Title };

// Keyframes wie in DaVinci: pro Clip und animierbarem Parameter eine Liste. Ohne Keyframes gilt der statische Wert
// (Clip::transform, Clip::title, Clip::volumeDb …). Funktionen dazu in core/Keyframes.h.
enum class AnimParam {
    ZoomX, ZoomY, PosX, PosY, Rotation,               // Transform
    CropLeft, CropRight, CropTop, CropBottom,         // Beschneiden
    Opacity,                                          // Composite
    TitleSize, TitlePosX, TitlePosY, TitleColor,      // Titel (Farbe als ARGB-Zahl)
    Volume, Pan,                                      // Audio
    FxBrightness, FxContrast, FxSaturation, FxTemp, FxTint, // Effekt Farbkorrektur (Werte in EffectInstance::params)
    FxBlur,                                           // Effekt Gaußsche Unschärfe
    Count
};
// Verlauf an einem Keyframe (Rechtsklick auf die Raute wie in DaVinci):
// Ease In = langsam ankommen, Ease Out = langsam losfahren
enum class KeyEase { Linear, EaseIn, EaseOut, EaseInOut };
struct Keyframe {
    int frame = 0;      // Quell-Frame (wie Clip::in) -> Trimmen am Anfang lässt die Keyframes an der Quelle stehen
    double value = 0;
    KeyEase ease = KeyEase::Linear;
    bool operator==(const Keyframe& o) const { return frame == o.frame && value == o.value && ease == o.ease; }
};
using KeyTrack = QVector<Keyframe>; // nach frame sortiert, jeder Frame höchstens einmal

struct Clip {
    int id = 0;
    ClipKind kind = ClipKind::Media;
    QString mediaPath; // leer bei Titeln (kein Medienverweis)
    TitleStyle title;  // nur bei kind == Title
    int start = 0;  // Position in der Timeline (Frames)
    int in = 0;     // erstes Frame im Quellmaterial
    int out = 0;    // letztes Frame im Quellmaterial (inklusive, wie bei MLT)
    int linkId = 0; // 0 = frei; gleiche linkId = verknüpftes Video+Audio
    double volumeDb = 0.0; // Clip-Lautstärke (nur Audio); <= kMinVolumeDb = stumm
    double pan = 0.0;      // Nur Audio: -100 = links, 0 = Mitte, +100 = rechts
    bool enabled = true;   // deaktiviert (Taste D) = unsichtbar/stumm, bleibt aber liegen
    ClipTransform transform; // nur Video
    QVector<EffectInstance> effects;
    // Übergang an Anfang/Ende (Frames, 0 = keiner). Liegt der Nachbar direkt an und hat an der
    // Gegenkante auch einen: Cross Dissolve zentriert auf dem Schnitt (braucht Handles),
    // sonst Ein-/Ausblenden aus Schwarz bzw. Stille. Wirksame Länge: TimelineOps::transitions().
    int transIn = 0, transOut = 0;
    // Art/Ausrichtung dazu; bei einer Überblendung tragen beide Kanten (transOut links, transIn rechts) denselben Wert
    TransitionStyle transInStyle, transOutStyle;
    // Fade-Griffe oben am Clip (Frames, wie DaVinci): Video blendet über Transparenz zur Spur darunter,
    // Audio über die Lautstärke. Unabhängig von Übergängen; beim Rendern auf die Cliplänge begrenzt.
    int fadeIn = 0, fadeOut = 0;
    // Keyframes je Parameter (leer = statischer Wert), siehe core/Keyframes.h
    QMap<AnimParam, KeyTrack> keys;
    // Geschwindigkeit wie DaVinci „Change Clip Speed“ (Strg+R). in/out/Keyframes zählen in Frames des umgerechneten
    // Materials (MLT timewarp): bei 50 % ist die Quelle doppelt so lang, rückwärts beginnt Frame 0 am Dateiende.
    double speed = 1.0;     // 1.0 = 100 %, immer > 0
    bool reverse = false;   // rückwärts abspielen
    bool freeze = false;    // Standbild: jedes Frame zeigt Frame `in` (Ton stumm), beliebig lang ziehbar
    bool keepPitch = true;  // Tonhöhe halten (Pitch Correction)

    bool isRetimed() const { return speed != 1.0 || reverse || freeze; }
    // Länge des umgerechneten Materials aus der Länge der Datei (<= 0 = unbegrenzt)
    int retimedLength(int fileLength) const
    {
        if (fileLength <= 0 || freeze) return 0;
        return std::max(1, int(fileLength / speed));
    }

    int length() const { return out - in + 1; }
    int end() const { return start + length(); } // exklusiv
    // Titel: jedes Frame gleich, beliebig lang trimmbar (in darf auch negativ werden)
    bool isTitle() const { return kind == ClipKind::Title; }
    // Name in Timeline/Inspector: Dateiname bzw. erste Textzeile
    QString displayName() const { return isTitle() ? title.firstLine() : QFileInfo(mediaPath).fileName(); }
};

// Spurfarben wie im DaVinci-Menü „Change Track Color“ (id = Schlüssel in der Projektdatei)
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

struct Track {
    TrackKind kind = TrackKind::Video;
    QString name;  // vom Nutzer vergeben (Doppelklick im Spurkopf); leer = Standard „Video 1“/„Audio 1“
    QString color; // Spurfarbe (id aus kTrackColors); leer = Standard (Video blau, Audio grün)
    bool locked = false; // gesperrt (Schloss im Spurkopf): Clips darauf lassen sich nicht auswählen/ändern
    QVector<Clip> clips; // immer nach start sortiert, ohne Überlappung
    bool muted = false;
    bool hidden = false;
    // Mixer (nur Audio): Spur-Fader, Pan (-100..+100), Solo
    double volumeDb = 0.0;
    double pan = 0.0;
    bool solo = false;
};

struct TrackRef {
    TrackKind kind = TrackKind::Video;
    int index = 0; // V1 = Video/0, A1 = Audio/0
    bool operator==(const TrackRef& o) const { return kind == o.kind && index == o.index; }
};

// Kürzel wie im DaVinci-Spurkopf („V1“, „A2“) und angezeigter Spurname (eigener oder „Video 1“/„Audio 1“)
inline QString trackShortName(TrackRef r)
{
    return QString("%1%2").arg(r.kind == TrackKind::Video ? "V" : "A").arg(r.index + 1);
}
inline QString trackDisplayName(const Track& t, TrackRef r)
{
    if (!t.name.isEmpty()) return t.name;
    return (r.kind == TrackKind::Video ? T("Video %1") : T("Audio %1")).arg(r.index + 1);
}

struct Timeline {
    QVector<Track> video; // [0] = V1 (unterste Spur)
    QVector<Track> audio; // [0] = A1
    QVector<int> markers; // Timeline-Marker (Frames, sortiert)
    int markIn = -1;      // In-/Out-Punkt (I/O), -1 = nicht gesetzt; Out ist das letzte Frame im Bereich
    int markOut = -1;
    double masterVolumeDb = 0.0; // Master-Fader im Mixer

    QVector<Track>& tracks(TrackKind k) { return k == TrackKind::Video ? video : audio; }
    const QVector<Track>& tracks(TrackKind k) const { return k == TrackKind::Video ? video : audio; }
    Track& track(TrackRef r) { return tracks(r.kind)[r.index]; }
    const Track& track(TrackRef r) const { return tracks(r.kind)[r.index]; }
};

struct MediaInfo {
    QString path;
    QString name;
    int length = 0; // Frames in Projekt-Framerate
    bool hasVideo = false;
    bool hasAudio = false;
    bool isImage = false;
    // Quell-In/Out (I/O im Quell-Viewer, wie DaVinci pro Clip im Media Pool gemerkt), -1 = nicht gesetzt;
    // Frames im Quellmaterial, Out ist das letzte Frame im Bereich
    int markIn = -1;
    int markOut = -1;
    // Media-Pool-Organisation (wie DaVinci): Bin (0 = Master), Clipfarbe (id aus kTrackColors, leer = keine;
    // gilt auch für die Clips in der Timeline) und Flags (ids aus kFlagColors, mehrere möglich)
    int bin = 0;
    QString clipColor;
    QStringList flags;
};

// Bin (Ordner) im Media Pool; Master (id 0) steht nicht in der Liste, parent 0 = direkt unter Master
struct MediaBin {
    int id = 0;
    int parent = 0;
    QString name;
    bool operator==(const MediaBin& o) const { return id == o.id && parent == o.parent && name == o.name; }
};

// Flag-Farben wie DaVinci (Media Pool/Timeline Rechtsklick → Flags)
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
