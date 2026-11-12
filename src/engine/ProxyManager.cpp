// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/ProxyManager.h"
#include "engine/Bundle.h"

#include "core/I18n.h"
#include "engine/Background.h"
#include "engine/HardwareEncoders.h"
#include "engine/MediaProbe.h"
#include "engine/ProxyCleanup.h"
#include "engine/video/Reader.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
}

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QRandomGenerator>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace {

constexpr const char* kSettingKey = "playback/useProxy";
constexpr const char* kImportKey = "proxy/onImport";
constexpr const char* kQualityKey = "proxy/quality";
// quality() is asked for every proxy lookup (preview, thumbnails): read the setting once. -1 = not read yet
std::atomic<int> g_quality{-1};

// What ffmpeg needs to know about the source; read with the own decoder (engine/video/Reader), which interprets the
// file the way the preview does (same frame rate, same color matrix)
struct SourceInfo {
    bool ok = false;
    bool hasVideo = false;
    int rateNum = 0, rateDen = 0;
    int colorspace = 709; // 601/709/240/2020/2021
    bool fullRange = false;
    bool alpha = false; // transparent pixels (VP8/VP9 with alpha, ProRes 4444, PNG/QuickTime RLE in MOV)
    QString decoder;    // the reader's (libvpx-vp9 for VP9 with alpha)
    double seconds = 0;
    int width = 0, height = 0; // the picture's pixels
    bool interlaced = false;
    QString parity = QStringLiteral("auto"); // interlaced: the field shown first ("tff", "bff"; "auto": not known)
};

struct ProxyJob {
    QString error;
    QStringList args; // ffmpeg arguments
    double durationUs = 0;
};

// Whether a decoded picture really has see-through pixels. Alpha formats are also written fully opaque (ProRes 4444
// from many editors, screen recordings in bgra, palette video): those keep the fast H.264 proxy (VP9 with alpha
// encodes ~2.5x and decodes ~3x slower). Every 8th row is enough for a picture with transparent parts.
bool seeThrough(const AVFrame* f)
{
    const AVPixFmtDescriptor* d = f ? av_pix_fmt_desc_get(AVPixelFormat(f->format)) : nullptr;
    if (!d || !(d->flags & AV_PIX_FMT_FLAG_ALPHA) || f->width <= 0 || f->height <= 0) return false;
    if (d->flags & AV_PIX_FMT_FLAG_PAL) { // palette entries are ARGB words: any used one below 255
        const auto* pal = reinterpret_cast<const uint32_t*>(f->data[1]);
        for (int y = 0; y < f->height; y += 8)
            for (int x = 0; x < f->width; ++x)
                if ((pal[f->data[0][size_t(y) * f->linesize[0] + x]] >> 24) != 0xff) return true;
        return false;
    }
    const int c = d->nb_components - 1; // the alpha component comes last (yuva, rgba/argb/bgra, gbrap, ya)
    const uint32_t opaque = (1u << d->comp[c].depth) - 1;
    std::vector<uint32_t> row(size_t(f->width));
    for (int y = 0; y < f->height; y += 8) {
        av_read_image_line2(row.data(), const_cast<const uint8_t**>(f->data), f->linesize, d, 0, y, c, f->width, 0, 4);
        for (uint32_t a : row)
            if (a < opaque) return true;
    }
    return false;
}

SourceInfo probeSource(const QString& path)
{
    SourceInfo s;
    // Still pictures need no proxy (and files without a moving picture)
    const std::optional<MediaInfo> media = MediaProbe::read(path, FrameRate{25, 1});
    if (media && (media->isImage || !media->hasVideo)) return s;
    Video::Reader::Options options;
    options.hardware = false; // only the stream's description is read
    const std::unique_ptr<Video::Reader> reader = Video::Reader::open(path, options);
    if (!reader) return s;
    const Video::Info& in = reader->info();
    s.ok = true;
    s.hasVideo = true;
    // The highest frame rate with variable frame rate (r_frame_rate as the preview counts it): CFR only duplicates
    s.rateNum = in.frameRate.num;
    s.rateDen = in.frameRate.den;
    s.width = in.width;
    s.height = in.height;
    s.colorspace = in.colorspace; // a guess by picture size already when the file states none
    s.fullRange = in.fullRange;
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(in.format);
    // Alpha only when the first picture shows it (an alpha format alone says little, see seeThrough)
    if (desc && (desc->flags & AV_PIX_FMT_FLAG_ALPHA)) {
        const std::shared_ptr<const Video::Picture> first = reader->picture(0);
        s.alpha = first && seeThrough(first->frame);
    }
    // Interlaced: from the stream's field order, else (not stated) from the first picture
    if (in.fieldOrder != AV_FIELD_UNKNOWN) {
        s.interlaced = in.fieldOrder != AV_FIELD_PROGRESSIVE;
    } else if (const std::shared_ptr<const Video::Picture> first = reader->picture(0)) {
        s.interlaced = !first->progressive;
    }
    // The field shown first: as the decoder flags the first picture, else as the stream states it (see
    // Video::topFieldShownFirst; the reader's pictures carry the same answer)
    if (s.interlaced) {
        const std::shared_ptr<const Video::Picture> first = reader->picture(0);
        const std::optional<bool> top = Video::topFieldShownFirst(first ? first->frame : nullptr, in.fieldOrder);
        s.parity = top ? (*top ? "tff" : "bff") : "auto";
    }
    s.decoder = in.codec;
    if (media) s.seconds = media->length / 25.0;
    else if (in.duration > 0) s.seconds = double(in.duration) / 1e6; // AV_TIME_BASE units
    return s;
}

// ffmpeg names for the color matrix (colorspace numbers 601/709/…) (the proxy gets the matrix of the original as tag,
// otherwise a 540p image without tag is taken for BT.601 and the colors shift)
const char* ffColorspace(int cs)
{
    switch (cs) {
    case 601: return "smpte170m";
    case 240: return "smpte240m";
    case 2020: return "bt2020nc";
    default: return "bt709";
    }
}

// Filters an ffmpeg program lists (`ffmpeg -filters`), cached per file and modification time. Only a listing that
// worked is kept: a failed or timed-out one (a virus scanner holding the program, a slow first start) is tried again
// next time instead of leaving the program without deinterlacing for the process's life.
QStringList listFilters(const QString& program)
{
    static std::mutex mutex;
    static QHash<QString, QStringList> cache;
    const QFileInfo info(program);
    const QString key = info.absoluteFilePath() + '|' + QString::number(info.lastModified().toMSecsSinceEpoch());
    {
        std::lock_guard lock(mutex);
        if (auto it = cache.constFind(key); it != cache.cend()) return *it;
    }
    QProcess p;
    p.start(program, {"-hide_banner", "-nostdin", "-filters"});
    QStringList list;
    if (p.waitForStarted(10000) && p.waitForFinished(20000) && p.exitStatus() == QProcess::NormalExit
        && p.exitCode() == 0) {
        // " TSC bwdif             V->V       Deinterlace the input image."
        for (const QByteArray& line : p.readAllStandardOutput().split('\n')) {
            const QList<QByteArray> words = line.simplified().split(' ');
            if (words.size() >= 3 && words[0].size() == 3) list << QString::fromUtf8(words[1]);
        }
    } else {
        if (p.state() != QProcess::NotRunning) {
            p.kill(); // our own process
            p.waitForFinished(2000);
        }
        return list;
    }
    std::lock_guard lock(mutex);
    cache.insert(key, list);
    return list;
}

// The deinterlacer the ffmpeg program has: bwdif (sharper, fewer jaggies on edges), else yadif; empty = none.
// One picture per frame (the proxy keeps the original's frame rate and frame count)
QString deinterlaceFilter(const QString& program, const QString& parity)
{
    if (!QFileInfo(program).isAbsolute()) return {};
    const QStringList filters = listFilters(program);
    const QString options = QString("=mode=send_frame:parity=%1:deint=all").arg(parity);
    if (filters.contains("bwdif")) return "bwdif" + options;
    if (filters.contains("yadif")) return "yadif" + options;
    return {};
}

// Runs in a helper thread (opening + decoding the first frame can take a while at 4K): assemble the ffmpeg call
ProxyJob prepareJob(const QString& original, const QString& part, int maxSize)
{
    ProxyJob job;
    const SourceInfo info = probeSource(original);
    if (!info.ok || !info.hasVideo) {
        job.error = T("Für diese Datei lässt sich kein Proxy erzeugen (kein Video).");
        return job;
    }
    job.durationUs = info.seconds * 1e6;

    // Longer edge at most maxSize (quality Small: 1080p -> 540p, 4K -> quarter), never enlarge; anamorphic pixels
    // (sar) are made square. Even dimensions for yuv420p.
    const QString k = QString::number(maxSize);
    const QString f = QString("min(1,%1/max(iw*sar,ih))").arg(k);
    // Fix the color matrix as the decoder assumes it for the original (also for files without tag), so that ffmpeg does not
    // "correct" it and the proxy looks exactly like the original in the preview
    const char* cs = ffColorspace(info.colorspace);
    const QString matrix = info.colorspace == 2020 ? "bt2020" : cs;
    QString scale = QString("scale=w='trunc(%1*iw*sar/2)*2':h='trunc(%1*ih/2)*2'"
                                  ":in_color_matrix=%2:out_color_matrix=%2:in_range=%3:out_range=tv,setsar=1")
                              .arg(f, matrix, info.fullRange ? "pc" : "tv");
    // Interlaced (DV, broadcast, camcorders): the two fields deinterlaced before scaling - scaled as one picture the
    // fields of two moments blend into combs and ghosts in the proxy (the original's preview draws field by field)
    if (info.interlaced) {
        if (const QString deint = deinterlaceFilter(Bundle::tool("ffmpeg"), info.parity); !deint.isEmpty())
            scale.prepend(deint + ',');
        else
            qWarning("Proxy of interlaced %s without deinterlacing: ffmpeg has neither bwdif nor yadif",
                     qPrintable(original));
    }
    // Frame 0 of the proxy = frame 0 of the original. The decoder counts from the first video key frame;
    // FFmpeg (without -copyts) shifts by the container start, the earliest of all streams. A recording whose sound
    // starts before the picture (OBS, phones, games: ~1 frame) would put the first picture at +1 frame, and the
    // constant frame rate fills the gap with a doubled first picture: proxy frame k = original frame k-1, cuts
    // one frame off in the preview. Move the picture so the counted start lands on 0.
    if (const std::optional<double> zero = Video::zeroOffset(original); zero && std::abs(*zero) > 1e-4)
        scale.prepend(QString("setpts=PTS-(%1)/TB,").arg(QString::number(*zero, 'f', 6)));
    const char* primaries = info.colorspace == 601 ? "smpte170m" : info.colorspace == 2020 ? "bt2020" : "bt709";
    const char* trc = info.colorspace == 601 ? "smpte170m" : "bt709";
    // A source with alpha keeps it: H.264 has none (the proxy played opaque, covering the tracks below). VP9 with
    // alpha in WebM, which the reader decodes with libvpx - written under the usual <hash>.mp4 name (cleanup, lookup;
    // the reader goes by the content, not the name).
    if (info.alpha && !avcodec_find_encoder_by_name("libvpx-vp9")) {
        job.error = T("Für Video mit Transparenz lässt sich kein Proxy erzeugen (VP9-Encoder fehlt).");
        return job;
    }
    QStringList args{"-hide_banner", "-nostdin", "-y", "-i", original,
                     "-map", "0:v:0", "-map", "0:a:0?", "-sn", "-dn", "-map_metadata", "-1",
                     "-vf", scale,
                     // short GOP without B-frames: jumping/scrubbing in the preview stays fast
                     "-g", "10", "-bf", "0", "-pix_fmt", info.alpha ? "yuva420p" : "yuv420p",
                     "-colorspace", cs, "-color_primaries", primaries, "-color_trc", trc, "-color_range", "tv"};
    // x264, or OpenH264 in the GPL-free build; with alpha VP9 (fast settings, no hidden alt-ref frames: alpha needs
    // them off)
    if (info.alpha)
        args << "-c:v" << "libvpx-vp9" << "-deadline" << "realtime" << "-cpu-used" << "8" << "-row-mt" << "1"
             << "-b:v" << "0" << "-crf" << "30" << "-auto-alt-ref" << "0";
    else {
        // The proxy's size (without the sar correction, close enough for the bit rate)
        const double f = std::min(1.0, double(maxSize) / std::max({info.width, info.height, 1}));
        args << HardwareEncoders::proxyVideoArguments(QSize(int(info.width * f), int(info.height * f)),
                                                      maxSize >= ProxyManager::kFullSize);
    }
    // VP8/VP9 alpha: ffmpeg's own decoders drop it, libvpx reads it (as the reader does)
    if (info.alpha && info.decoder.startsWith("libvpx")) {
        const qsizetype input = args.indexOf("-i");
        args.insert(input, info.decoder);
        args.insert(input, "-c:v");
    }
    // Constant frame rate equal to the source (phone clips with variable frame rate -> CFR)
    if (info.rateNum > 0 && info.rateDen > 0)
        args << "-r" << QString("%1/%2").arg(info.rateNum).arg(info.rateDen);
    // Threads chosen in the settings: decoding (before -i), filters and encoding; otherwise ffmpeg's own choice
    if (Background::chosenThreads() > 0) {
        const QString n = QString::number(Background::threads(1));
        const qsizetype input = args.indexOf("-i");
        args.insert(input, n);
        args.insert(input, "-threads");
        args << "-threads" << n << "-filter_threads" << n;
    }
    args << "-fps_mode" << "cfr";
    if (!info.alpha)
        args << "-c:a" << "aac" << "-b:a" << "192k" << "-movflags" << "+faststart" << "-f" << "mp4";
    else if (avcodec_find_encoder_by_name("libopus"))
        args << "-c:a" << "libopus" << "-b:a" << "160k" << "-f" << "webm";
    else
        args << "-c:a" << "libvorbis" << "-f" << "webm";
    args << "-progress" << "pipe:1" << "-nostats" << part;

    job.args = args;
    return job;
}

} // namespace

