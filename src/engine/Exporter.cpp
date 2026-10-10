#include "engine/Exporter.h"

#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "core/Subtitles.h"
#include "core/TimelineOps.h"
#include "engine/Bundle.h"
#include "engine/HardwareEncoders.h"
#include "engine/Profiles.h"
#include "engine/Stabilizer.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <atomic>
#include <cmath>

namespace {
std::atomic<int> g_running{0}; // laufende Exporte (m_timer läuft genau dann)

// Sichtbare Untertitelspur (sonst die erste mit Einträgen) im Bereich [from, to) als SRT, Zeiten ab Bereichsanfang
bool writeSubtitleFile(const Timeline& tl, double fps, int from, int to, const QString& path, QString* error)
{
    const SubtitleTrack* track = nullptr;
    for (const SubtitleTrack& t : tl.subtitles)
        if (t.enabled && !t.cues.isEmpty()) track = &t;
    for (const SubtitleTrack& t : tl.subtitles)
        if (!track && !t.cues.isEmpty()) track = &t;
    if (!track) {
        if (error) *error = T("Die Timeline enthält keine Untertitel.");
        return false;
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(Subtitles::toSrt(track->cues, fps, from, to)) < 0 || !f.commit()) {
        if (error) *error = T("Untertiteldatei kann nicht geschrieben werden: %1").arg(path);
        return false;
    }
    return true;
}

bool usesFrei0r(const Timeline& tl)
{
    for (const Track& t : tl.video)
        for (const Clip& c : t.clips)
            for (const EffectInstance& e : c.effects) {
                const EffectDescriptor* d = EffectRegistry::find(e.effectId);
                if (e.enabled && d && d->mltService.startsWith("frei0r.")) return true;
            }
    return false;
}

// Läuft im Encoder-Thread von avformat, direkt bevor er sich beendet
void onConsumerStopped(mlt_properties, void* done, mlt_event_data)
{
    static_cast<std::atomic<bool>*>(done)->store(true);
}

} // namespace

bool Exporter::sameFile(const QString& a, const QString& b)
{
    const QString ca = QFileInfo(a).canonicalFilePath(), cb = QFileInfo(b).canonicalFilePath();
    if (ca.isEmpty() || cb.isEmpty()) return false; // eine fehlt -> kann nicht dieselbe sein
#ifdef Q_OS_WIN
    return ca.compare(cb, Qt::CaseInsensitive) == 0;
#else
    return ca == cb;
#endif
}

namespace {

bool timelineReads(const Timeline& tl, const QString& path)
{
    for (const auto* tracks : {&tl.video, &tl.audio})
        for (const Track& t : *tracks)
            for (const Clip& c : t.clips)
                if (!c.mediaPath.isEmpty() && Exporter::sameFile(c.mediaPath, path)) return true;
    return false;
}

} // namespace

bool Exporter::readsFile(const Timeline& tl, const QString& path)
{
    if (timelineReads(tl, path)) return true;
    if (tl.nested)
        for (const Timeline& n : *tl.nested)
            if (timelineReads(n, path)) return true;
    return false;
}

bool Exporter::anyRunning()
{
    return g_running > 0;
}

int Exporter::savedThreads()
{
    return std::max(0, QSettings().value("render/cores", 0).toInt());
}

void Exporter::setSavedThreads(int threads)
{
    QSettings().setValue("render/cores", std::max(0, threads));
}

int Exporter::availableThreads()
{
    return std::max(1, QThread::idealThreadCount());
}

int Exporter::parallelFrames(const Timeline& tl, int threads)
{
    bool frei0r = usesFrei0r(tl);
    if (tl.nested)
        for (const Timeline& n : *tl.nested) frei0r = frei0r || usesFrei0r(n);
    return frei0r ? 1 : (threads > 0 ? threads : availableThreads());
}

Exporter::Exporter(QObject* parent) : QObject(parent)
{
    m_timer.setInterval(200);
    connect(&m_timer, &QTimer::timeout, this, &Exporter::poll);
}

Exporter::~Exporter()
{
    cancel();
}

