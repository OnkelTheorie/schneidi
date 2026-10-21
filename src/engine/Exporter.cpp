#include "engine/Exporter.h"

#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "core/RenderJob.h"
#include "core/Subtitles.h"
#include "core/TimelineOps.h"
#include "engine/Bundle.h"
#include "engine/HardwareEncoders.h"
#include "engine/OutputColor.h"
#include "engine/Profiles.h"
#include "engine/RateConverter.h"
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
std::atomic<int> g_running{0}; // running exports (m_timer runs exactly then)

// Visible subtitle track (otherwise the first one with entries) in the range [from, to) as SRT, times from the start of the range
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

// Runs in the avformat encoder thread, right before it ends
void onConsumerStopped(mlt_properties, void* done, mlt_event_data)
{
    static_cast<std::atomic<bool>*>(done)->store(true);
}

} // namespace

bool Exporter::sameFile(const QString& a, const QString& b)
{
    const QString ca = QFileInfo(a).canonicalFilePath(), cb = QFileInfo(b).canonicalFilePath();
    if (ca.isEmpty() || cb.isEmpty()) return false; // one is missing -> cannot be the same
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
    if (m_build) stopBuild();
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

    // Output size different from the timeline: scale pixel values (position, titles …) along
    ProjectFormat out = s.format;
    Timeline scaled = tl;
    if (!s.size.isEmpty() && s.size != s.format.size()) {
        out.width = s.size.width() & ~1;
        out.height = s.size.height() & ~1;
        scaleTimeline(scaled, s.format.size(), out.size());
    }
    // Audio only: video tracks carry no sound (TimelineBuilder hides it), so skip decoding their images
    if (s.videoCodec.isEmpty()) scaled.video.clear();
    // Muted (or not soloed) audio tracks and hidden video tracks add nothing to the file: leave their clips out, so
    // their media is neither opened (slow start) nor decoded while rendering. The length stays (end is computed above)
    const bool anySolo = std::any_of(scaled.audio.cbegin(), scaled.audio.cend(), [](const Track& t) { return t.solo; });
    for (Track& t : scaled.audio)
        if (t.muted || (anySolo && !t.solo)) t.clips.clear();
    for (Track& t : scaled.video)
        if (t.hidden) t.clips.clear();

    QDir().mkpath(QFileInfo(s.path).absolutePath());
    if (s.output == ExportSettings::Output::ImageSequence) QDir().mkpath(imageSequenceDir(s.path));
    if (!s.subtitlePath.isEmpty() && !writeSubtitleFile(tl, s.format.rate.fps(), m_from, m_from + m_length, s.subtitlePath, error))
        return false;
    // Different frame rate: play the timeline in the output's profile (engine/RateConverter)
    const FrameRate rate = s.rate.num > 0 && s.rate.den > 0 ? s.rate : s.format.rate;
    const bool convertRate = rate != s.format.rate && !s.videoCodec.isEmpty();
    const int threads = s.threads > 0 ? s.threads : availableThreads();
    m_settings = s;
    m_convertRate = convertRate;
    m_threads = threads;
    // Render every frame (real_time < 0), several frames at once (not with frei0r, see parallelFrames)
    m_frames = convertRate ? 1 : parallelFrames(scaled, threads); // effects on left-out tracks do not count
    m_target = s.path;
    switch (s.output) {
    case ExportSettings::Output::File: m_path = s.path; break;
    case ExportSettings::Output::ImageSequence: m_path = imageSequencePattern(s.path); break;
    case ExportSettings::Output::Gif:
        m_path = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                     .filePath(QString("schneidi-gif-%1.mov").arg(QUuid::createUuid().toString(QUuid::Id128)));
        break;
    }
    m_note = s.encoderNote;
    m_usedCodec.clear();

    // Building the MLT graph opens every file of the timeline (probing all streams, ~0.1 s per file even in
    // parallel): in a thread, so the window stays responsive. launch() continues in this thread.
    auto graph = std::make_shared<Graph>();
    const int from = m_from, length = m_length;
    m_buildCancel = std::make_shared<std::atomic<bool>>(false);
    m_build = QThread::create([graph, cancel = m_buildCancel, scaled = std::move(scaled), out, s, threads, end, from,
                               length, rate, convertRate] {
        Graph& g = *graph;
        g.profile = makeProfile(out);
        g.builder = std::make_unique<TimelineBuilder>(*g.profile);
        g.builder->setCancelFlag(cancel);
        g.builder->setDecoderThreads(threads);
        g.builder->setSubtitles(s.burnSubtitles && !s.videoCodec.isEmpty());
        g.tractor = g.builder->build(scaled);
        if (*cancel) return;
        if (s.videoCodec.isEmpty()) { // keep the full length when the sound ends before the picture
            Mlt::Playlist blank(*g.profile);
            blank.blank(end - 1);
            blank.set("hide", 3);
            g.tractor->set_track(blank, g.tractor->count());
        }
        g.tractor->set_in_and_out(from, from + length - 1);
        g.length = length;
        if (convertRate) {
            ProjectFormat target = out;
            target.rate = rate;
            g.outProfile = makeProfile(target);
            g.source = RateConverter::make(*g.outProfile, *g.profile, *g.tractor);
            if (!g.source || !g.source->is_valid()) {
                g.error = T("Bildrate kann nicht umgerechnet werden.");
                return;
            }
            // Length in the new frame rate
            g.length = std::max(1, int(std::lround(length * rate.fps() / s.format.rate.fps())));
            g.source->set("length", g.length);
            g.source->set_in_and_out(0, g.length - 1);
        } else {
            g.source = std::make_unique<Mlt::Producer>(*g.tractor);
        }
        // The workers deliver YUV in the output's color standard, the encoder thread only repacks (see OutputColor.h)
        if (!s.videoCodec.isEmpty())
            if (auto f = OutputColor::make(convertRate ? *g.outProfile : *g.profile)) g.source->attach(*f);
    });
    connect(m_build, &QThread::finished, this, [this, graph] {
        m_build->wait();
        delete m_build;
        m_build = nullptr;
        --g_running;
        if (!graph->error.isEmpty()) {
            emit finished(false, graph->error);
            return;
        }
        m_profile = std::move(graph->profile);
        m_builder = std::move(graph->builder);
        m_tractor = std::move(graph->tractor);
        m_outProfile = std::move(graph->outProfile);
        m_source = std::move(graph->source);
        m_length = graph->length;
        QString error;
        if (!launch(&error)) {
            const QString path = m_path;
            cleanup();
            QFile::remove(path);
            emit finished(false, error);
        }
    });
    ++g_running; // the render cache waits already
    m_build->start();
    emit progress(0);
    return true;
}

