// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Voice-over recording in the app (core/Voiceover.h, engine/audio/Capture.h): record on an armed audio track while
// the timeline plays from the playhead (after an optional pre-roll); stopping (button, Esc, Space, the end of the
// timeline) puts the take into the Media Pool and onto the track where the playhead stood, as one undo step.

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
    // Starts recording on the armed track (A1 when none is armed: it gets armed); false = no input (*error says why)
    bool start(QString* error = nullptr);
    // Ends it and places the take; false = nothing placed (*error says why, empty = nothing recorded)
    bool stop(QString* error = nullptr);
    void cancel(); // ends it without placing anything (the file stays)

signals:
    void armedChanged(int track);
    void recordingChanged(bool on);
    void inputLevel(float db); // while recording, ~30 times a second
    void finished(const QString& path, bool placed, const QString& error);

private:
    void tick();

    Engine* m_engine;
    Project* m_project;
    Editor* m_editor;
    std::function<QString()> m_projectPath;
    std::function<bool(const QString&)> m_import;
    std::unique_ptr<Audio::Capture> m_capture;
    QTimer* m_timer;
    Voiceover::Alignment m_alignment;
    Voiceover::Settings m_settings;
    int m_armed = -1;
    int m_from = 0;         // playhead when recording started (the clip starts here)
    double m_playFrom = 0;  // where playback started (pre-roll before m_from)
    bool m_playing = false; // the timeline plays along (not past its end)
    bool m_stopping = false;
};
