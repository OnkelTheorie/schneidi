#include "engine/ProxyManager.h"

#include "core/I18n.h"

#include <Mlt.h>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <memory>

namespace {

constexpr const char* kSettingKey = "playback/useProxy";

// Was ffmpeg über die Quelle wissen muss; gelesen über MLT, damit der Proxy genau so interpretiert wird
// wie das Original (gleiche Framerate, gleiche Farbmatrix)
struct SourceInfo {
    bool ok = false;
    bool hasVideo = false;
    int rateNum = 0, rateDen = 0;
    int colorspace = 709; // MLT: 601/709/240/2020
    bool fullRange = false;
    double seconds = 0;
};

struct ProxyJob {
    QString error;
    QStringList args; // ffmpeg-Argumente
    double durationUs = 0;
};

SourceInfo probeSource(const QString& path)
{
    SourceInfo s;
    Mlt::Profile profile("atsc_1080p_25"); // nur fürs Öffnen; Länge wird unten in Sekunden umgerechnet
    Mlt::Producer p(profile, path.toUtf8().constData());
    if (!p.is_valid()) return s;
    const QByteArray svc = p.get("mlt_service");
    if (svc == "qimage" || svc == "pixbuf") return s; // Standbilder brauchen keinen Proxy
    s.ok = true;
    s.hasVideo = p.get_int("video_index") >= 0;
    // meta.media.frame_rate_*/colorspace setzt MLT erst beim ersten Frame
    // (Framerate = r_frame_rate, bei variabler Framerate also die höchste -> CFR verdoppelt nur Bilder)
    std::unique_ptr<Mlt::Frame> frame(p.get_frame());
    s.rateNum = p.get_int("meta.media.frame_rate_num");
    s.rateDen = p.get_int("meta.media.frame_rate_den");
    // Ohne Angabe in der Datei rät MLT nach der Bildhöhe (unter 720 -> BT.601); der Wert hier ist schon das Ergebnis
    if (const int cs = p.get_int("meta.media.colorspace"); cs > 0) s.colorspace = cs;
    else if (const int h = p.get_int("meta.media.height"); h > 0) s.colorspace = h < 720 ? 601 : 709;
    s.fullRange = qstrcmp(p.get("meta.media.color_range"), "jpeg") == 0;
    s.seconds = p.get_length() / profile.fps();
    return s;
}

// ffmpeg-Namen zur MLT-Farbmatrix (Proxy bekommt die Matrix des Originals als Kennzeichnung,
// sonst hält MLT ein 540p-Bild ohne Angabe für BT.601 und die Farben verschieben sich)
const char* ffColorspace(int cs)
{
    switch (cs) {
    case 601: return "smpte170m";
    case 240: return "smpte240m";
    case 2020: return "bt2020nc";
    default: return "bt709";
    }
}

// Läuft in einem Hilfsthread (Öffnen + erstes Frame dekodieren kann bei 4K dauern): ffmpeg-Aufruf zusammenstellen
ProxyJob prepareJob(const QString& original, const QString& part)
{
    ProxyJob job;
    const SourceInfo info = probeSource(original);
    if (!info.ok || !info.hasVideo) {
        job.error = T("Für diese Datei lässt sich kein Proxy erzeugen (kein Video).");
        return job;
    }
    job.durationUs = info.seconds * 1e6;

    // Längere Kante höchstens kMaxSize (1080p -> 540p, 4K -> Viertel), nie vergrößern; anamorphe Pixel
    // (sar) werden quadratisch gemacht. Gerade Maße für yuv420p.
    const QString k = QString::number(ProxyManager::kMaxSize);
    const QString f = QString("min(1,%1/max(iw*sar,ih))").arg(k);
    // Farbmatrix fest wie MLT sie beim Original annimmt (auch bei Dateien ohne Angabe), damit ffmpeg nicht
    // "korrigiert" und der Proxy in der Vorschau genauso aussieht wie das Original
    const char* cs = ffColorspace(info.colorspace);
    const QString matrix = info.colorspace == 2020 ? "bt2020" : cs;
    const QString scale = QString("scale=w='trunc(%1*iw*sar/2)*2':h='trunc(%1*ih/2)*2'"
                                  ":in_color_matrix=%2:out_color_matrix=%2:in_range=%3:out_range=tv,setsar=1")
                              .arg(f, matrix, info.fullRange ? "pc" : "tv");
    const char* primaries = info.colorspace == 601 ? "smpte170m" : info.colorspace == 2020 ? "bt2020" : "bt709";
    const char* trc = info.colorspace == 601 ? "smpte170m" : "bt709";
    QStringList args{"-hide_banner", "-nostdin", "-y", "-i", original,
                     "-map", "0:v:0", "-map", "0:a:0?", "-sn", "-dn", "-map_metadata", "-1",
                     "-vf", scale,
                     // Kurze GOP ohne B-Frames: Springen/Scrubben in der Vorschau bleibt schnell
                     "-c:v", "libx264", "-preset", "veryfast", "-crf", "23", "-tune", "fastdecode",
                     "-g", "10", "-bf", "0", "-pix_fmt", "yuv420p",
                     "-colorspace", cs, "-color_primaries", primaries, "-color_trc", trc, "-color_range", "tv"};
    // Konstante Framerate gleich der Quelle (Handy-Clips mit variabler Framerate -> CFR)
    if (info.rateNum > 0 && info.rateDen > 0)
        args << "-r" << QString("%1/%2").arg(info.rateNum).arg(info.rateDen);
    args << "-fps_mode" << "cfr"
         << "-c:a" << "aac" << "-b:a" << "192k"
         << "-movflags" << "+faststart" << "-f" << "mp4"
         << "-progress" << "pipe:1" << "-nostats" << part;

    job.args = args;
    return job;
}

} // namespace

