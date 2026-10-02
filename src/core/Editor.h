#pragma once
#include "core/TimelineOps.h"
#include "core/Types.h"

#include <QHash>
#include <QObject>
#include <algorithm>
#include <functional>
#include <optional>

class Project;
class Selection;
class RetimeMap;

// Bearbeitungs-Operationen auf Nutzerebene (was ein Klick/Shortcut auslöst).
// UI ruft nur Editor-Methoden auf, nie direkt TimelineOps -> neue Werkzeuge
// werden hier ergänzt, ohne UI und Engine anzufassen.
class Editor : public QObject {
    Q_OBJECT
public:
    Editor(Project* project, Selection* selection, QObject* parent = nullptr);

    Project* project() const { return m_project; }
    Selection* selection() const { return m_selection; }

    bool linkedSelection() const { return m_linkedSelection; }
    void setLinkedSelection(bool on) { m_linkedSelection = on; }
    // Schnitt am Playhead: an = auf allen Spuren der Auswahl (auch der nächste Clip daneben),
    // aus = nur die ausgewählten Clips. Ohne Auswahl immer alle Clips unter dem Playhead.
    bool splitOnSelectedTracks() const { return m_splitOnSelectedTracks; }
    void setSplitOnSelectedTracks(bool on) { m_splitOnSelectedTracks = on; }

    // Clip + verknüpfte Partner (falls "Linked Selection" an ist); nie Clips auf gesperrten Spuren
    QVector<int> withLinked(const QVector<int>& ids) const;
    // Nur Clips, die sich bearbeiten lassen (nicht auf gesperrten Spuren)
    QVector<int> editable(const QVector<int>& ids) const;
    bool isTrackLocked(TrackRef ref) const;