bool Exporter::start(const Timeline& tl, const ExportSettings& s, QString* error)
{
    if (isRunning()) return false;
    const int end = TimelineOps::endFrame(tl);
    m_from = std::clamp(s.from, 0, std::max(0, end - 1));
    const int to = s.to < 0 ? end - 1 : std::min(s.to, end - 1);
    m_length = to - m_from + 1;
    if (end <= 0 || m_length <= 0) {
        if (error) *error = end <= 0 ? T("Die Timeline ist leer.") : T("Der In/Out-Bereich enthält nichts.");
        return false;
    }
    for (const QString& target : {s.path, s.subtitlePath})
        if (!target.isEmpty() && readsFile(tl, target)) {
            if (error) *error = T("%1 wird in der Timeline verwendet und kann nicht überschrieben werden.")
                                    .arg(QFileInfo(target).fileName());
            return false;
        }
    // Stabilization needs its analysis pass: run what is missing first, never export unstabilized silently
    if (!std::exchange(m_analysisDone, false) && runAnalyses(tl, s)) return true;

    // Andere Ausgabegröße als die Timeline: Pixelwerte (Position, Titel …) mitskalieren
    ProjectFormat out = s.format;
    Timeline scaled = tl;
    if (!s.size.isEmpty() && s.size != s.format.size()) {
        out.width = s.size.width() & ~1;
        out.height = s.size.height() & ~1;
        scaleTimeline(scaled, s.format.size(), out.size());
    }
    // Audio only: video tracks carry no sound (TimelineBuilder hides it), so skip decoding their images
    if (s.videoCodec.isEmpty()) scaled.video.clear();
    m_profile = makeProfile(out);
    m_builder = std::make_unique<TimelineBuilder>(*m_profile);
    const int threads = s.threads > 0 ? s.threads : availableThreads();
    m_builder->setDecoderThreads(threads);
    m_builder->setSubtitles(s.burnSubtitles && !s.videoCodec.isEmpty());
    m_tractor = m_builder->build(scaled);
    if (s.videoCodec.isEmpty()) { // keep the full length when the sound ends before the picture
        Mlt::Playlist blank(*m_profile);
        blank.blank(end - 1);
        blank.set("hide", 3);
        m_tractor->set_track(blank, m_tractor->count());
    }
    m_tractor->set_in_and_out(m_from, m_from + m_length - 1);

    QDir().mkpath(QFileInfo(s.path).absolutePath());
    if (!s.subtitlePath.isEmpty() && !writeSubtitleFile(tl, s.format.rate.fps(), m_from, m_from + m_length, s.subtitlePath, error)) {
        cleanup();
        return false;
    }
    // Andere Bildrate: Timeline (mit In/Out) als XML schreiben und im Profil der Ausgabe wieder einlesen
    const FrameRate rate = s.rate.num > 0 && s.rate.den > 0 ? s.rate : s.format.rate;
    const bool convertRate = rate != s.format.rate && !s.videoCodec.isEmpty();
    if (convertRate) {
        m_xmlPath = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                        .filePath(QString("schneidi-export-%1.mlt").arg(QUuid::createUuid().toString(QUuid::Id128)));
        Mlt::Consumer xml(*m_profile, "xml", m_xmlPath.toUtf8().constData());
        xml.set("no_meta", 1);
        xml.connect(*m_tractor);
        xml.start(); // schreibt sofort
        xml.stop();
        ProjectFormat target = out;
        target.rate = rate;
        m_outProfile = makeProfile(target);
        // über den Loader ("consumer:<datei>"): der hängt die Umwandler an (Tonformat, Farbraum), sonst kommt der Ton
        // im falschen Sampleformat beim Encoder an
        // Der Producer arbeitet intern mit einer Kopie dieses Profils; nur wenn es nicht „explicit“ ist, übernimmt er
        // das Profil (Bildrate der Timeline) aus dem XML – sonst liefe die Timeline in der neuen Bildrate zu schnell
        m_outProfile->set_explicit(0);
        m_source = std::make_unique<Mlt::Producer>(*m_outProfile, ("consumer:" + m_xmlPath).toUtf8().constData());
        m_outProfile->set_explicit(1);
        if (!qFuzzyCompare(m_outProfile->fps(), rate.fps())) { // darf sich dabei nicht ändern
            if (error) *error = T("Bildrate kann nicht umgerechnet werden.");
            cleanup();
            return false;
        }
        if (!m_source->is_valid()) {
            if (error) *error = T("Bildrate kann nicht umgerechnet werden (MLT-Modul „consumer“ fehlt).");
            cleanup();
            return false;
        }
        // Der Producer kennt nur die Bildzahl der Timeline -> Länge in der neuen Bildrate selbst setzen
        m_length = std::max(1, int(std::lround(m_length * rate.fps() / s.format.rate.fps())));
        m_source->set("length", m_length);
        m_source->set_in_and_out(0, m_length - 1);
    } else {
        m_source = std::make_unique<Mlt::Producer>(*m_tractor);
    }
    m_settings = s;
    m_convertRate = convertRate;
    m_threads = threads;
    // Render every frame (real_time < 0), several frames at once (not with frei0r, see parallelFrames)
    m_frames = convertRate ? 1 : parallelFrames(tl, threads);
    m_path = s.path;
    m_note = s.encoderNote;
    m_usedCodec.clear();

    // Hardware encoder: test it with the output size first (MLT's avformat consumer crashes when the encoder cannot
    // be opened, e.g. no such card, too large for the card, all its sessions busy). Runs ffmpeg in the background.
    if (HardwareEncoders::isHardwareCodec(s.videoCodec)) {
        Mlt::Profile& p = convertRate ? *m_outProfile : *m_profile;
        const QSize size(p.width(), p.height());
        const int known = HardwareEncoders::cachedResult(s.videoCodec, size);
        if (known == 0) {
            m_check = std::make_unique<QProcess>();
            m_check->setProcessChannelMode(QProcess::MergedChannels);
            connect(m_check.get(), &QProcess::finished, this, [this, size](int code, QProcess::ExitStatus status) {
                const bool works = status == QProcess::NormalExit && code == 0;
                HardwareEncoders::remember(m_settings.videoCodec, size, works);
                if (!works)
                    qWarning("Export: %s test failed: %s", qPrintable(m_settings.videoCodec),
                             m_check->readAll().trimmed().constData());
                onCheckFinished(works);
            });
            connect(m_check.get(), &QProcess::errorOccurred, this, [this, size](QProcess::ProcessError e) {
                if (e != QProcess::FailedToStart) return;
                HardwareEncoders::remember(m_settings.videoCodec, size, false);
                onCheckFinished(false);
            });
            m_check->start(Bundle::tool("ffmpeg"), HardwareEncoders::testArguments(s.videoCodec, size, s.crf));
            ++g_running;
            m_timer.start(); // poll() waits for the consumer
            emit progress(0);
            return true;
        }
        if (known < 0) {
            if (!fallBackToSoftware(error)) {
                cleanup();
                return false;
            }
            ++g_running;
            m_timer.start();
            emit progress(0);
            return true;
        }
    }
    if (!startConsumer(s.videoCodec, error)) {
        cleanup();
        return false;
    }
    ++g_running;
    m_timer.start();
    emit progress(0);
    return true;
}

