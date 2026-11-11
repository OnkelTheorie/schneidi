// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Voice-over recording in the app (core/Voiceover.h, engine/audio/Capture.h): record while the timeline plays from
// the playhead (after an optional pre-roll, also behind its end) onto the track Voiceover::recordTrack picks once at
// the start: the armed one (mic in its header; punch-in), else the selected clips' free track, else a Voiceover track,
// else a free unnamed one, else one new track. A take right after the one before butts against it. While it runs the
// take grows in the timeline (take(), liveTrack()). Stopping (the record key, Esc, Space) - or anything else moving
// the playhead (a seek, J, a speed change) - puts it into the Media Pool and onto that track, as one undo step.
// An armed track's input level is measured before recording too (a capture without a file) - only while the app is
// active and the timeline shown.
// Inputs open and close on a thread of their own, one after the other (a sound server takes 30-50 ms to open one,
// the app stood still that long when a take began, was armed or ended): start() returns at once, the take begins
// - the timeline plays - once the input runs, exactly as before, so the take lines up the same way.

#include "core/Voiceover.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

class Editor;
class Engine;
class Project;
class QTimer;
namespace Audio { class Capture; }

class VoiceoverRecorder : public QObject {
    Q_OBJECT
public:
    // projectPath: the saved project file ("" = never saved); import: brings a file into the Media Pool, true = it is
    // there now
    VoiceoverRecorder(Engine* engine, Project* project, Editor* editor, std::function<QString()> projectPath,
                      std::function<bool(const QString&)> import, QObject* parent = nullptr);
    ~VoiceoverRecorder() override;

    // Track that records (audio track index), -1 = none armed
    int armedTrack() const { return m_armed; }
    void setArmedTrack(int track);
    // Recording or about to (the input opens)
    bool recording() const { return m_capture != nullptr || m_opening != nullptr; }
    // The take really runs: the timeline is heard (until then the UI says "Gleich …"; what is said meanwhile is kept)
    bool live() const { return m_live; }
    // Starts recording (track: see above); false = it cannot (*error says why). The input opens meanwhile: none
    // found ends the take again with failed().
    bool start(QString* error = nullptr);
    // While recording: the growing take and the audio track it lands on now (>= the track count = a new one below)
    const Voiceover::LiveTake& take() const { return m_take; }
    int liveTrack() const { return m_liveTrack; }
    // The timeline the take runs in / the last take went to
    int sequence() const { return m_sequence; }
    // The track the last take was placed on (-1 = none)
    int placedTrack() const { return m_placedTrack; }
    // How many clips the last take cut into or replaced (an armed track: punch-in; earlier takes)
    int replaced() const { return m_replaced; }
    // Ends it and places the take; false = nothing placed (*error says why, empty = nothing recorded)
    bool stop(QString* error = nullptr);
    void cancel(); // ends it without placing anything (the file stays)
    // A take that was running when the app ended without stopping it (a crash, a power cut): its file, once ("" =
    // none). The WAV header follows while recording (Audio::Capture), so the file reads to about where it ended.
    static QString runningTake();
    void inputChanged(); // the input setting changed: the armed track's meter listens to the new one
    // The armed track's meter may listen (the timeline is shown). It never does while the app is in the background:
    // an open input keeps a Bluetooth headset in its call profile (poor sound for everything else)
    void setMeterShown(bool shown);
    bool metering() const { return m_meter != nullptr || m_meterOpening != nullptr; }

signals:
    void armedChanged(int track);
    void recordingChanged(bool on);
    void inputLevel(float db); // while armed or recording, ~30 times a second
    void takeChanged();        // the take grew (while live, with each block)
    void liveChanged(bool on); // the take really runs (the first heard frame) / ended
    void finished(const QString& path, bool placed, const QString& error);
    void failed(const QString& error); // the input did not open: no take

private:
    struct Opening; // an input opening on the input thread
    class InputThread;
    std::shared_ptr<Opening> openInput(const QString& path);
    void inputOpened(const std::shared_ptr<Opening>& opening);
    void dropOpening(std::shared_ptr<Opening>& opening); // no longer wanted: closed there (a take's file removed)
    // Closed on the input thread; remove: a file to delete after (a take that never began)
    void closeInput(std::unique_ptr<Audio::Capture> input, const QString& remove = QString());
    void begin();      // the input runs: the take begins (the timeline plays)
    void abortStart(); // stopped before the input ran: nothing recorded
    void tick();
    void stopLater(); // stop() from the event loop (once, for this take)
    void followTracks(); // the timeline changed: the armed track and the take's track follow theirs (Voiceover::findTrack)
    void updateMeter(); // the armed track's level meter on/off
    void goLive();
    Engine* m_engine;
    Project* m_project;
    Editor* m_editor;
    std::function<QString()> m_projectPath;
    std::function<bool(const QString&)> m_import;
    std::unique_ptr<Audio::Capture> m_capture;
    std::unique_ptr<Audio::Capture> m_meter; // armed, not recording: the input level only
    std::unique_ptr<InputThread> m_inputs;
    std::shared_ptr<Opening> m_opening;      // the take's input opening
    std::shared_ptr<Opening> m_meterOpening; // the meter's input opening
    QTimer* m_timer;
    QTimer* m_meterTimer;
    Voiceover::Alignment m_alignment;
    Voiceover::Settings m_settings;
    Voiceover::LiveTake m_take;
    int m_armed = -1;
    Voiceover::TrackKey m_armedKey; // which track m_armed is (tracks added/removed above it)
    int m_armedSequence = 0;        // in which timeline
    int m_headFrom = 0;      // the take may reach back to here (Voiceover::headLimit)
    bool m_live = false;
    QElapsedTimer m_sinceStart;
    int m_track = -1;       // the track the take goes to, fixed at the start (>= the track count: a new one)
    bool m_starting = false; // start() seeks and plays itself: that does not end the take
    int m_liveTrack = -1;
    int m_sequence = 0;     // the timeline the take began in: it goes there, also when another one is open by then
    int m_takeNumber = 0;
    Voiceover::TrackKey m_trackKey; // which track m_track is
    int m_placedTrack = -1;
    int m_replaced = 0;
    int m_from = 0;         // playhead when recording started (the clip starts here)
    double m_playFrom = 0;  // where playback started (pre-roll before m_from)
    bool m_playing = false; // the timeline plays along (open end: also behind its last clip)
    bool m_stopping = false;
    bool m_meterShown = true;
    bool m_appActive = true;
};
