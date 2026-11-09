// SPDX-License-Identifier: AGPL-3.0-only
#include "app/VoiceoverRecorder.h"

#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "engine/audio/Capture.h"

#include <QDateTime>
#include <QDir>
#include <QTimer>

#include <cmath>
#include <utility>

VoiceoverRecorder::VoiceoverRecorder(Engine* engine, Project* project, Editor* editor,
                                     std::function<QString()> projectPath, std::function<bool(const QString&)> import,
                                     QObject* parent)
    : QObject(parent), m_engine(engine), m_project(project), m_editor(editor), m_projectPath(std::move(projectPath)),
      m_import(std::move(import)), m_timer(new QTimer(this)), m_meterTimer(new QTimer(this))
{
    m_meterTimer->setInterval(33);
    connect(m_meterTimer, &QTimer::timeout, this, [this] {
        if (!m_meter) return;
        const float peak = m_meter->takePeak();
        emit inputLevel(peak > 0 ? 20.f * std::log10(peak) : -100.f);
    });
    m_timer->setInterval(10); // pairs for the lining up: the finer, the closer the best one (Voiceover::Alignment)
    connect(m_timer, &QTimer::timeout, this, &VoiceoverRecorder::tick);
    // Playback stopped (Space, Stop; the end of the timeline does not: open end) or turned (J, faster) ends the
    // recording; so does a seek (a click in the ruler, Home, arrow keys): a take only runs forward from its start
    // These end it from the event loop: a pause or seek may come from inside an undo step (one in another timeline
    // opens that one), and placing the take pushes a step of its own
    connect(m_engine, &Engine::speedChanged, this, [this](double speed) {
        if (speed != 1.0 && m_capture && m_playing && !m_stopping && !m_starting) stopLater();
    });
    connect(m_engine, &Engine::seeked, this, [this] {
        if (m_capture && !m_stopping && !m_starting) stopLater();
    });
    // Another timeline opened (a tab, an undo there, an AI): the take ends and goes to the timeline it began in
    connect(m_project, &Project::currentSequenceChanged, this, [this] {
        if (m_capture && !m_stopping) stopLater();
    });
    // Tracks added or removed (an undo, an AI's edit): the armed track and the take's track follow their track
    connect(m_project, &Project::timelineChanged, this, &VoiceoverRecorder::followTracks);
}

VoiceoverRecorder::~VoiceoverRecorder()
{
    if (m_capture) m_capture->stop(); // closing the window mid-take: the file stays complete, no signals any more
    m_meter.reset();
}

void VoiceoverRecorder::setArmedTrack(int track)
{
    if (m_capture || track == m_armed) return; // the track does not change in the middle of a take
    m_armed = track;
    m_armedKey = Voiceover::trackKey(m_project->timeline(), track);
    m_armedSequence = m_project->currentSequence();
    updateMeter();
    emit armedChanged(track);
}

void VoiceoverRecorder::followTracks()
{
    const Timeline& tl = m_project->timeline();
    const int current = m_project->currentSequence();
    if (m_capture && current == m_sequence) {
        // The take's track: the same one, else (not sure which it is now) a new one - a take never lands on a track
        // that is not the one it showed on
        int track = Voiceover::findTrack(tl, m_trackKey);
        if (track < 0) track = int(tl.audio.size());
        m_trackKey = Voiceover::trackKey(tl, track);
        if (track != m_track) {
            m_track = m_liveTrack = track;
            if (m_live) emit takeChanged();
        }
    }
    if (m_armed < 0) return;
    // The mic belongs to a track of one timeline: another one opened, a track gone or not to be told apart: disarmed
    int armed = current == m_armedSequence ? Voiceover::findTrack(tl, m_armedKey) : -1;
    // The track below the last one armed (the Mixer offers it: the take makes it): armed while it is still the next
    if (m_armedKey.index >= m_armedKey.clips.size()) armed = current == m_armedSequence && m_armed <= tl.audio.size() ? m_armed : -1;
    else if (armed >= tl.audio.size()) armed = -1;
    m_armedKey = Voiceover::trackKey(tl, armed);
    if (armed == m_armed) return;
    m_armed = armed;
    updateMeter();
    emit armedChanged(armed);
}