ProxyManager::ProxyManager(QObject* parent) : QObject(parent)
{
    m_enabled = QSettings().value(kSettingKey, true).toBool();
}

ProxyManager::~ProxyManager()
{
    m_queue.clear();
    stopCurrent();
    if (m_probeThread) {
        m_probeThread->disconnect(this);
        m_probeThread->wait();
        delete m_probeThread;
    }
}

QString ProxyManager::cacheDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/proxies";
}

QString ProxyManager::proxyPath(const QString& original)
{
    // Schlüssel wie beim Wellenform-Cache: geänderte Originale bekommen automatisch einen neuen Proxy
    const QFileInfo fi(original);
    const QString id = QString("%1|%2|%3|%4")
                           .arg(fi.absoluteFilePath())
                           .arg(fi.size())
                           .arg(fi.lastModified().toMSecsSinceEpoch())
                           .arg(kMaxSize);
    const QString hash = QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha1).toHex();
    return cacheDir() + "/" + hash + ".mp4";
}

bool ProxyManager::hasProxy(const QString& original) const
{
    return !original.isEmpty() && QFileInfo::exists(original) && QFileInfo::exists(proxyPath(original));
}

QString ProxyManager::resolve(const QString& original) const
{
    if (!m_enabled || !hasProxy(original)) return original;
    return proxyPath(original);
}

void ProxyManager::setEnabled(bool on)
{
    if (on == m_enabled) return;
    m_enabled = on;
    QSettings().setValue(kSettingKey, on);
    emit enabledChanged(on);
}

bool ProxyManager::isPending(const QString& original) const
{
    return original == m_current || m_queue.contains(original);
}

int ProxyManager::progressOf(const QString& original) const
{
    if (original == m_current) return m_progress;
    return m_queue.contains(original) ? 0 : -1;
}

void ProxyManager::generate(const QStringList& originals)
{
    for (const QString& p : originals)
        if (!isPending(p) && !hasProxy(p) && QFileInfo::exists(p)) m_queue << p;
    emit queueChanged();
    for (const QString& p : originals) emit progress(p, progressOf(p));
    if (m_current.isEmpty()) startNext();
}

void ProxyManager::remove(const QStringList& originals)
{
    cancel(originals);
    for (const QString& p : originals) {
        if (QFile::remove(proxyPath(p))) emit proxyChanged(p);
    }
}

void ProxyManager::cancel(const QStringList& originals)
{
    bool changed = false;
    for (const QString& p : originals) {
        if (m_queue.removeAll(p) > 0) {
            changed = true;
            emit proxyChanged(p);
        }
    }
    if (originals.contains(m_current)) {
        const QString was = m_current;
        stopCurrent();
        emit proxyChanged(was);
        changed = true;
        startNext();
    }
    if (changed) emit queueChanged();
}

