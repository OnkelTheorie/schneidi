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

// Liegt der Clip auf einer gesperrten Spur (Schloss im Spurkopf)? Unbekannte Clips gelten als nicht gesperrt.
bool isLocked(const Timeline& tl, int clipId);
// Nur die Clips, die nicht auf gesperrten Spuren liegen (Reihenfolge bleibt)
QVector<int> unlocked(const Timeline& tl, const QVector<int>& clipIds);
// Spuren, auf denen die Clips liegen (ohne Doppelte)
QVector<TrackRef> tracksOf(const Timeline& tl, const QVector<int>& clipIds);

// Ripple auf den übrigen Spuren wie DaVinci: alle nicht gesperrten Spuren außer `skip` bleiben synchron.
// shifts: (ab Frame, Versatz) – ein Clip rückt um die Summe der Versätze, deren Frame <= seinem Start ist.
// Würde eine Spur dabei einen stehenbleibenden Clip überschreiben (oder vor Frame 0 rutschen), bleibt sie
// ganz stehen – nie überschreiben.
void rippleTracks(Timeline& tl, const QVector<QPair<int, int>>& shifts, const QVector<TrackRef>& skip);
// Wie weit (Frames nach links) die übrigen Spuren ab `from` nachrücken können, ohne zu überschreiben
int rippleRoom(const Timeline& tl, int from, const QVector<TrackRef>& skip);

// Alle Clips mit gleicher linkId (inkl. des Clips selbst).
QVector<int> linkedGroup(const Timeline& tl, int clipId);

// Überschreibt den Bereich [start, end) auf einer Spur (wie "Overwrite" in DaVinci):
// Clips darin werden gekürzt, geteilt oder entfernt. Nichts rutscht nach.
// Wird ein verknüpfter Clip geteilt, bekommt das rechte Stück vorläufig die linkId -alt (alle Spuren gleich);
// resolvePendingLinks (ruft Project::edit auf) macht daraus eine neue gemeinsame Verknüpfung.
void clearRange(Track& track, int start, int end, const IdGen& newId);
// Vorläufige (negative) linkIds aus clearRange durch neue ersetzen: gleiche vorläufige Id -> gleiche neue Id
void resolvePendingLinks(Timeline& tl, const IdGen& newLinkId);

// Legt einen Clip im Overwrite-Modus auf die Spur.
void placeClip(Track& track, const Clip& clip, const IdGen& newId);

bool removeClip(Timeline& tl, int clipId);

// Einfügen mit Ripple (Insert, F9): auf den Spuren wird der Clip über `frame` geteilt (verknüpfte Teile bleiben
// verknüpft, siehe splitAt) und alles ab `frame` rückt um `length` nach rechts.
void insertGap(Timeline& tl, const QVector<TrackRef>& tracks, int frame, int length,
               const IdGen& newClipId, const IdGen& newLinkId);
// Verschiebt auf der Spur alle Clips mit start >= frame um delta (Ripple; Aufrufer sorgt für Platz)
void shiftFrom(Track& track, int frame, int delta);
// Überblendung am Schnitt bei `frame` entfernen (dort kommt gleich ein neuer Clip hin). Eigenständiges Aus-/Einblenden
// (zu Schwarz bzw. Stille, auch mit Nachbar) bleibt wie in DaVinci am Clip.
void clearDissolveAt(Track& track, int frame);

// Legt fehlende Spuren an, bis es `count` Spuren der Art gibt (Namen V3, A3, …).
void ensureTracks(Timeline& tl, TrackKind kind, int count);

// Löscht Clips mit Ripple: auf den betroffenen Spuren rückt alles Folgende nach (wie Shift+Entf in DaVinci),
// die übrigen nicht gesperrten Spuren rücken um die gelöschten Bereiche mit (rippleTracks).
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

// Trim-Modus (T) wie DaVinci. Alle Varianten lassen die Ansicht in Ruhe und überschreiben nie einen Nachbarn.
enum class TrimKind {
    Ripple, // Kante ziehen, spätere Clips rücken nach (Anfang: Clip bleibt stehen, Inhalt wandert);
            // wie DaVinci auf allen nicht gesperrten Spuren (Verkürzen nur so weit, wie alle nachrücken können)
    Roll,   // Schnitt zwischen zwei Clips verschieben: links Ende, rechts Anfang, Gesamtlänge bleibt
    Slip,   // Inhalt im Clip verschieben (In/Out), Lage und Länge bleiben
    Slide,  // Clip verschieben, linker Nachbar wird länger/kürzer, rechter umgekehrt
};
struct TrimEdit {
    TrimKind kind = TrimKind::Ripple;
    QVector<int> ids;      // Ripple: Clips der Kante; Roll: Clips links vom Schnitt; Slip/Slide: die Clips
    QVector<int> rightIds; // nur Roll: Clips rechts vom Schnitt
    Edge edge = Edge::End; // nur Ripple
    bool isNull() const { return ids.isEmpty() && rightIds.isEmpty(); }
};
// Größtes erlaubtes Delta in Richtung von `delta` (Quellmaterial, mind. 1 Frame, Nachbarn, Frame 0)
int clampTrimEdit(const Timeline& tl, const TrimEdit& e, int delta, const SourceLength& sourceLength);
void applyTrimEdit(Timeline& tl, const TrimEdit& e, int delta, const SourceLength& sourceLength);

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

// Bilden a (links) und b (rechts) eine Überblendung? Anliegend, beide Kanten mit Übergang, keiner eigenständig.
bool isDissolve(const Clip& a, const Clip& b);

// Überblendungen lösen, bei denen nur einer der beiden Clips in `clipIds` liegt
// (vor Löschen/Verschieben, damit der Übrige nicht plötzlich ausblendet).
void detachTransitions(Timeline& tl, const QVector<int>& clipIds);
// Nach einer Bearbeitung: Clip-Seiten, die vorher Teil einer Überblendung waren und jetzt keinen Partner mehr haben
// (auseinandergetrimmt, Partner hat keine Handles mehr, Tempo geändert …), verlieren ihren Übergang – sonst würde
// aus der Überblendung stillschweigend ein Aus-/Einblenden über Schwarz. Neue Clips (andere ids) bleiben unberührt.
void unpairBrokenDissolves(const Timeline& before, Timeline& after);
// Nach einer Bearbeitung: Aus- und Einblenden, die vorher nicht nebeneinander lagen und jetzt aneinanderstoßen
// (Lücke geschlossen, Clip herangeschoben), bleiben zwei Übergänge statt einer Überblendung (wie DaVinci).
// Räumt außerdem die Eigenständig-Markierung auf, wo die Kanten nicht mehr anliegen.
void keepFadesApart(const Timeline& before, Timeline& after);

int endFrame(const Timeline& tl);
// Liegt die Datei irgendwo auf einer Videospur? (z. B. Vorschau nur neu bauen, wenn ein Proxy dazu gehört)
bool usesMediaOnVideo(const Timeline& tl, const QString& path);

} // namespace TimelineOps
