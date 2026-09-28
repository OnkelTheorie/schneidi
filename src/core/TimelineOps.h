#pragma once
// Reine Funktionen auf dem Timeline-Modell. Kein Undo, keine Signale –
// das übernimmt Project::edit(). Dadurch leicht testbar und erweiterbar.

#include "core/Types.h"

#include <functional>

using IdGen = std::function<int()>;

namespace TimelineOps {

// Sucht einen Clip; optional wird die Spur zurückgegeben.
Clip* findClip(Timeline& tl, int clipId, TrackRef* where = nullptr);
const Clip* findClip(const Timeline& tl, int clipId, TrackRef* where = nullptr);

// Alle Clips mit gleicher linkId (inkl. des Clips selbst).
QVector<int> linkedGroup(const Timeline& tl, int clipId);

// Überschreibt den Bereich [start, end) auf einer Spur (wie "Overwrite" in DaVinci):
// Clips darin werden gekürzt, geteilt oder entfernt. Nichts rutscht nach.
void clearRange(Track& track, int start, int end, const IdGen& newId);

// Legt einen Clip im Overwrite-Modus auf die Spur.
void placeClip(Track& track, const Clip& clip, const IdGen& newId);

bool removeClip(Timeline& tl, int clipId);

// Legt fehlende Spuren an, bis es `count` Spuren der Art gibt (Namen V3, A3, …).
void ensureTracks(Timeline& tl, TrackKind kind, int count);

// Löscht Clips mit Ripple: auf den betroffenen Spuren rückt alles Folgende nach (wie Shift+Entf in DaVinci).
void rippleDelete(Timeline& tl, const QVector<int>& clipIds);

// Teilt die Clips an Frame `frame`. Rechte Hälften bekommen neue IDs; verknüpfte
// Paare bleiben verknüpft (neue gemeinsame linkId für die rechten Teile).
// Gibt die IDs der neuen rechten Teile zurück.
QVector<int> splitAt(Timeline& tl, const QVector<int>& clipIds, int frame,
                     const IdGen& newClipId, const IdGen& newLinkId);

// Begrenzt den Spurversatz, sodass alle Clips gleich weit wandern (wie DaVinci: V1->V2 nimmt A1->A2 mit):
// keine Spur unter V1/A1, Spuren der Art `anchorKind` bleiben im vorhandenen Bereich,
// die andere Art darf neue Spuren brauchen (werden beim Verschieben angelegt).
int clampTrackDelta(const Timeline& tl, const QVector<int>& clipIds, TrackKind anchorKind, int trackDelta);

// Verschiebt Clips um deltaFrames und trackDelta Spuren (siehe clampTrackDelta).
void moveClips(Timeline& tl, const QVector<int>& clipIds, int deltaFrames,
               TrackKind anchorKind, int trackDelta, const IdGen& newId);

enum class Edge { Start, End };
// Länge des Quellmaterials eines Clips in Frames, mit Geschwindigkeit umgerechnet (Clip::retimedLength;
// <= 0 = unbegrenzt, z. B. Standbilder)
using SourceLength = std::function<int(const Clip& clip)>;

// Begrenzt ein Trim-Delta so, dass für alle Clips gilt: nicht über das Quellmaterial hinaus,
// mind. 1 Frame lang, nicht vor Frame 0 und nicht in den Nachbarclip hinein (kein Überschreiben).
int clampTrim(const Timeline& tl, const QVector<int>& clipIds, Edge edge, int delta,
              const SourceLength& sourceLength);

// Verschiebt die Kante (Start oder Ende) der Clips um delta. Kein Ripple: nichts rückt nach.
void trimClips(Timeline& tl, const QVector<int>& clipIds, Edge edge, int delta,
               const SourceLength& sourceLength);

// Wirksamer Übergang auf einer Spur (siehe Clip::transIn/transOut)
struct TransitionSpan {
    int leftId = 0;         // Clip, der ausblendet (0 = keiner -> Einblenden aus Schwarz/Stille)
    int rightId = 0;        // Clip, der einblendet (0 = keiner -> Ausblenden)
    int start = 0, end = 0; // Timeline-Frames [start, end)
    int cut = 0;            // Schnitt (bei Einblenden = start, bei Ausblenden = end)
    TransitionStyle style;  // vom linken Clip (transOutStyle), beim Einblenden vom rechten (transInStyle)
    int length() const { return end - start; }
    bool isDissolve() const { return leftId && rightId; }
};

// Übergänge einer Spur mit der Länge, die wirklich passt: Cross Dissolve zentriert auf dem Schnitt,
// gekürzt, wenn Handles (Material über In/Out hinaus) oder Cliplänge nicht reichen; Länge 0 fällt weg.
QVector<TransitionSpan> transitions(const Track& track, const SourceLength& sourceLength);

// Fade-Griff-Rampe (0..1, linear) an Clip-Frame t (ab Clipanfang); Fades auf die Cliplänge begrenzt.
// Video nutzt den Wert als Deckkraft, Audio als Sinus-Kurve (audioFadeGain).
double fadeRamp(const Clip& c, double t);
// Pegel (linear, 0..1) eines Audioclips durch seine Fade-Griffe an Clip-Frame t
double audioFadeGain(const Clip& c, double t);
// Pegel (linear, 0..1) von Clip clipId im Audio-Übergang s an Timeline-Frame frame (1 außerhalb)
double audioTransitionGain(const TransitionSpan& s, int clipId, double frame);

// Überblendungen lösen, bei denen nur einer der beiden Clips in `clipIds` liegt
// (vor Löschen/Verschieben, damit der Übrige nicht plötzlich ausblendet).
void detachTransitions(Timeline& tl, const QVector<int>& clipIds);

int endFrame(const Timeline& tl);
// Liegt die Datei irgendwo auf einer Videospur? (z. B. Vorschau nur neu bauen, wenn ein Proxy dazu gehört)
bool usesMediaOnVideo(const Timeline& tl, const QString& path);

} // namespace TimelineOps
