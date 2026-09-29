#pragma once
// Untertitel (DaVinci Subtitle Tracks): SRT lesen/schreiben und Hilfen für Untertitelspuren.
// Reine Datenfunktionen, bearbeitet wird über den Editor (Undo).

#include "core/Types.h"

#include <QByteArray>
#include <QSize>

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

} // namespace Subtitles
