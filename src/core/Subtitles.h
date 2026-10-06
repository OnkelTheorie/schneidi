#pragma once
// Untertitel (DaVinci Subtitle Tracks): SRT lesen/schreiben und Hilfen für Untertitelspuren.
// Reine Datenfunktionen, bearbeitet wird über den Editor (Undo).

#include "core/Types.h"

#include <QByteArray>
#include <QPair>
#include <QSize>

#include <functional>

namespace Subtitles {

// SRT-Datei -> Einträge in Timeline-Frames (fps = Projekt-Framerate, z. B. 29.97). ids bleiben 0.
// Kodierung UTF-8 (mit/ohne BOM), sonst Windows-1252/Latin-1; Formatierungen (<i>, {\an8}) werden entfernt,
// kaputte Blöcke übersprungen (skipped = Anzahl). Sortiert, Überlappungen am Ende des vorigen gekürzt.
QVector<SubtitleCue> parseSrt(const QByteArray& data, double fps, int* skipped = nullptr);
// Einträge -> SRT (UTF-8, CRLF wie die meisten Programme erwarten). offset wird von allen Zeiten abgezogen
// (Export eines In/Out-Bereichs beginnt bei 00:00:00,000); Einträge davor fallen weg bzw. werden gekürzt.
QByteArray toSrt(const QVector<SubtitleCue>& cues, double fps, int offset = 0, int until = -1);
QString srtTime(qint64 ms); // "HH:MM:SS,mmm"

// Standardstil für ein Projektformat (Größen relativ zur Bildhöhe, Grundlage 1080 Pixel)
TitleStyle defaultStyle(QSize format);

// Eintrag auf die Spur legen; überschreibt, was dort liegt (wie Clips: ganz verdeckte fallen weg,
// angeschnittene werden gekürzt bzw. geteilt). Spur bleibt sortiert.
void place(SubtitleTrack& track, const SubtitleCue& cue);
// Eintrag per id suchen (trackIndex = Spur), nullptr = keiner
const SubtitleCue* find(const Timeline& tl, int id, int* trackIndex = nullptr);
SubtitleCue* find(Timeline& tl, int id, int* trackIndex = nullptr);
// Index des Eintrags unter dem Frame, -1 = keiner
int cueAt(const SubtitleTrack& track, int frame);
// Letztes Frame (exklusiv) aller Untertitel
int endFrame(const Timeline& tl);

// ---- Ripple/Teilen (Untertitelspuren folgen den Clip-Spuren wie in DaVinci) ----
// Teilt den Eintrag, der über `frame` liegt (start < frame < end): links behält die id, rechts bekommt newId(),
// beide den Text. Rückgabe: id des rechten Teils, 0 = dort lag keiner.
int splitAt(SubtitleTrack& track, int frame, const std::function<int()>& newId);
// Wie TimelineOps::rippleTracks für eine Untertitelspur: ein Eintrag rückt um die Summe der Versätze, deren Frame
// <= seinem Start ist. Würde er dabei einen stehenbleibenden überlappen oder vor Frame 0 rutschen, bleibt die ganze
// Spur stehen (nie überschreiben). Rückgabe: true = verschoben.
bool ripple(SubtitleTrack& track, const QVector<QPair<int, int>>& shifts);
// Wie weit (Frames nach links) die Einträge ab `from` nachrücken können, ohne zu überschreiben (-1 = keiner rückt)
int rippleRoom(const SubtitleTrack& track, int from);
// Einfügen (Insert): Eintrag über `frame` teilen, alles ab `frame` um `length` nach rechts
void insertGap(SubtitleTrack& track, int frame, int length, const std::function<int()>& newId);

} // namespace Subtitles