void Exporter::onCheckFinished(bool works)
{
    if (!m_check) return;
    // Not deleted right here: we are inside a signal of m_check
    m_check.release()->deleteLater();
    QString error;
    const bool ok = works ? startConsumer(m_settings.videoCodec, &error) : fallBackToSoftware(&error);
    if (!ok) {
        const QString path = m_path;
        cleanup();
        QFile::remove(path);
        emit finished(false, error);
    }
}

bool Exporter::fallBackToSoftware(QString* error)
{
    const QString hw = m_usedCodec.isEmpty() ? m_settings.videoCodec : m_usedCodec;
    if (m_settings.softwareCodec.isEmpty() || !HardwareEncoders::isHardwareCodec(hw)) {
        if (error) *error = T("Hardware-Encoder %1 funktioniert nicht.").arg(hw);
        return false;
    }
    qWarning("Export: hardware encoder %s failed, rendering with %s", qPrintable(hw), qPrintable(m_settings.softwareCodec));
    m_note = T("Hardware-Encoder %1 funktioniert nicht, mit Software gerendert.")
                 .arg(videoEncoderLabel(HardwareEncoders::encoderOfCodec(hw)));
    return startConsumer(m_settings.softwareCodec, error);
}

bool Exporter::startConsumer(const QString& codec, QString* error)
{
    const ExportSettings& s = m_settings;
    Mlt::Profile& outProfile = m_convertRate ? *m_outProfile : *m_profile;
    if (m_consumer) { // again (fallback): the old one has stopped
        m_consumer->stop();
        m_stoppedEvent.reset();
        m_fatalEvent.reset();
        m_consumer.reset();
    }
    m_consumer = std::make_unique<Mlt::Consumer>(outProfile, "avformat", s.path.toUtf8().constData());
    if (!m_consumer->is_valid()) {
        if (error) *error = T("FFmpeg-Ausgabe (avformat) nicht verfügbar.");
        return false;
    }
    m_usedCodec = codec;
    if (s.videoCodec.isEmpty()) {
        m_consumer->set("vn", 1); // audio only
    } else {
        m_consumer->set("vcodec", codec.toUtf8().constData());
        if (HardwareEncoders::isHardwareCodec(codec)) {
            m_consumer->set("pix_fmt", HardwareEncoders::pixelFormat(codec).toUtf8().constData());
            if (codec.endsWith("_vaapi"))
                m_consumer->set("vaapi_device", HardwareEncoders::vaapiDevice().toUtf8().constData());
            for (const auto& [key, value] : HardwareEncoders::codecOptions(codec, s.crf))
                m_consumer->set(key.toUtf8().constData(), value.toUtf8().constData());
        } else {
            m_consumer->set("pix_fmt", s.pixFmt.toUtf8().constData());
            if (codec == "libx264" || codec == "libx265") {
                m_consumer->set("crf", s.crf);
                m_consumer->set("preset", s.preset.toUtf8().constData());
            }
        }
    }
    m_consumer->set("acodec", s.audioCodec.toUtf8().constData());
    if (!s.audioSampleFormat.isEmpty()) m_consumer->set("sample_fmt", s.audioSampleFormat.toUtf8().constData());
    if (s.audioRate > 0) m_consumer->set("frequency", s.audioRate);
    if (s.audioChannels > 0) m_consumer->set("channels", s.audioChannels);
    if (!s.audioCodec.startsWith("pcm_") && s.audioCodec != "alac") {
        const int bitrate = s.audioCodec == "libmp3lame" ? std::min(s.audioBitrateK, 320) : s.audioBitrateK;
        m_consumer->set("ab", QString("%1k").arg(bitrate).toUtf8().constData());
    }
    const QString ext = QFileInfo(s.path).suffix().toLower();
    if (ext == "mp4" || ext == "mov" || ext == "m4a") m_consumer->set("movflags", "+faststart");
    // Decoder (start) and encoder get the same thread count. When converting the frame rate the "consumer" producer
    // fetches frames in order -> one at a time there, and without the read-ahead thread (real_time 0): repeated
    // frames deliver 0 samples; the read-ahead thread fetches the sound early, and on the encoder's second fetch
    // MLT fills the empty frames with silence (sound too long, with jumps).
    m_consumer->set("real_time", m_convertRate ? 0 : -m_frames);
    m_consumer->set("threads", m_threads);
    qInfo("Export: %s, %d threads, %d frames at once%s", s.videoCodec.isEmpty() ? "audio only" : qPrintable(codec),
          m_threads, m_frames, m_convertRate ? ", frame rate converted" : "");
    m_consumer->set("terminate_on_pause", 1); // stop by itself at the end
    m_consumer->connect(*m_source);

    // avformat sets "running" only AFTER pthread_create: when the encoder thread is faster (short audio exports
    // under load), it sees running=0, ends without a frame and afterwards running stays 1 -> is_stopped() never true.
    // So also detect the end of the thread through the event (see poll).
    m_threadDone = false;
    m_fatal = false;
    m_restarts = 0;
    m_stoppedEvent.reset(m_consumer->listen("consumer-stopped", &m_threadDone, (mlt_listener)onConsumerStopped));
    m_fatalEvent.reset(m_consumer->listen("consumer-fatal-error", &m_fatal, (mlt_listener)onConsumerStopped));

    m_source->set_speed(1);
    m_source->seek(0); // position counts from the in point (seek(m_from) dropped the first m_from frames with In/Out)
    if (m_consumer->start() != 0) {
        if (error) *error = T("Export konnte nicht gestartet werden.");
        return false;
    }
    return true;
}
void Exporter::poll()
{
    if (!m_consumer) return;
    // Race at the start (see start): the thread saw running=0 and ended without a frame. Either running=1 comes
    // after its end (never stopped), or before mlt_consumer_stopped() puts it back to 0 (looks finished, but the
    // file is empty) -> start again in both cases
    const bool nothingWritten = m_consumer->is_stopped() && QFileInfo(m_path).size() <= 0;
    if (m_threadDone && (!m_consumer->is_stopped() || nothingWritten)) {
        if (++m_restarts <= 3) {
            qWarning("Export: encoder thread ended at once, restart %d", m_restarts);
            m_consumer->stop(); // sets running=0, the thread has ended already
            m_threadDone = false;
            m_source->seek(0);
            if (m_consumer->start() == 0) return;
        }
        m_consumer->stop();
    }
    // Hardware encoder failed while rendering (fatal error or nothing written): render again with software
    // (not when the file cannot be written at all: that is no encoder problem)
    const bool hardware = HardwareEncoders::isHardwareCodec(m_usedCodec) && !m_settings.softwareCodec.isEmpty();
    const QFileInfo dir(QFileInfo(m_path).absolutePath());
    if (hardware && (m_fatal || (m_consumer->is_stopped() && QFileInfo(m_path).size() <= 0)) && dir.isDir()
        && dir.isWritable()) {
        m_consumer->stop();
        const Mlt::Profile& p = m_convertRate ? *m_outProfile : *m_profile;
        HardwareEncoders::remember(m_usedCodec, QSize(p.width(), p.height()), false); // not again this session
        QFile::remove(m_path);
        QString error;
        if (fallBackToSoftware(&error)) return;
        const QString path = m_path;
        cleanup();
        QFile::remove(path);
        emit finished(false, error);
        return;
    }
    if (m_consumer->is_stopped()) {
        const QString path = m_path;
        cleanup();
        // Encoder or container failed (e.g. codec missing): no file or an empty one
        if (QFileInfo(path).size() <= 0) {
            QFile::remove(path);
            emit finished(false, T("Export fehlgeschlagen: %1").arg(path));
            return;
        }
        emit progress(100);
        emit finished(true, T("Export fertig: %1").arg(path));
        return;
    }
    const int pos = m_source->position(); // relativ zum In-Punkt
    emit progress(std::clamp(pos * 100 / std::max(1, m_length), 0, 99));
}