    // Legt Media ab: Video auf V[track], Audio auf A[track] (wie DaVinci, V2 <-> A2).
    // Fehlende Spuren werden angelegt. Mehrere Dateien landen hintereinander.
    void addMediaAt(const QStringList& paths, int frame, int track = 0);
    // Titel (5 s, wie DaVinci "Text") am Frame auf die unterste Videospur, die dort über allen Clips frei ist
    // (fehlende Spur wird angelegt); track >= 0 = feste Spur (Drag aus dem Media Pool). Neuer Clip wird ausgewählt.
    void addTitle(int frame, int track = -1);
    void toggleTrackMute(TrackRef ref);
    void toggleTrackHidden(TrackRef ref);
    // Spur sperren (Schloss wie DaVinci): Clips darauf lassen sich nicht mehr auswählen oder ändern,
    // Ripple lässt die Spur stehen. Ausgewählte Clips der Spur werden abgewählt.
    void toggleTrackLock(TrackRef ref);
    // Spurname (leer = Standard „Video 1“ usw.) und Spurfarbe (id aus kTrackColors, leer = Standard)
    void renameTrack(TrackRef ref, const QString& name);
    void setTrackColor(TrackRef ref, const QString& colorId);
    // Spuren hinzufügen/löschen (DaVinci „Add Track“/„Delete Track“), je ein Undo-Schritt. addTrack: neue leere
    // Spur an Index `index` (0 = ganz unten bei Video bzw. A1 bei Audio), die übrigen rücken auf. Eigene Namen und
    // Eigenschaften bleiben an ihrer Spur, Standardnamen („Video 2“) folgen der Position. Die letzte Spur eines Typs
    // und gesperrte Spuren lassen sich nicht löschen; Clips darauf werden mitgelöscht. Zielspuren rücken mit.
    void addTrack(TrackKind kind, int index);
    bool canRemoveTrack(TrackRef ref) const;
    void removeTrack(TrackRef ref);
    // Spur innerhalb ihres Typs an Index `to` verschieben (Ziehen am Spurkopf); Clips, Name, Mute/Solo/Pegel usw.
    // ziehen mit, Zielspuren folgen. Ein Undo-Schritt.
    void moveTrack(TrackKind kind, int from, int to);
    void moveClips(const QVector<int>& ids, int deltaFrames, TrackKind kind, int trackDelta);
    // Verschieben in 1/100 Frame (Feinposition von Ton-Clips, Clip::subframe); ganze Frames wie moveClips
    void moveClipsFine(const QVector<int>& ids, int fine, TrackKind kind, int trackDelta);
    // Kante eines Clips (und verknüpfter Partner) ziehen, ohne Ripple
    void trimClip(int clipId, TimelineOps::Edge edge, int delta);
    // Wie weit sich die Kante tatsächlich bewegen lässt (für die Live-Vorschau)
    int clampTrim(int clipId, TimelineOps::Edge edge, int delta) const;
    // Trim-Modus (T): Bearbeitung zum gegriffenen Clip bauen (mit verknüpften Partnern).
    // Ripple: Kante `edge`; Roll: Schnitt an dieser Kante (Clips beider Seiten auf den Spuren der Partner); Slip/Slide: Clip.
    TimelineOps::TrimEdit trimEdit(TimelineOps::TrimKind kind, int clipId, TimelineOps::Edge edge = TimelineOps::Edge::End) const;
    int clampTrimEdit(const TimelineOps::TrimEdit& e, int delta) const;
    Timeline previewTrimEdit(const TimelineOps::TrimEdit& e, int delta) const; // für die Live-Vorschau
    void applyTrimEdit(const TimelineOps::TrimEdit& e, int delta);
    // Trim-Modus + , / . wie DaVinci: ausgewählte Clips um `frames` slippen
    void slipSelection(int frames);
    // Lautstärke eines Audioclips (dB, wird auf kMinVolumeDb..kMaxVolumeDb begrenzt)
    void setClipVolume(int clipId, double db);
    // Audiopegel normalisieren (DaVinci Normalize Audio Levels, Sample Peak): peakDb = gemessener Spitzenpegel je
    // Audioclip (ohne Clip-Lautstärke). Clip-Lautstärke so setzen, dass die Spitze bei targetDb liegt; relative = alle
    // Clips um denselben Betrag (lautester trifft das Ziel), sonst jeder einzeln. Lautstärke-Keyframes: Kurve so
    // verschieben, dass ihr höchster Punkt den Wert bekommt. Stille/gesperrte Clips bleiben. Ein Undo-Schritt.
    // Lautheit (LUFS, BS.1770) genauso: peakDb = integrierte Lautheit je Clip; relativeRef = Bezugswert für
    // relative (z. B. gemeinsame Lautheit aller Clips), ohne = lautester Clip.
    void normalizeAudio(const QHash<int, double>& peakDb, double targetDb, bool relative,
                        std::optional<double> relativeRef = std::nullopt);
    // Audioclips zum Normalisieren: Auswahl (mit verknüpften Partnern), nur Ton, nicht gesperrt
    QVector<int> selectedAudioClips() const;
    // Beliebige Clip-Eigenschaft ändern (Inspector). Gleicher mergeKey = ein Undo-Schritt.
    void modifyClips(const QVector<int>& ids, const QString& text, const std::function<void(Clip&)>& fn,
                     const QString& mergeKey = {});
    void bladeAt(int clipId, int frame);
    // Fade-Griff ziehen (nur dieser Clip, wie DaVinci); frames wird auf die Cliplänge begrenzt
    void setClipFade(int clipId, TimelineOps::Edge edge, int frames, const QString& mergeKey = {});
    void splitAtPlayhead(int frame);
    void deleteSelection();
    void rippleDeleteSelection();
    void selectAll();
    // Clips ab Playhead wählen (DaVinci Y / Strg+Y / Alt+Y / Strg+Alt+Y): forward = Playhead und rechts davon,
    // sonst Playhead und links davon. allTracks = alle Spuren, sonst die Spuren der Auswahl (ohne Auswahl: Zielspuren)
    void selectFromPlayhead(int frame, bool forward, bool allTracks);
    // Auswahl um `frames` verschieben (Nudge, wie , und . in DaVinci)
    void nudgeSelection(int frames);
    // Anfang/Ende bis zum Playhead trimmen (Shift+[ / Shift+]); Auswahl oder Clips unter dem Playhead
    void trimToPlayhead(TimelineOps::Edge edge, int frame);
    void toggleSelectionEnabled();
    // Render-Cache Clip-Ausgabe (wie DaVinci) für die ausgewählten Videoclips: sind alle markiert, aus, sonst an.
    // Titel werden nicht gecacht. State: 0 = keiner markiert, 1 = alle, 2 = gemischt, -1 = keine passenden Clips
    void toggleSelectionRenderCache();
    int selectionRenderCacheState() const;
    // Ausgewählte Clips verknüpfen, bzw. trennen, wenn sie schon verknüpft sind
    void toggleLinkSelection();
    void toggleMarker(int frame);
    // In-/Out-Punkt (I/O wie DaVinci); -1 = entfernen. Liegt Out vor In, wird der andere Punkt verworfen.
    void setMarkIn(int frame);
    void setMarkOut(int frame);
    void clearMarks();