QStringList ProxyManager::proxyArguments(const QString& original, int maxSize)
{
    return prepareJob(original, QStringLiteral("-"), maxSize).args;
}

QStringList ProxyManager::programFilters(const QString& program) { return listFilters(program); }

ProxyManager::ProxyManager(QObject* parent) : QObject(parent)
{
    m_enabled = QSettings().value(kSettingKey, true).toBool();
}

ProxyManager::~ProxyManager()
{
    m_queue.clear();
    stopCurrent();
    // Probes of canceled jobs may also still be running (not only m_probeThread)
    for (QThread* t : findChildren<QThread*>(Qt::FindDirectChildrenOnly)) {
        t->disconnect(this);
        t->wait();
    }
}

QString ProxyManager::cacheDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/proxies";
}

QString ProxyManager::decodeSource(const QString& original)
{
    const QString proxy = proxyPath(original);
    return QFileInfo::exists(proxy) ? proxy : original;
}

QString ProxyManager::proxyPath(const QString& original)
{
    return proxyPath(original, maxSize());
}

QStringList ProxyManager::withProxy(const QStringList& originals, int maxSize)
{
    QStringList out;
    for (const QString& p : originals)
        if (!p.isEmpty() && !out.contains(p) && QFileInfo::exists(p) && QFileInfo::exists(proxyPath(p, maxSize))) out << p;
    return out;
}

