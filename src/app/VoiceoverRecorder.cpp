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
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QGuiApplication>
#include <QTimer>

#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace {
constexpr char kRunningKey[] = "voiceover/running"; // the file of the take running now (QSettings)

Audio::Capture::Config inputConfig()
{
    Audio::Capture::Config config;
    config.rate = 48000;
    config.channels = 1;
    config.backend = Engine::audioDriver();
    config.input = Voiceover::Settings::load().input;
    config.buffer = Engine::audioBuffer();
    return config;
}
} // namespace

// An input opening on the input thread. Taken back by the GUI thread (inputOpened) or dropped (cancelled): what it
// opened after that is closed there at once
struct VoiceoverRecorder::Opening {
    std::mutex mutex;
    bool cancelled = false;
    std::unique_ptr<Audio::Capture> input;
    QString path, why;
};

// Opens and closes inputs one after the other, never on the GUI thread (a sound server takes 30-50 ms to open one,
// ma_device_uninit and the writer's end a few more). In order, so an input is closed before the next one opens (an
// ALSA hw device opens only once). Ending runs what is still queued.
class VoiceoverRecorder::InputThread {
public:
    InputThread() { m_thread = std::thread([this] { run(); }); }
    ~InputThread()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_quit = true;
        }
        m_wake.notify_all();
        m_thread.join();
    }
    void post(std::function<void()> job)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_jobs.push_back(std::move(job));
        }
        m_wake.notify_all();
    }

private:
    void run()
    {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.wait(lock, [this] { return m_quit || !m_jobs.empty(); });
                if (m_jobs.empty()) return;
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
            }
            job();
            job = nullptr; // what it holds (an input to close) ends here, not on the GUI thread
        }
    }
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<std::function<void()>> m_jobs;
    bool m_quit = false;
    std::thread m_thread;
};

VoiceoverRecorder::VoiceoverRecorder(Engine* engine, Project* project, Editor* editor,
                                     std::function<QString()> projectPath, std::function<bool(const QString&)> import,
                                     QObject* parent)
    : QObject(parent), m_engine(engine), m_project(project), m_editor(editor), m_projectPath(std::move(projectPath)),
      m_import(std::move(import)), m_timer(new QTimer(this)), m_meterTimer(new QTimer(this)),
      m_inputs(std::make_unique<InputThread>())
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
        if (recording() && !m_stopping) stopLater();
    });
    // The level meter closes its input while the app is in the background (setMeterShown)
    if (auto* gui = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        m_appActive = gui->applicationState() == Qt::ApplicationActive || QGuiApplication::platformName() == "offscreen";
        connect(gui, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            m_appActive = state == Qt::ApplicationActive;
            updateMeter();
        });
    }
    // Tracks added or removed (an undo, an AI's edit): the armed track and the take's track follow their track
    connect(m_project, &Project::timelineChanged, this, &VoiceoverRecorder::followTracks);
}

VoiceoverRecorder::~VoiceoverRecorder()
{
    if (m_capture) { // closing the window mid-take: the file stays complete, no signals any more
        m_capture->stop();
        QSettings().remove(kRunningKey);
    }
    dropOpening(m_opening);
    dropOpening(m_meterOpening);
    if (m_meter) closeInput(std::move(m_meter));
    m_inputs.reset(); // closes what is still open, then ends
}

std::shared_ptr<VoiceoverRecorder::Opening> VoiceoverRecorder::openInput(const QString& path)
{
    auto opening = std::make_shared<Opening>();
    opening->path = path;
    m_inputs->post([this, opening, config = inputConfig()] {
        {
            std::lock_guard<std::mutex> lock(opening->mutex);
            if (opening->cancelled) return;
        }
        QString why;
        std::unique_ptr<Audio::Capture> input = Audio::Capture::start(config, opening->path, &why);
        {
            std::lock_guard<std::mutex> lock(opening->mutex);
            if (opening->cancelled) {
                if (input) {
                    input.reset();
                    if (!opening->path.isEmpty()) QFile::remove(opening->path);
                }
                return;
            }
            opening->input = std::move(input);
            opening->why = why;
        }
        // The recorder waits for this thread before it goes (its destructor): it is still there
        QMetaObject::invokeMethod(this, [this, opening] { inputOpened(opening); }, Qt::QueuedConnection);
    });
    return opening;
}

void VoiceoverRecorder::dropOpening(std::shared_ptr<Opening>& opening)
{
    if (!opening) return;
    std::unique_ptr<Audio::Capture> input;
    {
        std::lock_guard<std::mutex> lock(opening->mutex);
        opening->cancelled = true;
        input = std::move(opening->input);
    }
    if (input) closeInput(std::move(input), opening->path);
    opening.reset();
}

void VoiceoverRecorder::closeInput(std::unique_ptr<Audio::Capture> input, const QString& remove)
{
    std::shared_ptr<Audio::Capture> shared(std::move(input));
    m_inputs->post([shared, remove]() mutable {
        shared->stop();
        shared.reset();
        if (!remove.isEmpty()) QFile::remove(remove);
    });
}

