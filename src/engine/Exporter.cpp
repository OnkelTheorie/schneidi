#include "engine/Exporter.h"

#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "core/Subtitles.h"
#include "core/TimelineOps.h"
#include "engine/Profiles.h"
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

int Exporter::savedCores()
{
    return std::max(0, QSettings().value("render/cores", 0).toInt());
}

void Exporter::setSavedCores(int cores)
{
    QSettings().setValue("render/cores", std::max(0, cores));
}

int Exporter::availableCores()
{
    return std::max(1, QThread::idealThreadCount());
}

int Exporter::parallelFrames(const Timeline& tl, int cores)
{
    bool frei0r = usesFrei0r(tl);
    if (tl.nested)
        for (const Timeline& n : *tl.nested) frei0r = frei0r || usesFrei0r(n);
    return frei0r ? 1 : (cores > 0 ? cores : availableCores());
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

    // Andere Ausgabegröße als die Timeline: Pixelwerte (Position, Titel …) mitskalieren
    ProjectFormat out = s.format;
    Timeline scaled = tl;
    if (!s.size.isEmpty() && s.size != s.format.size()) {
        out.width = s.size.width() & ~1;
        out.height = s.size.height() & ~1;
        scaleTimeline(scaled, s.format.size(), out.size());
    }
    m_profile = makeProfile(out);
    m_builder = std::make_unique<TimelineBuilder>(*m_profile);
    const int cores = s.cores > 0 ? s.cores : availableCores();
    m_builder->setDecoderThreads(cores);
    m_builder->setSubtitles(s.burnSubtitles && !s.videoCodec.isEmpty());
    m_tractor = m_builder->build(scaled);
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
    Mlt::Profile& outProfile = convertRate ? *m_outProfile : *m_profile;

    m_consumer = std::make_unique<Mlt::Consumer>(outProfile, "avformat", s.path.toUtf8().constData());
    if (!m_consumer->is_valid()) {
        if (error) *error = T("FFmpeg-Ausgabe (avformat) nicht verfügbar.");
        cleanup();
        return false;
    }
    if (s.videoCodec.isEmpty()) {
        m_consumer->set("vn", 1); // nur Audio
    } else {
        m_consumer->set("vcodec", s.videoCodec.toUtf8().constData());
        m_consumer->set("pix_fmt", s.pixFmt.toUtf8().constData());
        if (s.videoCodec == "libx264" || s.videoCodec == "libx265") {
            m_consumer->set("crf", s.crf);
            m_consumer->set("preset", s.preset.toUtf8().constData());
        }
    }
    m_consumer->set("acodec", s.audioCodec.toUtf8().constData());
    if (!s.audioSampleFormat.isEmpty()) m_consumer->set("sample_fmt", s.audioSampleFormat.toUtf8().constData());
    if (!s.audioCodec.startsWith("pcm_") && s.audioCodec != "alac") {
        const int bitrate = s.audioCodec == "libmp3lame" ? std::min(s.audioBitrateK, 320) : s.audioBitrateK;
        m_consumer->set("ab", QString("%1k").arg(bitrate).toUtf8().constData());
    }
    const QString ext = QFileInfo(s.path).suffix().toLower();
    if (ext == "mp4" || ext == "mov" || ext == "m4a") m_consumer->set("movflags", "+faststart");
    // Jedes Frame rendern (real_time < 0), mehrere Bilder gleichzeitig (außer mit frei0r, siehe parallelFrames);
    // Decoder (oben) und Encoder bekommen dieselbe Kernzahl. Beim Umrechnen der Bildrate holt der "consumer"-Producer
    // die Bilder der Reihe nach -> dort nur eins nach dem anderen, und ohne Vorlese-Thread (real_time 0): Wiederholte
    // Bilder liefern 0 Samples; der Vorlese-Thread holt den Ton vorab, und beim zweiten Abholen durch den Encoder
    // füllt MLT die leeren Bilder mit Stille auf (Ton zu lang, mit Sprüngen).
    const int frames = convertRate ? 1 : parallelFrames(tl, cores);
    m_consumer->set("real_time", convertRate ? 0 : -frames);
    m_consumer->set("threads", cores);
    qInfo("Export: %d Kerne, %d Bilder gleichzeitig%s", cores, frames,
          convertRate ? qPrintable(QString(", Bildrate %1 -> %2").arg(s.format.rate.label(), rate.label())) : "");
    m_consumer->set("terminate_on_pause", 1); // am Ende automatisch stoppen
    m_consumer->connect(*m_source);

    // avformat setzt "running" erst NACH pthread_create: ist der Encoder-Thread schneller (kurze Audio-Exporte unter
    // Last), sieht er running=0, beendet sich ohne ein Bild und danach bleibt running=1 -> is_stopped() nie wahr.
    // Das Ende des Threads daher zusätzlich über das Ereignis erkennen (siehe poll).
    m_threadDone = false;
    m_restarts = 0;
    m_stoppedEvent.reset(m_consumer->listen("consumer-stopped", &m_threadDone, (mlt_listener)onConsumerStopped));

    m_path = s.path;
    m_source->set_speed(1);
    m_source->seek(0); // Position zählt ab dem In-Punkt (seek(m_from) ließ bei In/Out die ersten m_from Frames weg)
    if (m_consumer->start() != 0) {
        if (error) *error = T("Export konnte nicht gestartet werden.");
        cleanup();
        return false;
    }
    ++g_running;
    m_timer.start();
    emit progress(0);
    return true;
}

void Exporter::poll()
{
    if (!m_consumer) return;
    if (m_threadDone && !m_consumer->is_stopped()) {
        // Wettlauf beim Start (siehe start): Thread weg, nichts gerendert -> neu starten
        if (++m_restarts <= 3) {
            qWarning("Export: Encoder-Thread endete sofort, Neustart %d", m_restarts);
            m_consumer->stop(); // setzt running=0, Thread ist schon beendet
            m_threadDone = false;
            m_source->seek(0);
            if (m_consumer->start() == 0) return;
        }
        m_consumer->stop();
    }
    if (m_consumer->is_stopped()) {
        const QString path = m_path;
        cleanup();
        // Encoder/Container gescheitert (z. B. Codec fehlt): keine oder leere Datei
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

void Exporter::cancel()
{
    if (!m_consumer) return;
    m_consumer->stop();
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
    if (m_consumer) m_consumer->stop();
    m_stoppedEvent.reset();
    m_consumer.reset();
    m_source.reset();
    m_outProfile.reset();
    if (!m_xmlPath.isEmpty()) QFile::remove(m_xmlPath);
    m_xmlPath.clear();
    m_tractor.reset();
    m_builder.reset();
    m_profile.reset();
}
