// SPDX-License-Identifier: AGPL-3.0-only
#include "app/VoiceoverRecorder.h"

#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "engine/audio/Capture.h"

#include <QDateTime>
#include <QDir>
#include <QTimer>

#include <cmath>

VoiceoverRecorder::VoiceoverRecorder(Engine* engine, Project* project, Editor* editor,
                                     std::function<QString()> projectPath, std::function<bool(const QString&)> import,
                                     QObject* parent)
    : QObject(parent), m_engine(engine), m_project(project), m_editor(editor), m_projectPath(std::move(projectPath)),
      m_import(std::move(import)), m_timer(new QTimer(this))
{
    m_timer->setInterval(10); // pairs for the lining up: the finer, the closer the best one (Voiceover::Alignment)
    connect(m_timer, &QTimer::timeout, this, &VoiceoverRecorder::tick);
    // Playback stopped (Space, Stop, the end of the timeline) ends the recording
    connect(m_engine, &Engine::speedChanged, this, [this](double speed) {
        if (speed == 0.0 && m_capture && m_playing && !m_stopping) stop();
    });
    // A track that is gone cannot stay armed
    connect(m_project, &Project::timelineChanged, this, [this] {
        if (m_armed >= m_project->timeline().audio.size() && !m_capture) setArmedTrack(-1);
    });
}

VoiceoverRecorder::~VoiceoverRecorder()
{
    if (m_capture) m_capture->stop(); // closing the window mid-take: the file stays complete, no signals any more
}

void VoiceoverRecorder::setArmedTrack(int track)
{
    if (m_capture || track == m_armed) return; // the track does not change in the middle of a take
    m_armed = track;
    emit armedChanged(track);
}

bool VoiceoverRecorder::start(QString* error)
{
    if (m_capture) return true;
    if (m_engine->mode() != Engine::Mode::Timeline) {
        if (error) *error = T("Voiceover nimmt nur in der Timeline auf.");
        return false;
    }
    if (m_armed < 0) setArmedTrack(0);
    if (m_editor->isTrackLocked({TrackKind::Audio, m_armed})) {
        if (error) *error = T("Die Spur ist gesperrt.");
        return false;
    }
    m_settings = Voiceover::Settings::load();
    const QString dir = Voiceover::folder(m_projectPath());
    if (!QDir().mkpath(dir)) {
        if (error) *error = T("Ordner lässt sich nicht anlegen: %1").arg(QDir::toNativeSeparators(dir));
        return false;
    }
    Audio::Capture::Config config;
    config.rate = 48000;
    config.channels = 1;
    config.backend = Engine::audioDriver();
    config.input = m_settings.input;
    config.buffer = Engine::audioBuffer();
    QString why;
    m_capture = Audio::Capture::start(config, Voiceover::uniquePath(dir, QDateTime::currentDateTime()), &why);
    if (!m_capture) {
        if (error) *error = T("Kein Eingang: %1").arg(why);
        return false;
    }
    qInfo("Voiceover: %s -> %s", qPrintable(m_capture->name()), qPrintable(m_capture->path()));
    m_alignment = {};
    m_stopping = false;
    m_engine->pause();
    m_from = m_engine->position();
    // Past the end of the timeline nothing plays along (play would start from the front)
    m_playing = m_from < TimelineOps::endFrame(m_project->timeline());
    m_playFrom = m_from;
    if (m_playing) {
        const int preroll = int(std::lround(m_settings.prerollSeconds * m_project->frameRate()));
        m_playFrom = std::max(0, m_from - preroll);
        if (m_playFrom != m_from) m_engine->seek(int(m_playFrom));
        m_engine->play();
    }
    m_timer->start();
    emit recordingChanged(true);
    return true;
}

void VoiceoverRecorder::tick()
{
    if (!m_capture) return;
    const double heard = m_engine->soundFrame();
    if (m_playing && std::isfinite(heard))
        m_alignment.add(m_capture->frames(), heard / m_project->frameRate(), m_capture->rate());
    const float peak = m_capture->takePeak();
    emit inputLevel(peak > 0 ? 20.f * std::log10(peak) : -100.f);
}

bool VoiceoverRecorder::stop(QString* error)
{
    if (!m_capture || m_stopping) return false;
    m_stopping = true;
    tick(); // one more pair
    m_timer->stop();
    if (m_playing) m_engine->pause();
    QString why;
    const bool written = m_capture->stop(&why);
    const QString path = m_capture->path();
    const int inputLatency = m_capture->latency();
    const int rate = m_capture->rate();
    const long long frames = m_capture->frames(), dropped = m_capture->dropped();
    m_capture.reset();
    emit recordingChanged(false);
    if (dropped > 0) qWarning("Voiceover: %lld von %lld Samples verloren", dropped, frames);

    const double fps = m_project->frameRate();
    // Where file sample 0 lies in the timeline: from the pairs taken while playing; nothing played along (past the
    // end, stopped at once): where playback started
    // The input's buffer size is not its delay: a block reaches the callback as soon as it is full, its newest sample
    // only a moment old. Taking the buffer off put the take 2048 samples (43 ms) early (measured: timeline played into
    // a PipeWire sink, its monitor recorded, take cross-correlated with the timeline sound: +45 ms with, +2.4 to +4 ms
    // without). What the hardware adds before the system sees the sound (converter, USB) is not reported by
    // miniaudio: the user's offset (Settings::offsetMs) takes that.
    constexpr int inputDelay = 0;
    double fileStart = m_playFrom;
    if (const auto s = m_alignment.fileStartSeconds(inputDelay, rate, m_settings.offsetMs)) fileStart = *s * fps;
    else fileStart -= m_settings.offsetMs / 1000.0 * fps;

    qInfo("Voiceover: %lld Samples, Datei beginnt bei Frame %.3f (Eingang %d Samples, Versatz %d ms, Start %d)",
          frames, fileStart, inputLatency, m_settings.offsetMs, m_from);

    QString problem;
    bool placed = false;
    if (!written) problem = T("Aufnahme nicht gespeichert: %1").arg(why);
    else if (frames <= 0) problem = T("Nichts aufgenommen.");
    else if (!m_import(path)) problem = T("Aufnahme nicht lesbar: %1").arg(QDir::toNativeSeparators(path));
    else placed = m_editor->placeRecording(path, m_armed, fileStart, m_from) != 0;
    if (written && frames > 0 && !placed && problem.isEmpty()) problem = T("Aufnahme liegt nur im Media Pool.");
    if (error) *error = problem;
    emit finished(path, placed, problem);
    m_stopping = false;
    return placed;
}

void VoiceoverRecorder::cancel()
{
    if (!m_capture) return;
    m_stopping = true;
    m_timer->stop();
    m_capture->stop();
    m_capture.reset();
    if (m_playing) m_engine->pause();
    m_stopping = false;
    emit recordingChanged(false);
}