bool Exporter::launch(QString* error)
{
    const ExportSettings& s = m_settings;
    // Hardware encoder: test it with the output size first (MLT's avformat consumer crashes when the encoder cannot
    // be opened, e.g. no such card, too large for the card, all its sessions busy). Runs ffmpeg in the background.
    if (HardwareEncoders::isHardwareCodec(s.videoCodec)) {
        Mlt::Profile& p = m_convertRate ? *m_outProfile : *m_profile;
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
            return true;
        }
        if (known < 0) {
            if (!fallBackToSoftware(error)) return false;
            ++g_running;
            m_timer.start();
            return true;
        }
    }
    if (!startConsumer(s.videoCodec, error)) return false;
    ++g_running;
    m_timer.start();
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
    m_consumer = std::make_unique<Mlt::Consumer>(outProfile, "avformat", m_path.toUtf8().constData());
    if (!m_consumer->is_valid()) {
        if (error) *error = T("FFmpeg-Ausgabe (avformat) nicht verfügbar.");
        return false;
    }
    m_usedCodec = codec;
    if (s.videoCodec.isEmpty()) {
        m_consumer->set("vn", 1); // audio only
        // vn only drops the video encoder: without video_off MLT's render threads still fetch (decode) every image
        m_consumer->set("video_off", 1);
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
            if (s.crf > 0) m_consumer->set("crf", s.crf); // H.264/H.265/AV1/VP9; fixed-quality formats have 0
            if (codec == "libx264" || codec == "libx265") m_consumer->set("preset", s.preset.toUtf8().constData());
        }
        // 4:2:0 output: the workers deliver yuv420p (MLT asks for yuv422 otherwise), so the encoder thread only copies
        // or interleaves instead of subsampling every frame a second time (~10 % faster, one chroma step less)
        const QByteArray pixFmt = m_consumer->get("pix_fmt");
        if (pixFmt == "yuv420p" || pixFmt == "nv12") m_consumer->set("mlt_image_format", "yuv420p");
    }
    if (!s.videoCodec.isEmpty() && s.maxBitrateK > 0) { // quality target as before, but capped (VBV)
        m_consumer->set("maxrate", QString("%1k").arg(s.maxBitrateK).toUtf8().constData());
        m_consumer->set("bufsize", QString("%1k").arg(s.maxBitrateK * 2).toUtf8().constData());
    }
    for (const QString& option : s.codecOptions) { // "key=value" from the format (profile, speed …)
        const qsizetype eq = option.indexOf('=');
        if (eq > 0) m_consumer->set(option.left(eq).toUtf8().constData(), option.mid(eq + 1).toUtf8().constData());
    }
    if (s.output == ExportSettings::Output::ImageSequence) m_consumer->set("f", "image2"); // one file per frame
    if (s.audioCodec.isEmpty()) m_consumer->set("an", 1); // GIF, image sequence: no sound
    else m_consumer->set("acodec", s.audioCodec.toUtf8().constData());
    if (!s.audioSampleFormat.isEmpty()) m_consumer->set("sample_fmt", s.audioSampleFormat.toUtf8().constData());
    if (s.audioRate > 0) m_consumer->set("frequency", s.audioRate);
    if (s.audioChannels > 0) m_consumer->set("channels", s.audioChannels);
    if (!s.audioCodec.isEmpty() && !s.audioCodec.startsWith("pcm_") && s.audioCodec != "alac" && s.audioCodec != "flac") {
        const int bitrate = s.audioCodec == "libmp3lame" ? std::min(s.audioBitrateK, 320) : s.audioBitrateK;
        m_consumer->set("ab", QString("%1k").arg(bitrate).toUtf8().constData());
    }
    const QString ext = QFileInfo(m_path).suffix().toLower();
    if ((ext == "mp4" || ext == "mov" || ext == "m4a") && s.output == ExportSettings::Output::File)
        m_consumer->set("movflags", "+faststart");
    // Decoder (start) and encoder get the same thread count. When converting the frame rate the RateConverter
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
    const bool nothingWritten = m_consumer->is_stopped() && !wroteOutput();
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
        const bool wrote = wroteOutput();
        cleanup();
        // Encoder or container failed (e.g. codec missing): no file or an empty one
        if (!wrote) {
            QFile::remove(path);
            emit finished(false, T("Export fehlgeschlagen: %1").arg(m_target));
            return;
        }
        if (m_settings.output == ExportSettings::Output::Gif) {
            convertToGif();
            return;
        }
        emit progress(100);
        emit finished(true, T("Export fertig: %1").arg(m_target));
        return;
    }
    const int pos = m_source->position(); // relative to the In point
    // GIF: the conversion afterwards gets the last tenth
    const int scale = m_settings.output == ExportSettings::Output::Gif ? 90 : 100;
    emit progress(std::clamp(pos * scale / std::max(1, m_length), 0, scale - 1));
}