void ProxyManager::cancelAll()
{
    QStringList all = m_queue;
    if (!m_current.isEmpty()) all << m_current;
    cancel(all);
}

void ProxyManager::stopCurrent()
{
    ++m_job; // laufende Untersuchung verfällt
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(3000);
        m_process->deleteLater();
        m_process = nullptr;
    }
    if (!m_currentPart.isEmpty()) QFile::remove(m_currentPart); // halbfertige Datei weg
    m_current.clear();
    m_currentPart.clear();
    m_progress = 0;
}

void ProxyManager::startNext()
{
    if (!m_current.isEmpty()) return;
    if (m_queue.isEmpty()) {
        emit queueChanged();
        return;
    }
    const QString original = m_queue.takeFirst();
    m_current = original;
    m_currentPart = proxyPath(original).chopped(4) + ".part.mp4";
    m_progress = 0;
    m_errorTail.clear();
    emit queueChanged();
    emit progress(original, 0);

    // Quelle im Hilfsthread untersuchen, dann ffmpeg im UI-Thread starten (Abbruch dazwischen: Job verfällt)
    const int job = ++m_job;
    auto result = std::make_shared<ProxyJob>();
    m_probeThread = QThread::create([result, original, part = m_currentPart] { *result = prepareJob(original, part); });
    connect(m_probeThread, &QThread::finished, this, [this, result, original, job, thread = m_probeThread] {
        thread->deleteLater();
        if (m_probeThread == thread) m_probeThread = nullptr;
        if (job == m_job) launch(original, result->args, result->durationUs, result->error);
    });
    m_probeThread->start();
}

void ProxyManager::launch(const QString& original, const QStringList& args, double durationUs, const QString& error)
{
    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (!error.isEmpty() || ffmpeg.isEmpty()) {
        m_current.clear();
        m_currentPart.clear();
        emit failed(original, !error.isEmpty() ? error : T("ffmpeg wurde nicht gefunden. Bitte ffmpeg installieren."));
        if (ffmpeg.isEmpty()) m_queue.clear();
        emit progress(original, -1);
        emit proxyChanged(original);
        startNext();
        return;
    }
    QDir().mkpath(cacheDir());
    m_durationUs = durationUs;
    m_process = new QProcess(this);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &ProxyManager::onOutput);
    connect(m_process, &QProcess::readyReadStandardError, this, [this] {
        m_errorTail = (m_errorTail + QString::fromLocal8Bit(m_process->readAllStandardError())).right(600);
    });
    connect(m_process, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus st) { onFinished(code, st == QProcess::CrashExit); });
    m_process->start(ffmpeg, args);
}

void ProxyManager::onOutput()
{
    // -progress liefert Zeilen "key=value", u. a. out_time_us (Mikrosekunden fertig)
    while (m_process && m_process->canReadLine()) {
        const QByteArray line = m_process->readLine().trimmed();
        if (!line.startsWith("out_time_us=") || m_durationUs <= 0) continue;
        const double us = line.mid(12).toDouble();
        const int pct = std::clamp(int(us * 100 / m_durationUs), 0, 99);
        if (pct != m_progress) {
            m_progress = pct;
            emit progress(m_current, pct);
            emit queueChanged();
        }
    }
}

void ProxyManager::onFinished(int exitCode, bool crashed)
{
    const QString original = m_current;
    const QString part = m_currentPart;
    const QString tail = m_errorTail.trimmed();
    m_process->deleteLater();
    m_process = nullptr;
    m_current.clear();
    m_currentPart.clear();
    m_progress = 0;

    const QString target = proxyPath(original);
    if (!crashed && exitCode == 0 && QFileInfo(part).size() > 0) {
        QFile::remove(target);
        QFile::rename(part, target); // erst jetzt sichtbar -> nie halbfertige Proxies in der Vorschau
    } else {
        QFile::remove(part);
        emit failed(original, T("Proxy konnte nicht erzeugt werden:\n%1").arg(tail.section('\n', -3)));
    }
    emit progress(original, -1);
    emit proxyChanged(original);
    startNext();
}