ProxyManager::Quality ProxyManager::quality()
{
    int q = g_quality.load(std::memory_order_relaxed);
    if (q < 0) {
        const QString v = QSettings().value(kQualityKey).toString();
        q = int(v == "full" ? Quality::Full : v == "large" ? Quality::Large : v == "medium" ? Quality::Medium : Quality::Small);
        g_quality.store(q, std::memory_order_relaxed);
    }
    return Quality(q);
}

void ProxyManager::reloadQuality()
{
    QSettings settings;
    settings.sync(); // picks up what another process wrote
    g_quality.store(-1, std::memory_order_relaxed);
}

int ProxyManager::maxSizeOf(Quality q)
{
    switch (q) {
    case Quality::Medium: return 1280;
    case Quality::Large: return 1920;
    case Quality::Full: return kFullSize;
    case Quality::Small: break;
    }
    return kMaxSize;
}

QString ProxyManager::qualityLabel(Quality q)
{
    switch (q) {
    case Quality::Medium: return "720p";
    case Quality::Large: return "1080p";
    case Quality::Full: return T("volle Auflösung");
    case Quality::Small: break;
    }
    return "540p";
}

void ProxyManager::setQuality(Quality q)
{
    if (q == quality()) return;
    QSettings().setValue(kQualityKey, q == Quality::Full     ? "full"
                                      : q == Quality::Large  ? "large"
                                      : q == Quality::Medium ? "medium"
                                                             : "small");
    g_quality.store(int(q), std::memory_order_relaxed);
    // The proxy being made has the old size: start it again first (the waiting ones are made at the new size)
    if (!m_current.isEmpty()) {
        const QString was = m_current;
        stopCurrent();
        m_queue.prepend(was);
        startNext();
    }
    emit qualityChanged();
}