void VoiceoverRecorder::inputChanged()
{
    if (!m_meter) return;
    m_meterTimer->stop();
    m_meter.reset();
    updateMeter();
}

void VoiceoverRecorder::updateMeter()
{
    const bool want = m_armed >= 0 && !m_capture;
    if (want == (m_meter != nullptr)) return;
    if (!want) {
        m_meterTimer->stop();
        m_meter.reset();
        emit inputLevel(-100.f);
        return;
    }
    Audio::Capture::Config config;
    config.rate = 48000;
    config.channels = 1;
    config.backend = Engine::audioDriver();
    config.input = Voiceover::Settings::load().input;
    config.buffer = Engine::audioBuffer();
    m_meter = Audio::Capture::start(config, QString()); // no input: no meter, recording says why
    if (m_meter) m_meterTimer->start();
}

bool VoiceoverRecorder::start(QString* error)
{
    if (m_capture) return true;
    if (m_engine->mode() != Engine::Mode::Timeline) {
        if (error) *error = T("Voiceover nimmt nur in der Timeline auf.");
        return false;
    }
    // The track, once: armed (locked: refused), else selected/Voiceover/free/one new (Voiceover::recordTrack)
    const QSet<int> selected = m_editor->selection()->ids();
    const Timeline& tl = m_project->timeline();
    m_track = Voiceover::recordTrack(tl, m_armed, Voiceover::selectedTrack(tl, QVector<int>(selected.begin(), selected.end())),
                                     m_engine->position(), INT_MAX, Voiceover::folder(m_projectPath()));
    if (m_track < 0) {
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
    // Pausing below emits speedChanged(0): with m_playing still true from the take before, that ended this take
    // at once (every second take recorded nothing and left the red sign on)
    m_playing = false;
    m_meterTimer->stop();
    m_meter.reset(); // one input at a time
    m_capture = Audio::Capture::start(config, Voiceover::uniquePath(dir, QDateTime::currentDateTime()), &why);
    if (!m_capture) {
        if (error) *error = T("Kein Eingang: %1").arg(why);
        updateMeter();
        return false;
    }
    qInfo("Voiceover: %s -> %s", qPrintable(m_capture->name()), qPrintable(m_capture->path()));
    m_alignment = {};
    m_sequence = m_project->currentSequence();
    m_trackKey = Voiceover::trackKey(m_project->timeline(), m_track);
    ++m_takeNumber;
    m_stopping = false;
    m_starting = true;
    m_engine->pause();
    // Right after the take before: start at its end (the playhead stops a frame or two off it)
    const int tolerance = std::max(3, int(std::lround(m_project->frameRate() / 10)));
    m_from = Voiceover::buttStart(m_project->timeline(), m_track, m_engine->position(), tolerance);
    // The timeline plays along also behind its last clip and on an empty one (open end: black picture, silence, the
    // playhead and the take run on) - recording does not stop at the end
    m_engine->setOpenEnd(true);
    const int preroll = int(std::lround(m_settings.prerollSeconds * m_project->frameRate()));
    m_playFrom = std::max(0, m_from - preroll);
    if (m_playFrom != m_engine->position()) m_engine->seek(int(m_playFrom));
    m_engine->play();
    m_playing = m_engine->speed() != 0.0;
    m_starting = false;
    // What the input recorded before the timeline was heard (the player waits for the first picture; words said
    // right at the key press) is kept: the take reaches back before the playhead, to the clip before it on the track.
    // With a pre-roll the take starts at the playhead (the pre-roll is for listening in).
    m_headFrom = preroll > 0 ? m_from : Voiceover::headLimit(m_project->timeline(), m_track, m_from);
    m_take = Voiceover::LiveTake(m_playFrom, m_headFrom, m_project->frameRate(), m_capture->rate());
    m_liveTrack = m_track;
    m_live = false;
    m_sinceStart.start();
    m_timer->start();
    emit recordingChanged(true); // "Gleich …": the take shows once the timeline is heard (tick)
    if (!m_playing) goLive();
    return true;
}

void VoiceoverRecorder::stopLater()
{
    QMetaObject::invokeMethod(this, [this, take = m_takeNumber] {
        if (m_capture && !m_stopping && take == m_takeNumber) stop();
    }, Qt::QueuedConnection);
}

void VoiceoverRecorder::tick()
{
    if (!m_capture) return;
    const double heard = m_engine->soundFrame();
    if (m_playing && std::isfinite(heard))
        m_alignment.add(m_capture->frames(), heard / m_project->frameRate(), m_capture->rate());
    const float peak = m_capture->takePeak();
    const long long before = m_take.samples();
    m_take.add(m_capture->frames(), peak);
    // The player starts a moment after the input (it waits for the picture): placed by the pairs, the growing clip
    // ends at the playhead instead of running ahead of it
    if (const auto s = m_alignment.fileStartSeconds(0, m_capture->rate(), m_settings.offsetMs))
        m_take.setFileStart(*s * m_project->frameRate());
    // Live once the timeline sound runs (the first pair counted) - or after a moment without sound output
    if (!m_live && (!m_alignment.empty() || m_sinceStart.elapsed() > 1500)) goLive();
    if (m_live && m_take.samples() != before) emit takeChanged();
    emit inputLevel(peak > 0 ? 20.f * std::log10(peak) : -100.f);
}

void VoiceoverRecorder::goLive()
{
    m_live = true;
    emit liveChanged(true);
    emit takeChanged();
}

bool VoiceoverRecorder::stop(QString* error)
{
    if (!m_capture || m_stopping) return false;
    m_stopping = true;
    tick(); // one more pair
    m_timer->stop();
    if (m_playing) m_engine->pause();
    m_engine->setOpenEnd(false);
    QString why;
    const bool written = m_capture->stop(&why);
    const QString path = m_capture->path();
    const int inputLatency = m_capture->latency();
    const int rate = m_capture->rate();
    const long long frames = m_capture->frames(), dropped = m_capture->dropped();
    m_capture.reset();
    if (std::exchange(m_live, false)) emit liveChanged(false);
    emit recordingChanged(false);
    updateMeter();
    if (dropped > 0) qWarning("Voiceover: %lld von %lld Samples verloren", dropped, frames);

    const double fps = m_project->frameRate();
    // Where file sample 0 lies in the timeline: from the pairs taken while playing; none (stopped at once, no
    // sound output): where playback started
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
    m_placedTrack = -1;
    m_replaced = 0;
    if (!written) problem = T("Aufnahme nicht gespeichert: %1").arg(why);
    else if (frames <= 0) problem = T("Nichts aufgenommen.");
    else if (!m_import(path)) problem = T("Aufnahme nicht lesbar: %1").arg(QDir::toNativeSeparators(path));
    else {
        // Still the track it began on? (An edit in the timeline it went on meanwhile is followed by followTracks;
        // one while another timeline was open is checked here.) Not sure: a new track rather than someone else's.
        if (const Sequence* seq = m_project->sequence(m_sequence)) {
            const Timeline& tl = seq->id == m_project->currentSequence() ? m_project->timeline() : seq->timeline;
            m_track = Voiceover::findTrack(tl, m_trackKey);
            if (m_track < 0) m_track = int(tl.audio.size());
        }
        placed = m_editor->placeRecording(path, m_track, fileStart, m_headFrom, &m_placedTrack, m_sequence,
                                          &m_replaced) != 0;
    }
    if (written && frames > 0 && !placed && problem.isEmpty()) problem = T("Aufnahme liegt nur im Medienpool.");
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
    m_engine->setOpenEnd(false);
    m_stopping = false;
    if (std::exchange(m_live, false)) emit liveChanged(false);
    emit recordingChanged(false);
    updateMeter();
}