bool Exporter::runAnalyses(const Timeline& tl, const ExportSettings& s)
{
    if (!Stabilizer::available()) return false; // the effect does nothing here (Inspector says so)
    const QVector<Stabilizer::Request> todo = Stabilizer::missing(tl, s.format);
    if (todo.isEmpty()) return false;
    qInfo("Export: stabilization analysis of %lld clip(s) first", qlonglong(todo.size()));
    m_pendingTimeline = tl;
    m_pendingSettings = s;
    m_analysisCancel = std::make_shared<std::atomic<bool>>(false);
    auto cancel = m_analysisCancel;
    auto ok = std::make_shared<bool>(true);
    auto error = std::make_shared<QString>();
    m_analysis = QThread::create([todo, cancel, ok, error] {
        for (const Stabilizer::Request& r : todo) {
            if (Stabilizer::find(r).valid()) continue; // the preview finished it meanwhile
            if (!(*ok = Stabilizer::analyze(r, error.get(), cancel.get()))) return;
        }
    });
    connect(m_analysis, &QThread::finished, this, [this, ok, error] {
        m_analysis->wait();
        delete m_analysis;
        m_analysis = nullptr;
        --g_running;
        if (!*ok) {
            emit finished(false, T("Stabilisieren fehlgeschlagen: %1").arg(*error));
            return;
        }
        m_analysisDone = true;
        const Timeline tl = std::exchange(m_pendingTimeline, Timeline{});
        QString startError;
        if (!start(tl, m_pendingSettings, &startError)) emit finished(false, startError);
    });
    ++g_running; // the render cache waits as during the export itself
    m_analysis->start();
    emit progress(0);
    return true;
}

