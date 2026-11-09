// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Voice-over recording in the app (core/Voiceover.h, engine/audio/Capture.h): record while the timeline plays from
// the playhead (after an optional pre-roll, also behind its end) onto the track Voiceover::recordTrack picks once at
// the start: the armed one (mic in its header; punch-in), else the selected clips' free track, else a Voiceover track,
// else a free unnamed one, else one new track. A take right after the one before butts against it. While it runs the
// take grows in the timeline (take(), liveTrack()). Stopping (the record key, Esc, Space) - or anything else moving
// the playhead (a seek, J, a speed change) - puts it into the Media Pool and onto that track, as one undo step.
// An armed track's input level is measured before recording too (a capture without a file).

#include "core/Voiceover.h"

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
    bool recording() const { return m_capture != nullptr; }
    // Starts recording (track: see above); false = no input (*error says why)
    bool start(QString* error = nullptr);
    // While recording: the growing take and the audio track it lands on now (>= the track count = a new one below)
    const Voiceover::LiveTake& take() const { return m_take; }
    int liveTrack() const { return m_liveTrack; }
    // The track the last take was placed on (-1 = none)
    int placedTrack() const { return m_placedTrack; }
    // Ends it and places the take; false = nothing placed (*error says why, empty = nothing recorded)
    bool stop(QString* error = nullptr);
    void cancel(); // ends it without placing anything (the file stays)
    void inputChanged(); // the input setting changed: the armed track's meter listens to the new one

signals:
    void armedChanged(int track);
    void recordingChanged(bool on);
    void inputLevel(float db); // while armed or recording, ~30 times a second
    void takeChanged();        // the take grew (while recording, with each block)
    void finished(const QString& path, bool placed, const QString& error);

private:
    void tick();
    void updateMeter(); // the armed track's level meter on/off
    Engine* m_engine;
    Project* m_project;
    Editor* m_editor;
    std::function<QString()> m_projectPath;
    std::function<bool(const QString&)> m_import;
    std::unique_ptr<Audio::Capture> m_capture;
    std::unique_ptr<Audio::Capture> m_meter; // armed, not recording: the input level only
    QTimer* m_timer;
    QTimer* m_meterTimer;
    Voiceover::Alignment m_alignment;
    Voiceover::Settings m_settings;
    Voiceover::LiveTake m_take;
    int m_armed = -1;
    int m_track = -1;       // the track the take goes to, fixed at the start (>= the track count: a new one)
    bool m_starting = false; // start() seeks and plays itself: that does not end the take
    int m_liveTrack = -1;
    int m_placedTrack = -1;
    int m_from = 0;         // playhead when recording started (the clip starts here)
    double m_playFrom = 0;  // where playback started (pre-roll before m_from)
    bool m_playing = false; // the timeline plays along (open end: also behind its last clip)
    bool m_stopping = false;
};
