#pragma once
#include "core/TimelineOps.h"
#include "core/Types.h"

#include <QObject>
#include <algorithm>
#include <optional>

class Project;
class Selection;

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

    // Clip + verknüpfte Partner (falls "Linked Selection" an ist)
    QVector<int> withLinked(const QVector<int>& ids) const;

    // Legt Media ab: Video auf V[track], Audio auf A[track] (wie DaVinci, V2 <-> A2).
    // Fehlende Spuren werden angelegt. Mehrere Dateien landen hintereinander.
    void addMediaAt(const QStringList& paths, int frame, int track = 0);
    // Titel (5 s, wie DaVinci "Text") am Frame auf die unterste Videospur, die dort über allen Clips frei ist
    // (fehlende Spur wird angelegt); track >= 0 = feste Spur (Drag aus dem Media Pool). Neuer Clip wird ausgewählt.
    void addTitle(int frame, int track = -1);
    void toggleTrackMute(TrackRef ref);
    void toggleTrackHidden(TrackRef ref);
    void moveClips(const QVector<int>& ids, int deltaFrames, TrackKind kind, int trackDelta);
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
    // Auswahl um `frames` verschieben (Nudge, wie , und . in DaVinci)
    void nudgeSelection(int frames);
    // Anfang/Ende bis zum Playhead trimmen (Shift+[ / Shift+]); Auswahl oder Clips unter dem Playhead
    void trimToPlayhead(TimelineOps::Edge edge, int frame);
    void toggleSelectionEnabled();
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
    void removeKeyframes(int clipId, const QVector<int>& times);

    // Geschwindigkeit wie DaVinci „Change Clip Speed“ (Strg+R) für die Clips (mit Partnern). Titel/Standbilder bleiben.
    // ripple = Cliplänge folgt der Geschwindigkeit, spätere Clips derselben Spuren rücken nach; sonst bleibt die Länge
    // (begrenzt aufs vorhandene Material). Rückwärts zeigt denselben Ausschnitt umgekehrt; Keyframes wandern mit.
    struct Retime {
        double speed = 1.0;
        bool reverse = false, freeze = false, keepPitch = true;
    };
    void setClipSpeed(const QVector<int>& ids, const Retime& r, bool ripple);

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
    void setTargetTracks(int video, int audio) { m_targetVideo = std::max(0, video); m_targetAudio = std::max(0, audio); }

    // Zwischenablage (Strg+C/X/V): Einfügen am Playhead auf denselben Spuren, überschreibt
    void copySelection();
    void cutSelection();
    void paste(int frame);

private:
    TimelineOps::SourceLength sourceLength() const;
    // Kopie der Timeline mit Übergang am Schnitt (siehe addTransitionAt); nullopt = Clips nicht gefunden
    std::optional<Timeline> withTransitionAt(int leftId, int rightId, const TransitionStyle& style,
                                             TrackRef* where = nullptr) const;

    // Quellbereich ab Quell-Frame sIn (len Frames) an `start` auf V[vTrack]/A[aTrack] legen (überschreibt, verknüpft)
    void placeSource(Timeline& tl, const MediaInfo& m, int sIn, int len, int start, int vTrack, int aTrack);

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