    // Übergänge (Cross Dissolve, Strg+T) mit Standardlänge 1 s: an beiden Kanten der ausgewählten Clips,
    // ohne Auswahl am Schnitt, der dem Playhead am nächsten liegt. Zu wenig Handles -> kürzer.
    // style gesetzt (Doppelklick in der Effects Library): diese Art, vorhandene werden ersetzt;
    // onlyKind = nur Kanten auf Video- bzw. Audiospuren. Ist ein Übergang ausgewählt, bekommt er die Art.
    void addTransitions(int frame, std::optional<TransitionStyle> style = {}, std::optional<TrackKind> onlyKind = {});
    // Übergang an einem Schnitt (Drop aus der Effects Library): leftId endet, rightId beginnt dort
    // (0 = keiner -> Ein-/Ausblenden). Standardlänge 1 s, ersetzt einen vorhandenen; ein Undo-Schritt.
    void addTransitionAt(int leftId, int rightId, const TransitionStyle& style);
    // Wie addTransitionAt, ändert aber nichts: wirksamer Übergang (für die Vorschau beim Ziehen)
    std::optional<TimelineOps::TransitionSpan> previewTransitionAt(int leftId, int rightId,
                                                                   const TransitionStyle& style) const;
    // Übergang zwischen leftId und rightId (0 = Schwarz/Stille) entfernen bzw. Länge ändern
    void removeTransition(int leftId, int rightId);
    void setTransitionLength(int leftId, int rightId, int length, const QString& mergeKey = {});
    void setTransitionStyle(int leftId, int rightId, const TransitionStyle& style, const QString& mergeKey = {});
    // Wirksame Übergänge einer Spur (für Anzeige)
    QVector<TimelineOps::TransitionSpan> transitions(TrackRef ref) const;

    // Keyframes (Inspector-Rauten): an Timeline-Frame `frame` für die Parameter setzen (on) bzw. entfernen,
    // jeweils im Clip-Frame unter dem Playhead (auf den Clip begrenzt)
    void setKeyframes(const QVector<int>& ids, const QVector<AnimParam>& params, int frame, bool on);
    // Verlauf (Linear/Ease) an den Keyframes; params leer = alle Parameter (Keyframe-Spur der Timeline)
    void setKeyframeEase(const QVector<int>& ids, const QVector<AnimParam>& params, int frame, KeyEase ease);
    // Keyframe-Spur der Timeline: Rauten an Clip-Frames `times` (alle Parameter) verschieben/löschen
    void moveKeyframes(int clipId, const QVector<int>& times, int delta);
    // params leer = alle Parameter (Keyframe-Spur), sonst nur diese (Kurven-Editor)
    void removeKeyframes(int clipId, const QVector<int>& times, const QVector<AnimParam>& params = {});

