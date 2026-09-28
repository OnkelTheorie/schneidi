#pragma once

// Reines Datenmodell der Timeline – ohne Qt-Widgets und ohne MLT,
// damit Engine und UI unabhängig davon austauschbar bleiben.

#include "core/I18n.h"

#include <QColor>
#include <QFileInfo>
#include <QMap>
#include <QString>
#include <QVariantMap>
#include <QVector>

enum class TrackKind { Video, Audio };

struct EffectInstance {
    QString effectId;   // Schlüssel in der EffectRegistry, z. B. "chromakey"
    QVariantMap params; // Parameter-Werte (fehlende = Default aus der Registry)
    bool enabled = true;
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

    int length() const { return out - in + 1; }
    int end() const { return start + length(); } // exklusiv
    // Titel: jedes Frame gleich, beliebig lang trimmbar (in darf auch negativ werden)
    bool isTitle() const { return kind == ClipKind::Title; }
    // Name in Timeline/Inspector: Dateiname bzw. erste Textzeile
    QString displayName() const { return isTitle() ? title.firstLine() : QFileInfo(mediaPath).fileName(); }
};

struct Track {
    TrackKind kind = TrackKind::Video;
    QString name;
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
};