QString ProxyManager::proxyPath(const QString& original, int maxSize)
{
    // Key as with the waveform cache: changed originals automatically get a new proxy. The trailing version is
    // raised when proxies made before are wrong: 2 = frame 0 at the first video key frame (older proxies of files
    // whose picture starts after the sound were one frame early)
    const QFileInfo fi(original);
    const QString id = QString("%1|%2|%3|%4|v2")
                           .arg(fi.absoluteFilePath())
                           .arg(fi.size())
                           .arg(fi.lastModified().toMSecsSinceEpoch())
                           .arg(maxSize);
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

QStringList ProxyManager::missing(const QStringList& originals) const
{
    QStringList out;
    for (const QString& p : originals)
        if (!isPending(p) && !hasProxy(p) && QFileInfo::exists(p) && !out.contains(p)) out << p;
    return out;
}

ProxyManager::ImportRule ProxyManager::importRule()
{
    const QString v = QSettings().value(kImportKey).toString();
    return v == "always" ? ImportRule::Always : v == "heavy" ? ImportRule::Heavy : ImportRule::Never;
}

void ProxyManager::setImportRule(ImportRule rule)
{
    QSettings().setValue(kImportKey, rule == ImportRule::Always ? "always" : rule == ImportRule::Heavy ? "heavy" : "never");
}

bool ProxyManager::heavyVideo(int width, int height, const QString& codec, int bitDepth)
{
    if (std::max(width, height) >= 3840) return true;
    const QString c = codec.toLower(); // decoder names: hevc, hevc_cuvid, av1, libdav1d, libaom-av1 …
    if (c.contains("hevc") || c.contains("265") || c.contains("av1")) return true;
    return bitDepth > 8;
}

bool ProxyManager::wantedOnImport(ImportRule rule, const QString& path)
{
    if (rule == ImportRule::Never) return false;
    const std::optional<MediaInfo> media = MediaProbe::read(path, FrameRate{25, 1});
    if (!media || media->isImage || !media->hasVideo) return false;
    if (rule == ImportRule::Always) return true;
    Video::Reader::Options options;
    options.hardware = false; // only the stream's description is read
    const std::unique_ptr<Video::Reader> reader = Video::Reader::open(path, options);
    if (!reader) return false;
    const Video::Info& in = reader->info();
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(in.format);
    return heavyVideo(in.width, in.height, in.codec, desc ? desc->comp[0].depth : 8);
}

void ProxyManager::importedFiles(const QStringList& paths, std::function<bool(const QString&)> stillThere)
{
    const ImportRule rule = importRule();
    if (rule == ImportRule::Never || paths.isEmpty()) return;
    auto wanted = std::make_shared<QStringList>();
    QThread* thread = QThread::create([rule, paths, wanted] {
        Background::lowerThisThread(Background::Level::Low);
        for (const QString& p : paths)
            if (wantedOnImport(rule, p)) *wanted << p;
    });
    thread->setParent(this); // the destructor waits for it
    connect(thread, &QThread::finished, this, [this, thread, wanted, stillThere, run = m_importRun] {
        thread->deleteLater();
        if (run != m_importRun) return; // canceled or another project meanwhile
        if (stillThere) wanted->removeIf([&](const QString& p) { return !stillThere(p); });
        if (!wanted->isEmpty()) generate(*wanted);
    });
    thread->start();
}

void ProxyManager::remove(const QStringList& originals)
{
    cancel(originals);
    for (const QString& p : originals) {
        const QString proxy = proxyPath(p);
        QFile::remove(ProxyCleanup::notePath(proxy));
        if (QFile::remove(proxy)) emit proxyChanged(p);
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
    ++m_importRun; // files of an import still being read are not queued afterwards
    QStringList all = m_queue;
    if (!m_current.isEmpty()) all << m_current;
    cancel(all);
}

void ProxyManager::stopCurrent()
{
    ++m_job; // a running probe expires
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(3000);
        m_process->deleteLater();
        m_process = nullptr;
    }
    if (!m_currentPart.isEmpty()) QFile::remove(m_currentPart); // half-finished file gone
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
    // Already there at the current size (the quality was switched back meanwhile): nothing to make
    while (!m_queue.isEmpty() && hasProxy(m_queue.first())) {
        const QString done = m_queue.takeFirst();
        emit progress(done, -1);
        emit proxyChanged(done);
    }
    if (m_queue.isEmpty()) {
        emit queueChanged();
        return;
    }
    const QString original = m_queue.takeFirst();
    m_current = original;
    // A name of its own: another schneidi on the same cache (a second window, the app and a CLI) making the proxy of
    // the same file at once writes its own part file - two ffmpegs in one file would give a broken proxy
    m_currentSize = maxSize();
    m_currentPart = QString("%1.%2.part.mp4")
                        .arg(proxyPath(original, m_currentSize).chopped(4))
                        .arg(QRandomGenerator::global()->generate(), 8, 16, QChar('0'));
    m_progress = 0;
    m_errorTail.clear();
    emit queueChanged();
    emit progress(original, 0);

    // Probe the source in a helper thread, then start ffmpeg in the UI thread (cancel in between: job expires)
    const int job = ++m_job;
    auto result = std::make_shared<ProxyJob>();
    m_probeThread = QThread::create([result, original, part = m_currentPart, size = m_currentSize] {
        *result = prepareJob(original, part, size);
    });
    m_probeThread->setParent(this); // destructor waits for all still running ones
    connect(m_probeThread, &QThread::finished, this, [this, result, original, job, thread = m_probeThread] {
        thread->deleteLater();
        if (m_probeThread == thread) m_probeThread = nullptr;
        if (job == m_job) launch(original, result->args, result->durationUs, result->error);
    });
    m_probeThread->start();
}

void ProxyManager::launch(const QString& original, const QStringList& args, double durationUs, const QString& error)
{
    QString ffmpeg = Bundle::tool("ffmpeg");
    if (!QFileInfo(ffmpeg).isAbsolute()) ffmpeg.clear(); // neither bundled nor in PATH
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
        m_errorTail = (m_errorTail + QString::fromUtf8(m_process->readAllStandardError())).right(600);
    });
    connect(m_process, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus st) { onFinished(code, st == QProcess::CrashExit); });
    // If ffmpeg does not start at all (broken, blocked), no finished() arrives -> otherwise the queue would hang forever
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart || !m_process) return;
        m_errorTail = m_process->errorString();
        onFinished(-1, true);
    });
    Background::lowerProcess(*m_process); // playback takes precedence
    m_process->start(ffmpeg, args);
}

void ProxyManager::onOutput()
{
    // -progress delivers lines "key=value", among others out_time_us (microseconds done)
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

    const QString target = proxyPath(original, m_currentSize); // the size it was made at
    if (!crashed && exitCode == 0 && QFileInfo(part).size() > 0) {
        QFile::remove(target);
        QFile::rename(part, target); // only now visible -> never half-finished proxies in the preview
        ProxyCleanup::writeNote(target, original, m_currentSize); // which file it is for (cleaning up tells why it goes)
    } else {
        QFile::remove(part);
        qWarning("Proxy fehlgeschlagen (%s, Code %d): %s", qPrintable(original), exitCode, qPrintable(tail));
        emit failed(original, T("Proxy konnte nicht erzeugt werden:\n%1").arg(tail.section('\n', -3)));
    }
    emit progress(original, -1);
    emit proxyChanged(original);
    startNext();
}