    // Geschwindigkeit wie DaVinci „Change Clip Speed“ (Strg+R) für die Clips (mit Partnern). Titel/Standbilder bleiben.
    // ripple = Cliplänge folgt der Geschwindigkeit, spätere Clips derselben Spuren rücken nach; sonst bleibt die Länge
    // (begrenzt aufs vorhandene Material). Rückwärts zeigt denselben Ausschnitt umgekehrt; Keyframes wandern mit.
    struct Retime {
        double speed = 1.0;
        bool reverse = false, freeze = false, keepPitch = true;
    };
    void setClipSpeed(const QVector<int>& ids, const Retime& r, bool ripple);

    // Speed Ramp wie DaVinci „Retime Controls“ am Clip (mit verknüpften Partnern derselben Datei), je ein Undo-Schritt.
    // Anfang/Ende und Keyframes bleiben an ihrer Quellstelle, die Cliplänge folgt dem Tempo (spätere Clips derselben
    // Spuren rücken nach). frame = Timeline-Frame im Clip; Abschnitt 0 liegt vor dem ersten Speed-Punkt.
    bool canRetime(int clipId) const;
    void addSpeedPoint(int clipId, int frame);
    void removeSpeedPoint(int clipId, int index);
    void moveSpeedPoint(int clipId, int index, int delta); // Punkt um delta Timeline-Frames, Tempo davor bleibt
    void setSegmentSpeed(int clipId, int segment, double speed);
    void setSpeedPointSmooth(int clipId, int index, int frames);

    // Quell-In/Out (I/O im Quell-Viewer) eines Media-Pool-Clips, -1 = entfernen; Regeln wie setMarkIn/setMarkOut
    void setSourceMarkIn(const QString& path, int frame);
    void setSourceMarkOut(const QString& path, int frame);
    void clearSourceMarks(const QString& path);

    // Bearbeitungen aus dem Quell-Viewer wie DaVinci (Edit-Menü, F9–F12)
    enum class SourceEditMode {
        Insert,          // F9: am Timeline-In bzw. Playhead einfügen, spätere Clips der Zielspuren rücken nach
        Overwrite,       // F10: überschreiben
        Replace,         // F11: Clip unter dem Playhead ersetzen (Länge bleibt, Quell-Playhead/-In deckt sich mit dem Playhead)
        PlaceOnTop,      // F12: auf die erste freie Spur über allen Clips im Bereich
        RippleOverwrite, // Shift+F10: Clip unter dem Playhead ersetzen, Rest der Spur rückt um den Längenunterschied
        AppendAtEnd,     // Shift+F12: ans Ende der Timeline
        FitToFill,       // Shift+F11: Quell-In/Out per Geschwindigkeit genau zwischen Timeline-In und -Out (überschreibt)
    };
    // 3-Punkt-Schnitt: Quellbereich = Quell-In/Out (fehlt einer: Clipanfang/-ende), Ziel = Timeline-In, sonst Playhead;
    // Timeline-In+Out begrenzt die Länge, nur Timeline-Out = rückwärts ab Out. Benutzte Timeline-In/Out werden
    // danach entfernt (wie DaVinci). srcPos = Quell-Playhead (für Replace ohne Quell-In).
    // Video/Audio verknüpft auf die Zielspuren, ein Undo-Schritt. Rückgabe: Ende des neuen Clips (neuer Playhead),
    // -1 = nichts passiert (kein Clip unter dem Playhead, zu wenig Material).
    int sourceEdit(SourceEditMode mode, const QString& path, int srcPos, int playhead);
    // Quellbereich [in, out] an Frame `frame` auf Spur-Index `track` (V[n]/A[n]) überschreiben (Drag aus dem Viewer)
    void placeSourceRange(const QString& path, int in, int out, int frame, int track);
    // Zielspuren für F9/F10 … (wie DaVinci Destination Controls), Index 0 = V1/A1
    int targetVideoTrack() const { return m_targetVideo; }
    int targetAudioTrack() const { return m_targetAudio; }
    void setTargetTracks(int video, int audio);
    // Effekte (Open FX aus der Effects Library): an Videoclips anhängen (Clips, die ihn schon haben, bleiben),
    // bzw. entfernen; je ein Undo-Schritt
    void addEffect(const QVector<int>& ids, const QString& effectId); // auch "lut:<Pfad>" (EffectFolders::LutPrefix)
    void removeEffect(const QVector<int>& ids, const QString& effectId);
    // Ziel für Doppelklick in der Effects Library: ausgewählte Videoclips, sonst der oberste Videoclip am Playhead
    QVector<int> effectTargets(int frame) const;