bool Exporter::wroteOutput() const
{
    if (m_settings.output == ExportSettings::Output::ImageSequence)
        return QFileInfo(QString(m_path).replace("%05d", "00001")).size() > 0;
    return QFileInfo(m_path).size() > 0;
}

void Exporter::convertToGif()
{
    // One palette for the whole clip from the changing parts (stats_mode=diff: moving things get the colors),
    // error diffusion only where pixels change (diff_mode=rectangle: smaller file, no flicker in still areas)
    const QString part = m_target + ".part";
    m_gif = std::make_unique<QProcess>();
    m_gif->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_gif.get(), &QProcess::readyReadStandardOutput, this, [this] {
        // -progress pipe:1 reports "frame=N" lines
        for (const QByteArray& line : m_gif->readAllStandardOutput().split('\n'))
            if (line.startsWith("frame="))
                emit progress(std::clamp(90 + line.mid(6).trimmed().toInt() * 10 / std::max(1, m_length), 90, 99));
    });
    connect(m_gif.get(), &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        finishGif(status == QProcess::NormalExit && code == 0, QString::fromUtf8(m_gif->readAllStandardError()).trimmed());
    });
    connect(m_gif.get(), &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) finishGif(false, T("ffmpeg nicht gefunden."));
    });
    ++g_running; // the render cache keeps waiting
    m_gif->start(Bundle::tool("ffmpeg"),
                 {"-hide_banner", "-nostdin", "-nostats", "-loglevel", "error", "-progress", "pipe:1", "-i", m_path, "-vf",
                  "split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=sierra2_4a:diff_mode=rectangle",
                  "-loop", "0", "-f", "gif", "-y", part});
    emit progress(90);
}