void VoiceoverRecorder::inputOpened(const std::shared_ptr<Opening>& opening)
{
    std::unique_ptr<Audio::Capture> input;
    QString why;
    {
        std::lock_guard<std::mutex> lock(opening->mutex);
        if (opening->cancelled) return; // dropped meanwhile (dropOpening closed what it opened)
        input = std::move(opening->input);
        why = opening->why;
    }
    if (opening == m_meterOpening) {
        m_meterOpening.reset();
        m_meter = std::move(input); // no input: no meter, recording says why
        if (m_meter) m_meterTimer->start();
    } else if (opening == m_opening) {
        m_opening.reset();
        if (!input) {
            m_engine->setOpenEnd(false);
            emit recordingChanged(false);
            updateMeter();
            emit failed(T("Kein Eingang: %1").arg(why));
            return;
        }
        m_capture = std::move(input);
        begin();
    } else if (input) {
        closeInput(std::move(input), opening->path);
    }
}

void VoiceoverRecorder::setArmedTrack(int track)
{
    if (recording() || track == m_armed) return; // the track does not change in the middle of a take
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
    if (recording() && current == m_sequence) {
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
    if (!metering()) return;
    m_meterTimer->stop();
    dropOpening(m_meterOpening);
    if (m_meter) closeInput(std::move(m_meter));
    updateMeter();
}

QString VoiceoverRecorder::runningTake()
{
    QSettings settings;
    const QString path = settings.value(kRunningKey).toString();
    settings.remove(kRunningKey); // offered once
    return !path.isEmpty() && QFileInfo(path).size() > 44 ? path : QString(); // more than a header: sound in it
}

void VoiceoverRecorder::setMeterShown(bool shown)
{
    m_meterShown = shown;
    updateMeter();
}

void VoiceoverRecorder::updateMeter()
{
    const bool want = m_armed >= 0 && !recording() && m_meterShown && m_appActive;
    if (want == metering()) return;
    if (!want) {
        m_meterTimer->stop();
        dropOpening(m_meterOpening);
        if (m_meter) closeInput(std::move(m_meter));
        emit inputLevel(-100.f);
        return;
    }
    m_meterOpening = openInput(QString()); // the level only, no file
}

bool VoiceoverRecorder::start(QString* error)
{
    if (recording()) return true;
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
    // Pausing in begin() emits speedChanged(0): with m_playing still true from the take before, that ended this
    // take at once (every second take recorded nothing and left the red sign on)
    m_playing = false;
    m_meterTimer->stop();
    dropOpening(m_meterOpening);
    if (m_meter) closeInput(std::move(m_meter)); // one input at a time: closed before the take's opens
    m_sequence = m_project->currentSequence();
    m_trackKey = Voiceover::trackKey(m_project->timeline(), m_track);
    m_liveTrack = m_track;
    m_live = false;
    ++m_takeNumber;
    m_stopping = false;
    // The timeline plays along also behind its last clip and on an empty one (open end: black picture, silence, the
    // playhead and the take run on) - recording does not stop at the end
    m_engine->setOpenEnd(true);
    // The input opens on its thread (30-50 ms, more when the sound server wakes up): the take begins once it runs
    // (begin), "Gleich …" meanwhile. In this order as before: the input first, then the timeline plays.
    m_opening = openInput(Voiceover::uniquePath(dir, QDateTime::currentDateTime()));
    emit recordingChanged(true); // "Gleich …": the take shows once the timeline is heard (tick)
    return true;
}

void VoiceoverRecorder::begin()
{
    qInfo("Voiceover: %s -> %s", qPrintable(m_capture->name()), qPrintable(m_capture->path()));
    QSettings().setValue(kRunningKey, m_capture->path()); // a crash before the end: offered next time (runningTake)
    m_alignment = {};
    m_starting = true;
    m_engine->pause();
    // Right after the take before: start at its end (the playhead stops a frame or two off it)
    const int tolerance = std::max(3, int(std::lround(m_project->frameRate() / 10)));
    m_from = Voiceover::buttStart(m_project->timeline(), m_track, m_engine->position(), tolerance);
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
    if (!m_playing) goLive();
}

void VoiceoverRecorder::abortStart()
{
    dropOpening(m_opening); // its file goes too: nothing was recorded
    m_engine->setOpenEnd(false);
    emit recordingChanged(false);
    updateMeter();
}

void VoiceoverRecorder::stopLater()
{
    QMetaObject::invokeMethod(this, [this, take = m_takeNumber] {
        if (recording() && !m_stopping && take == m_takeNumber) stop();
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
    if (m_opening && !m_capture) { // before the input ran
        abortStart();
        const QString problem = T("Nichts aufgenommen.");
        if (error) *error = problem;
        emit finished(QString(), false, problem);
        return false;
    }
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
    QSettings().remove(kRunningKey);
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
    if (m_opening && !m_capture) {
        abortStart();
        return;
    }
    if (!m_capture) return;
    m_stopping = true;
    m_timer->stop();
    m_capture->stop();
    m_capture.reset();
    QSettings().remove(kRunningKey);
    if (m_playing) m_engine->pause();
    m_engine->setOpenEnd(false);
    m_stopping = false;
    if (std::exchange(m_live, false)) emit liveChanged(false);
    emit recordingChanged(false);
    updateMeter();
}