    // Color-Seite (Effekt "grade", Ziel wie effectTargets): Werte am Timeline-Frame `frame` setzen; legt die Korrektur
    // bei Bedarf an, animierte Werte bekommen dort einen Keyframe (wie im Inspector). Gleicher mergeKey = ein Undo-Schritt.
    void setGradeValues(const QVector<int>& ids, const QVector<QPair<AnimParam, double>>& values, int frame,
                        const QString& text, const QString& mergeKey = {});
    // Zurücksetzen: params = diese Werte auf den Standard (am Frame), leer = ganze Korrektur samt LUT/Keyframes weg
    void resetGrade(const QVector<int>& ids, const QVector<AnimParam>& params, int frame, const QString& text);
    void setGradeLut(const QVector<int>& ids, const QString& path); // leer = LUT entfernen
    void setGradeEnabled(const QVector<int>& ids, bool on);
    // Keyframe für die ganze Korrektur (wie ein DaVinci-Node): alle Werte am Frame setzen bzw. entfernen
    void setGradeKeyframe(const QVector<int>& ids, int frame, bool on);

    // Zwischenablage (Strg+C/X/V): Einfügen am Playhead auf denselben Spuren, überschreibt
    void copySelection();
    void cutSelection();
    void paste(int frame);

    // ---- Untertitel (DaVinci Subtitle Tracks, core/Subtitles.h) ----
    // Untertitel-Einträge teilen sich die Auswahl mit den Clips (eigene ids aus demselben Zähler)
    bool isSubtitle(int id) const;
    QVector<int> clipIdsOf(const QSet<int>& ids) const; // nur Clips (ohne Untertitel)
    QVector<int> selectedSubtitles() const; // ausgewählte Einträge, nicht auf gesperrten Spuren
    bool isSubtitleTrackLocked(int index) const;
    // Neue Spur oben (ST1, ST2 …), Stil passend zum Projektformat; sichtbar, wenn keine andere sichtbar ist. Liefert den Index.
    int addSubtitleTrack();
    void removeSubtitleTrack(int index);
    // Sichtbar im Viewer/beim Einbrennen; wie DaVinci immer nur eine Spur zugleich (an = andere aus)
    void setSubtitleTrackEnabled(int index, bool on);
    void toggleSubtitleTrackLock(int index);
    void renameSubtitleTrack(int index, const QString& name);
    // Spurstil (Schrift, Farbe, Umrandung, Hintergrund, Position); gleicher mergeKey = ein Undo-Schritt
    void setSubtitleStyle(int index, const QString& text, const std::function<void(TitleStyle&)>& fn,
                          const QString& mergeKey = {});
    // Untertitel am Frame (Standard 3 s, vor dem nächsten Eintrag gekürzt) auf Spur `track`, < 0 = sichtbare Spur
    // (keine vorhanden: ST1 anlegen). Wird ausgewählt; liefert die id, 0 = dort liegt schon einer / Spur gesperrt.
    int addSubtitle(int frame, int track = -1, const QString& text = {});
    void setSubtitleText(int id, const QString& text, const QString& mergeKey = {});
    // Start/Ende (Inspector): begrenzt auf die Nachbarn, mindestens 1 Frame
    void setSubtitleTiming(int id, int start, int end);
    // Verschieben (auch auf andere Untertitelspuren); überschreibt, was dort liegt (wie Clips)
    void moveSubtitles(const QVector<int>& ids, int delta, int trackDelta);
    int clampSubtitleTrackDelta(const QVector<int>& ids, int trackDelta) const;
    // Kante ziehen: begrenzt auf die Nachbarn und mindestens 1 Frame
    int clampSubtitleTrim(int id, TimelineOps::Edge edge, int delta) const;
    void trimSubtitle(int id, TimelineOps::Edge edge, int delta);
    // Eingelesene Einträge (SRT) als neue Spur; name = Spurname (z. B. Dateiname). Liefert den Spur-Index, -1 = leer.
    int importSubtitles(const QVector<SubtitleCue>& cues, const QString& name);