void Exporter::cancel()
{
    if (m_analysis) { // still analyzing for stabilization
        *m_analysisCancel = true;
        m_analysis->disconnect(this);
        m_analysis->wait();
        delete m_analysis;
        m_analysis = nullptr;
        --g_running;
        m_pendingTimeline = {};
        emit finished(false, T("Export abgebrochen."));
        return;
    }
    if (!isRunning()) return;
    if (m_consumer) m_consumer->stop();
    const QString path = m_path;
    cleanup();
    QFile::remove(path); // halbfertige Datei weg
    emit finished(false, T("Export abgebrochen."));
}

void Exporter::cleanup()
{
    if (m_timer.isActive()) --g_running;
    m_timer.stop();
    // Immer stop(): beendet sich der Consumer selbst (terminate_on_pause), laufen seine Worker-Threads (real_time < -1)
    // sonst weiter – mlt_consumer_close stoppt sie nicht – und rechnen später auf freigegebenen Profilen/Producern
    if (m_check) { // test encode still running: only our own ffmpeg process
        m_check->disconnect(this);
        m_check->kill();
        m_check->waitForFinished(2000);
        m_check.reset();
    }
    if (m_consumer) m_consumer->stop();
    m_stoppedEvent.reset();
    m_fatalEvent.reset();
    m_consumer.reset();
    m_source.reset();
    m_outProfile.reset();
    if (!m_xmlPath.isEmpty()) QFile::remove(m_xmlPath);
    m_xmlPath.clear();
    m_tractor.reset();
    m_builder.reset();
    m_profile.reset();
}
