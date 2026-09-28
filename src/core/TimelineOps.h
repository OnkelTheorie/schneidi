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

// Teilt die Clips an Frame `frame`. Rechte Hälften bekommen neue IDs; verknüpfte
// Paare bleiben verknüpft (neue gemeinsame linkId für die rechten Teile).
// Gibt die IDs der neuen rechten Teile zurück.
QVector<int> splitAt(Timeline& tl, const QVector<int>& clipIds, int frame,
                     const IdGen& newClipId, const IdGen& newLinkId);

// Verschiebt Clips um deltaFrames. trackDelta gilt nur für Clips der Art `trackDeltaKind`
// (so wandert beim Ziehen eines Videoclips das verknüpfte Audio nur zeitlich mit).
void moveClips(Timeline& tl, const QVector<int>& clipIds, int deltaFrames,
               TrackKind trackDeltaKind, int trackDelta, const IdGen& newId);

enum class Edge { Start, End };
// Länge des Quellmaterials in Frames (<= 0 = unbegrenzt, z. B. Standbilder)
using SourceLength = std::function<int(const QString& mediaPath)>;

// Begrenzt ein Trim-Delta so, dass für alle Clips gilt: nicht über das Quellmaterial hinaus,
// mind. 1 Frame lang, nicht vor Frame 0 und nicht in den Nachbarclip hinein (kein Überschreiben).
int clampTrim(const Timeline& tl, const QVector<int>& clipIds, Edge edge, int delta,
              const SourceLength& sourceLength);

// Verschiebt die Kante (Start oder Ende) der Clips um delta. Kein Ripple: nichts rückt nach.
void trimClips(Timeline& tl, const QVector<int>& clipIds, Edge edge, int delta,
               const SourceLength& sourceLength);

int endFrame(const Timeline& tl);

} // namespace TimelineOps