    // ---- Compound Clips und verschachtelte Timelines (core/EditorCompound.cpp) ----
    // Ausgewählte Clips (mit Partnern) zu einem Compound Clip zusammenfassen (DaVinci „New Compound Clip“): neue
    // Sequenz mit diesen Clips (Spuren ab der untersten benutzten, Zeit ab dem frühesten Clip); in der Timeline
    // ersetzt sie ein Compound Clip über die ganze Spanne auf der untersten Video- bzw. Audiospur ab der Auswahl, auf
    // der die Spanne frei ist (Video + Audio verknüpft, überschreibt nie andere Clips). Ein Undo-Schritt; neuer
    // Clip wird ausgewählt. Liefert die Sequenz-id, 0 = nichts ausgewählt.
    int createCompoundClip(const QString& name = {});
    // Compound Clips (mit Partnern) wieder in ihre Clips zerlegen (DaVinci „Decompose in Place“): Inhalt im
    // benutzten Ausschnitt an dieselbe Stelle, Spuren ab der des Compound Clips. Die Sequenz bleibt im Media Pool.
    bool decomposeCompoundClips(const QVector<int>& ids);
    // Ausgewählter Compound Clip (für „In Timeline öffnen“), 0 = keiner
    int selectedCompoundSequence() const;
    // Timeline/Compound Clip aus dem Media Pool am Frame auf V[track]/A[track] legen (überschreibt, verknüpft).
    // false = leer oder Schleife (Sequenz enthält die geöffnete Timeline).
    bool addSequenceAt(int sequenceId, int frame, int track = 0);

signals:
    void targetTracksChanged(); // Zielspuren (Spurkopf-Markierung neu zeichnen)
    void mixerOnlyEdit();       // nächste Änderung betrifft nur den Mixer (Vorschau live, ohne Neuaufbau)

private:
    TimelineOps::SourceLength sourceLength() const;
    // Kopie der Timeline mit Übergang am Schnitt (siehe addTransitionAt); nullopt = Clips nicht gefunden
    std::optional<Timeline> withTransitionAt(int leftId, int rightId, const TransitionStyle& style,
                                             TrackRef* where = nullptr) const;

    // Quellbereich ab Quell-Frame sIn (len Frames) an `start` auf V[vTrack]/A[aTrack] legen (überschreibt, verknüpft)
    // sIn/len in Frames des umgerechneten Materials, wenn speed != 1 (siehe Clip::speed)
    void placeSource(Timeline& tl, const MediaInfo& m, int sIn, int len, int start, int vTrack, int aTrack,
                     double speed = 1.0);

    // Gemeinsamer Ablauf der Speed-Ramp-Bearbeitungen (siehe addSpeedPoint); change bekommt den alten Verlauf
    // und die Dateilänge und liefert false, wenn sich nichts ändert
    void editRamp(const QString& text, int clipId, const std::function<bool(Clip&, const RetimeMap&, int)>& change);

    // Clips unter dem Playhead bzw. die Auswahl (mit Partnern), wie DaVinci bei Strg+B
    QVector<int> targetIds(int frame) const;

    struct ClipboardItem {
        Clip clip;
        TrackRef ref;
    };

    Project* m_project;
    Selection* m_selection;
    bool m_linkedSelection = true;
    bool m_splitOnSelectedTracks = true;
    int m_targetVideo = 0, m_targetAudio = 0;
    QVector<ClipboardItem> m_clipboard; // Starts relativ zum frühesten Clip
};