void Exporter::finishGif(bool ok, const QString& log)
{
    if (!m_gif) return;
    m_gif->disconnect(this);
    m_gif.release()->deleteLater(); // inside one of its signals
    --g_running;
    QFile::remove(m_path); // intermediate
    const QString part = m_target + ".part";
    if (ok) {
        QFile::remove(m_target);
        ok = QFile::rename(part, m_target);
    }
    if (!ok) {
        if (!log.isEmpty()) qWarning("Export: GIF conversion failed: %s", qPrintable(log));
        QFile::remove(part);
        emit finished(false, T("Export fehlgeschlagen: %1").arg(m_target));
        return;
    }
    m_usedCodec = "gif"; // what wrote the file (not the intermediate)
    emit progress(100);
    emit finished(true, T("Export fertig: %1").arg(m_target));
}

bool Exporter::runAnalyses(const Timeline& tl, const ExportSettings& s)
{
    if (!Stabilizer::available()) return false; // the effect does nothing here (Inspector says so)
    if (s.videoCodec.isEmpty()) return false;   // audio only (also transcribe/loudness): no picture to stabilize
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

void Exporter::stopBuild()
{
    // Not left running detached: it would still open files while the program ends (MLT closed: crash)
    *m_buildCancel = true;
    m_build->disconnect(this);
    m_build->wait();
    delete m_build;
    m_build = nullptr;
    --g_running;
}

void Exporter::cancel()
{
    if (m_build) { // still building the graph
        stopBuild();
        emit finished(false, T("Export abgebrochen."));
        return;
    }
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
    if (m_gif) { // GIF conversion: only our own ffmpeg process
        m_gif->disconnect(this);
        m_gif->kill();
        m_gif->waitForFinished(2000);
        m_gif.reset();
        --g_running;
        QFile::remove(m_path);
        QFile::remove(m_target + ".part");
        emit finished(false, T("Export abgebrochen."));
        return;
    }
    if (!isRunning()) return;
    if (m_consumer) m_consumer->stop();
    const QString path = m_path;
    cleanup();
    QFile::remove(path); // half-finished file gone
    emit finished(false, T("Export abgebrochen."));
}

void Exporter::cleanup()
{
    if (m_timer.isActive()) --g_running;
    m_timer.stop();
    // Always stop(): if the consumer ends itself (terminate_on_pause), its worker threads (real_time < -1) keep running
    // otherwise – mlt_consumer_close does not stop them – and later compute on freed profiles/producers
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
    m_tractor.reset();
    m_builder.reset();
    m_profile.reset();
}
