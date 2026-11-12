// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/Exporter.h"

#include "core/CodeTrust.h"
#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "core/RenderJob.h"
#include "core/Subtitles.h"
#include "core/SystemMemory.h"
#include "core/TimelineOps.h"
#include "engine/Bundle.h"
#include "engine/ClipFormat.h"
#include "engine/ExportWriter.h"
#include "engine/HardwareEncoders.h"
#include "engine/MemoryRelease.h"
#include "engine/OpenMedia.h"
#include "engine/MediaProbe.h"
#include "engine/SmartRender.h"
#include "engine/Stabilizer.h"
#include "engine/audio/Mixer.h"
#include "engine/picture/Renderer.h"
#include "engine/picture/gpu/Gpu.h"
#include "engine/video/Reader.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace {
std::atomic<int> g_running{0}; // running exports (m_timer runs exactly then)

// Memory model of automaticThreads (see there). Measured on a 1080x1920 short (x264 medium, peak RSS for 1, 2, 4,
// 8 threads): about 65 MB more per thread = 8 RGBA frames of the output size (the frame it renders and its compositing
// buffers, the writer's queue, decoder and encoder frame threads)
constexpr qint64 kMB = 1024 * 1024;
constexpr qint64 kFramesPerThread = 8;
// Left for the program itself, the encoder and the rest of the machine: at least this, or an eighth of what is
// available (the source decoders are counted on their own, decodedSourceBytes)
constexpr qint64 kReserveBytes = 32 * kMB;
// Decoded pictures a video decoder holds (YUV 4:2:0, 1.5 bytes per pixel): the reader's window ahead and behind and
// the decoder's own reference and thread frames. Measured: a second 1080p60 decoder (picture-in-picture of the same
// recording 30 frames apart) added 70 MB = 22 pictures
constexpr qint64 kPicturesPerDecoder = 20;
// ... and per decoder thread of a software decoder (frame threads: each decodes a picture of its own and keeps its
// references). Measured: two 8K H.264 sources rendered to 1080p, 6 -> 10 threads +1.2 GB = 150 MB per thread and
// decoder = 3 pictures. A hardware decoder runs with one thread.
constexpr qint64 kPicturesPerDecoderThread = 3;
// x264's lookahead (shortLookahead): frames at preset medium and bytes per pixel and frame it keeps (measured: ~6 MB
// per 1080x1920 frame; 8K with 40 frames and 2 threads went over 5 GB)
constexpr qint64 kLookaheadFrames = 40;
constexpr qint64 kLookaheadBytesPerPixel = 3;

// The subtitle tracks with entries in the range [from, to) as SRT files (Subtitles::srtFiles: the visible one into path,
// the others next to it), times from the start of the range. Not committed yet: an existing file of a name is replaced
// only when the export worked (false = error set)
bool writeSubtitleFiles(const Timeline& tl, double fps, int from, int to, const QString& path,
                        std::vector<std::unique_ptr<QSaveFile>>& files, QString* error)
{
    const QVector<Subtitles::SrtFile> list = Subtitles::srtFiles(tl, path);
    if (list.isEmpty()) {
        if (error) *error = T("Die Timeline enthält keine Untertitel.");
        return false;
    }
    for (const Subtitles::SrtFile& srt : list) {
        auto f = std::make_unique<QSaveFile>(srt.path);
        if (!f->open(QIODevice::WriteOnly) || f->write(Subtitles::toFile(srt.path, tl.subtitles[srt.track].cues, fps, from, to)) < 0) {
            if (error) *error = T("Untertiteldatei kann nicht geschrieben werden: %1").arg(srt.path);
            return false;
        }
        files.push_back(std::move(f));
    }
    return true;
}

// Temporary name next to a target file, with its extension (the muxer is picked by it): "clip.1a2b3c4d.part.mp4"
// (watch folders take it for a file still being written, core/WatchFolders)
QString partPath(const QString& target, const QString& id)
{
    const QFileInfo fi(target);
    const QString suffix = fi.suffix().isEmpty() ? QString() : "." + fi.suffix();
    return fi.dir().filePath(QString("%1.%2.part%3").arg(fi.completeBaseName(), id, suffix));
}

// Temporary files of renders to `target` that were killed (crash, SIGKILL, power off) stay behind: removed once
// nothing has written them for an hour (a running render writes its part file all the time; another render to the
// same name keeps its own). The same for the x264/x265 extension's intermediates of every render into this folder
// (ExternalEncoder::intermediatePath of a part file: ".<part file>.schneidi-video.nut", hidden, up to several GB; its
// program writes it all the time while encoding).
void removeStaleParts(const QString& target)
{
    const QFileInfo fi(target);
    const QString suffix = fi.suffix().isEmpty() ? QString() : "." + fi.suffix();
    const QRegularExpression part("^" + QRegularExpression::escape(fi.completeBaseName()) + "\\.[0-9a-f]{8}\\.part"
                                  + QRegularExpression::escape(suffix) + "$");
    static const QRegularExpression intermediate("^\\..+\\.[0-9a-f]{8}\\.part(\\.[^.]+)?\\.schneidi-video\\.nut$");
    const QDateTime old = QDateTime::currentDateTime().addSecs(-3600);
    for (const QFileInfo& f : fi.dir().entryInfoList(QDir::Files | QDir::Hidden))
        if ((part.match(f.fileName()).hasMatch() || intermediate.match(f.fileName()).hasMatch()) && f.lastModified() < old)
            QFile::remove(f.absoluteFilePath());
}

// Rename from onto to, replacing an existing file. QFile::rename refuses an existing target: the old file is moved
// aside first and put back when the rename fails, so it is never lost.
bool replaceFile(const QString& from, const QString& to)
{
    if (!QFileInfo::exists(to)) return QFile::rename(from, to);
    const QString aside = partPath(to, QUuid::createUuid().toString(QUuid::Id128).left(8) + "-old");
    if (!QFile::rename(to, aside)) return false;
    if (!QFile::rename(from, to)) {
        QFile::rename(aside, to);
        return false;
    }
    QFile::remove(aside);
    return true;
}

// The sample format the audio encoder gets: the wanted one if the codec takes it, otherwise the codec's first; empty =
// unknown codec
QString encoderSampleFormat(const QString& codecName, const QString& wanted)
{
    const AVCodec* codec = avcodec_find_encoder_by_name(codecName.toUtf8().constData());
    if (!codec) return {};
    const AVSampleFormat* formats = nullptr;
    int count = 0;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void* list = nullptr;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &list, &count) < 0) count = 0;
    formats = static_cast<const AVSampleFormat*>(list);
#else
    formats = codec->sample_fmts;
    while (formats && formats[count] != AV_SAMPLE_FMT_NONE) ++count;
#endif
    const AVSampleFormat want = wanted.isEmpty() ? AV_SAMPLE_FMT_S16 : av_get_sample_fmt(wanted.toUtf8().constData());
    for (int i = 0; i < count; ++i)
        if (formats[i] == want) return QString::fromLatin1(av_get_sample_fmt_name(want));
    for (int i = 0; i < count; ++i)
        switch (formats[i]) {
        case AV_SAMPLE_FMT_U8: case AV_SAMPLE_FMT_S16: case AV_SAMPLE_FMT_S32: case AV_SAMPLE_FMT_FLT:
        case AV_SAMPLE_FMT_U8P: case AV_SAMPLE_FMT_S16P: case AV_SAMPLE_FMT_S32P: case AV_SAMPLE_FMT_FLTP:
            return QString::fromLatin1(av_get_sample_fmt_name(formats[i]));
        default: break;
        }
    return {};
}

// Bits the file keeps of an integer sample format (the writer rounds to them); 0 = float or the full format
int fileBits(const QString& codec, const QString& sampleFormat)
{
    if (codec.startsWith("pcm_s16") || sampleFormat.startsWith("s16")) return 16;
    if (codec.startsWith("pcm_s24")) return 24;
    if (codec == "alac" || codec == "flac") return sampleFormat.startsWith("s32") ? 24 : 16; // s32 = 24 bit there
    if (sampleFormat.startsWith("s32")) return 32;
    if (sampleFormat.startsWith("u8")) return 8;
    return 0;
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

QString Exporter::overwrittenSource(const Timeline& tl, const QString& path, const QString& subtitlePath)
{
    QStringList targets{path};
    if (!subtitlePath.isEmpty())
        for (const Subtitles::SrtFile& srt : Subtitles::srtFiles(tl, subtitlePath)) targets << srt.path;
    for (const QString& target : std::as_const(targets))
        if (!target.isEmpty() && readsFile(tl, target)) return target;
    return {};
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

Exporter::ThreadChoice Exporter::automaticThreads(int processors, QSize output, qint64 availableBytes,
                                                  qint64 sourceBytes, qint64 sourcePerThread)
{
    processors = std::max(1, processors);
    if (availableBytes <= 0 || output.isEmpty()) return {processors, false};
    const qint64 perThread =
        qint64(output.width()) * output.height() * 4 * kFramesPerThread + std::max<qint64>(0, sourcePerThread);
    const qint64 budget = availableBytes - std::max(kReserveBytes, availableBytes / 8) - std::max<qint64>(0, sourceBytes);
    const int fit = int(std::clamp<qint64>(budget / std::max<qint64>(1, perThread), 1, processors));
    return {fit, fit < processors};
}

bool Exporter::shortLookahead(QSize output, qint64 availableBytes)
{
    if (availableBytes <= 0 || output.isEmpty()) return false;
    const qint64 lookahead = qint64(output.width()) * output.height() * kLookaheadBytesPerPixel * kLookaheadFrames;
    return lookahead > availableBytes / 3;
}

bool Exporter::lowMemoryLookahead(int processors, const ThreadChoice& automatic)
{
    return automatic.lowMemory && (automatic.threads * 2 < processors || automatic.threads <= 2);
}

qint64 Exporter::decodedSourceBytes(const Timeline& tl, int from, int to,
                                   const std::function<QSize(const QString&)>& sizeOf)
{
    return peakDecoderBytes(tl, from, to, sizeOf, kPicturesPerDecoder);
}

qint64 Exporter::sourceBytesPerThread(const Timeline& tl, int from, int to,
                                     const std::function<QSize(const QString&)>& softwareSizeOf)
{
    return peakDecoderBytes(tl, from, to, softwareSizeOf, kPicturesPerDecoderThread);
}

qint64 Exporter::peakDecoderBytes(const Timeline& tl, int from, int to,
                                 const std::function<QSize(const QString&)>& sizeOf, qint64 pictures)
{
    // One decoder per file and position (source frame - timeline frame), as the renderer shares them
    // (Picture::Renderer's lanes); compound clips' contents are not looked into
    struct Use {
        int start, end;
        QString key;
        qint64 bytes;
    };
    QHash<QString, QSize> sizes;
    std::vector<Use> uses;
    for (const Track& t : tl.video) {
        if (t.hidden) continue;
        for (const Clip& c : t.clips) {
            if (!c.enabled || c.mediaPath.isEmpty() || c.isNested() || c.isTitle()) continue;
            const int a = std::max(c.start, from), b = std::min(c.end(), to);
            if (a >= b) continue;
            auto it = sizes.find(c.mediaPath);
            if (it == sizes.end()) it = sizes.insert(c.mediaPath, sizeOf(c.mediaPath));
            if (it->isEmpty()) continue;
            const qint64 bytes = qint64(it->width()) * it->height() * 3 / 2 * pictures;
            uses.push_back({a, b, c.mediaPath + '|' + QString::number(c.in - c.start), bytes});
        }
        // A dissolve shows both sides over its whole span, each from a decoder of its own - also the same file
        // going on across the cut (Picture::Renderer gives the right side a second decoder then). Handles taken as
        // long enough: the span may come out a little longer than rendered, never shorter
        const QVector<TimelineOps::TransitionSpan> spans =
            TimelineOps::transitions(t, [](const Clip&) { return std::numeric_limits<int>::max() / 4; });
        for (const TimelineOps::TransitionSpan& sp : spans) {
            if (!sp.isDissolve()) continue;
            const int a = std::max(sp.start, from), b = std::min(sp.end, to);
            if (a >= b) continue;
            QString leftKey;
            for (const int id : {sp.leftId, sp.rightId})
                for (const Clip& c : t.clips) {
                    if (c.id != id || !c.enabled || c.mediaPath.isEmpty() || c.isNested() || c.isTitle()) continue;
                    const QSize size = sizes.value(c.mediaPath, sizeOf(c.mediaPath));
                    if (size.isEmpty()) continue;
                    QString key = c.mediaPath + '|' + QString::number(c.in - c.start);
                    if (id == sp.leftId) leftKey = key;
                    else if (key == leftKey) key += 'x';
                    uses.push_back({a, b, key, qint64(size.width()) * size.height() * 3 / 2 * pictures});
                }
        }
    }
    // The frames where something starts are where the sum can grow
    qint64 peak = 0;
    for (const Use& at : uses) {
        QSet<QString> open;
        qint64 sum = 0;
        for (const Use& u : uses)
            if (u.start <= at.start && at.start < u.end && !open.contains(u.key)) {
                open.insert(u.key);
                sum += u.bytes;
            }
        peak = std::max(peak, sum);
    }
    return peak;
}

int Exporter::parallelFrames(const Timeline& tl, int threads)
{
    if (Stabilizer::needsFrameOrder(tl)) return 1; // border "keep": each frame takes over parts of the one before
    return threads > 0 ? threads : availableThreads();
}

Exporter::Exporter(QObject* parent) : QObject(parent)
{
    m_timer.setInterval(200);
    connect(&m_timer, &QTimer::timeout, this, &Exporter::poll);
}

QString Exporter::gifIntermediate(const QString& partId)
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(QString("schneidi-gif-%1.mov").arg(partId));
}

Exporter::~Exporter()
{
    if (m_build) stopBuild();
    cancel();
    discardOutput(); // e.g. stopped while building the graph
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
    if (const QString target = overwrittenSource(tl, s.path, s.subtitlePath); !target.isEmpty()) {
        if (error) *error = T("%1 wird in der Timeline verwendet und kann nicht überschrieben werden.")
                                .arg(QFileInfo(target).fileName());
        return false;
    }
    // Stabilization needs its analysis pass: run what is missing first, never export unstabilized silently
    if (!std::exchange(m_analysisDone, false) && runAnalyses(tl, s)) return true;

    // Output size: the one asked for or the timeline's, always even (4:2:0 encoders cannot open odd sizes, and a
    // project format from an import or a project file may be odd). Different from the timeline: scale pixel values
    // (position, titles …) along
    ProjectFormat out = s.format;
    const QSize size = s.size.isEmpty() ? s.format.size() : s.size;
    out.width = size.width() & ~1;
    out.height = size.height() & ~1;
    Timeline scaled = tl;
    if (out.size() != s.format.size()) scaleTimeline(scaled, s.format.size(), out.size());
    // Audio only: video tracks carry no sound, so skip decoding their images
    if (s.videoCodec.isEmpty()) scaled.video.clear();
    // An encoder this FFmpeg does not have (the GPL-free build has no x264/x265 and offers what it has instead):
    // say so instead of failing without a reason
    if (!s.videoCodec.isEmpty() && !avcodec_find_encoder_by_name(s.videoCodec.toUtf8().constData())
        && HardwareEncoders::externalProgram(s.videoCodec).isEmpty() && !ExportWriter::isOwnH264(s.videoCodec)) {
        if (error) *error = T("Video-Encoder %1 ist in diesem Build nicht verfügbar.").arg(s.videoCodec);
        return false;
    }
    // The mixer gets the timeline as it is (it handles mute/solo itself), the renderer no sound
    const bool sound = !s.audioCodec.isEmpty();
    QString sampleFormat;
    if (sound) {
        sampleFormat = encoderSampleFormat(s.audioCodec, s.audioSampleFormat);
        if (sampleFormat.isEmpty()) {
            if (error) *error = T("Audio-Encoder %1 nicht verfügbar.").arg(s.audioCodec);
            return false;
        }
    }
    scaled.audio.clear(); // the picture only
    scaled.masterVolumeDb = 0;
    scaled.masterLimiter = false;
    // Hidden video tracks add nothing to the file: leave their clips out, so their media is neither opened nor
    // decoded while rendering. The length stays (end is computed above)
    for (Track& t : scaled.video)
        if (t.hidden) t.clips.clear();
    // Custom code untrusted now stays unrun for the whole file, even when the user trusts it meanwhile (the list of
    // skipped effects below says so)
    CodeTrust::freeze(scaled);

    QDir().mkpath(QFileInfo(s.path).absolutePath());
    std::vector<std::unique_ptr<QSaveFile>> subtitles;
    if (!s.subtitlePath.isEmpty()
        && !writeSubtitleFiles(tl, s.format.rate.fps(), m_from, m_from + m_length, s.subtitlePath, subtitles, error))
        return false;
    // Different frame rate: the writer picks the timeline frame of every output frame
    const FrameRate rate = s.rate.num > 0 && s.rate.den > 0 ? s.rate : s.format.rate;
    const bool convertRate = rate != s.format.rate && !s.videoCodec.isEmpty();
    const qint64 available = s.videoCodec.isEmpty() ? 0 : SystemMemory::read().available;
    ThreadChoice automatic{availableThreads(), false};
    if (s.threads <= 0 && available > 0) {
        // Each file once: its picture size and whether it would decode in software (pictures per decoder thread)
        struct Source {
            QSize size;
            bool software = true;
        };
        QHash<QString, Source> known;
        auto source = [&known](const QString& path) -> const Source& {
            auto it = known.find(path);
            if (it != known.end()) return *it;
            Source src;
            // The container's stream description only: no decoder opened, nothing decoded, no graphics card
            // device (start() runs on the GUI thread; opening every file there blocked it 115-140 ms for 24 clips)
            AVFormatContext* fmt = nullptr;
            if (!MediaProbe::isAudioFile(path) && OpenMedia::input(&fmt, path.toUtf8().constData()) >= 0) {
                const char* demuxer = fmt->iformat ? fmt->iformat->name : "";
                const bool picture = std::strstr(demuxer, "image2") || std::strstr(demuxer, "_pipe");
                const int v = picture ? -1 : av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
                // MPEG-TS/M2TS (a camera's or recorder's .ts/.m2ts) name no picture size before their stream analysis:
                // a short one then (the first 1 MB, half a second), else counted as 1080p - never as nothing
                if (v >= 0 && fmt->streams[v]->codecpar->width <= 0
                    && !(fmt->streams[v]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
                    fmt->probesize = 1 << 20;
                    fmt->max_analyze_duration = AV_TIME_BASE / 2;
                    avformat_find_stream_info(fmt, nullptr);
                }
                if (v >= 0) {
                    const AVCodecParameters* par = fmt->streams[v]->codecpar;
                    const bool cover = fmt->streams[v]->disposition & AV_DISPOSITION_ATTACHED_PIC;
                    if (!cover) {
                        const bool known = par->width > 0 && par->height > 0;
                        src.size = known ? QSize(par->width, par->height) : QSize(1920, 1080);
                        src.software = !known || !Video::likelyHardware(par);
                    }
                }
                avformat_close_input(&fmt);
            }
            return *known.insert(path, src);
        };
        const qint64 sources = decodedSourceBytes(scaled, m_from, m_from + m_length,
                                                  [&](const QString& path) { return source(path).size; });
        const qint64 perThread = sourceBytesPerThread(scaled, m_from, m_from + m_length, [&](const QString& path) {
            const Source& src = source(path);
            return src.software ? src.size : QSize();
        });
        automatic = automaticThreads(availableThreads(), out.size(), available, sources, perThread);
    }
    const int threads = s.threads > 0 ? s.threads : automatic.threads;
    m_threadsLowMemory = s.threads <= 0 && automatic.lowMemory;
    m_shortLookahead = (s.threads <= 0 && lowMemoryLookahead(availableThreads(), automatic))
                       || shortLookahead(out.size(), available);
    m_settings = s;
    m_convertRate = convertRate;
    m_sampleFormat = sampleFormat;
    m_threads = threads;
    m_frames = parallelFrames(scaled, threads); // effects on left-out tracks do not count
    m_target = s.path;
    m_subtitles = std::move(subtitles);
    m_textTracks.clear();
    if (s.embedSubtitles && s.output == ExportSettings::Output::File && !s.videoCodec.isEmpty())
        m_textTracks = Subtitles::textTracks(tl, s.format.rate.fps(), m_from, m_from + m_length);
    // Written under a temporary name next to the target, renamed onto it at the end (see m_part)
    static const QRegularExpression ownId("^[0-9a-f]{8}$");
    const QString partId = ownId.match(s.partId).hasMatch() ? s.partId : QUuid::createUuid().toString(QUuid::Id128).left(8);
    switch (s.output) {
    case ExportSettings::Output::File: m_path = m_part = partPath(s.path, partId); break;
    case ExportSettings::Output::ImageSequence:
        m_part = imageSequenceDir(s.path) + "." + partId + ".part";
        QDir().mkpath(m_part);
        m_path = QDir(m_part).filePath(QFileInfo(imageSequencePattern(s.path)).fileName());
        break;
    case ExportSettings::Output::Gif:
        m_path = gifIntermediate(partId);
        m_part = partPath(s.path, partId);
        break;
    }
    m_note = s.encoderNote;
    m_usedCodec.clear();
    m_copiedPercent = 0;
    // Effects this engine does not have (frei0r/LADSPA plugins, ids of newer versions) stay in the project but are
    // skipped: said in the log and to the caller (skippedEffects())
    m_skipped.clear();
    for (const Picture::SkippedEffect& e : Picture::skippedEffects(tl)) {
        if (e.video ? s.videoCodec.isEmpty() : s.audioCodec.isEmpty()) continue; // no picture/sound written anyway
        m_skipped << e;
        qWarning("Export: effect %s of clip %d is %s and is skipped", qPrintable(e.effect), e.clip,
                 e.reason == QLatin1String("untrusted") ? "custom code nobody trusted here" : "not available here");
    }
    if (QString why; !writable(s, sampleFormat, &why)) {
        if (error) *error = T("Export konnte nicht gestartet werden.") + " (" + why + ")";
        discardOutput();
        return false;
    }
    m_outSize = out.size();
    m_outRate = rate;
    // Smart render: only where the timeline is written as it is (size, frame rate)
    m_copy.clear();
    const bool sameSize = out.size() == s.format.size();
    // (not with x264/x265 of the extension: its program encodes the whole video in one go)
    if (s.smartRender && !s.overlay && SmartRender::enabled() && s.output == ExportSettings::Output::File && !s.videoCodec.isEmpty()
        && !convertRate && sameSize && HardwareEncoders::externalProgram(s.videoCodec).isEmpty()
        && !ExportWriter::isOwnH264(s.videoCodec)) // (the own encoder has no pieces to put between copies)
        m_copy = SmartRender::spans(tl, m_from, m_length, s.burnSubtitles);
    const bool smart = !m_copy.isEmpty();
    m_keyframes = ExportWriter::isOwnH264(s.videoCodec) ? cutFrames(tl, m_from, m_length, s.format.rate, rate) : QVector<int>();

    // Renderer and mixer are made in a thread (smart render looks at the files to copy), so the window stays
    // responsive. launch() continues in this thread. Sources open with their first frame.
    auto graph = std::make_shared<Graph>();
    // The GPU's surface on this thread (the renderer is made in the build thread), also while the app still defers it
    Picture::Gpu::Service::prepare(true);
    const int from = m_from, length = m_length, frames = m_frames;
    m_buildCancel = std::make_shared<std::atomic<bool>>(false);
    std::optional<Timeline> mixed;
    if (sound) mixed = tl;
    // A cancel (stopBuild) waits for this thread: it stops between its steps and between the files it opens. What it
    // made then is never used (its finished() is stale).
    m_build = QThread::create([graph, cancel = m_buildCancel, scaled = std::move(scaled), mixed = std::move(mixed), out, s,
                               threads, from, length, frames, rate, convertRate, smart, copy = m_copy] {
        Graph& g = *graph;
        {
            if (smart)
                if (const int cs = SmartRender::colorspace(copy, s.videoCodec, out.size(), cancel.get()); cs > 0)
                    g.colorspace = cs;
            if (*cancel) return;
            if (!s.videoCodec.isEmpty()) {
                Picture::Options po;
                po.format = out;
                po.mode = Picture::Mode::Export;
                po.rangeStart = from;
                po.rangeEnd = from + length - 1;
                po.subtitles = s.burnSubtitles;
                po.decoderThreads = threads;
                po.parallelFrames = frames;
                po.overlay = s.overlay;
                g.renderer = std::make_shared<Picture::Renderer>(scaled, po);
            }
            if (*cancel) return;
            if (mixed) {
                Audio::Mixer::Options o;
                o.fileLength = [lengths = s.mediaLengths](const QString& path) { return lengths.value(path); };
                o.rate = s.audioRate > 0 ? s.audioRate : 48000;
                g.mixer = std::make_shared<Audio::Mixer>(*mixed, s.format.rate, o);
            }
            g.length = convertRate ? std::max(1, int(std::lround(length * rate.fps() / s.format.rate.fps()))) : length;
        }
    });
    const quint64 run = ++m_threadRun;
    connect(m_build, &QThread::finished, this, [this, graph, run] {
        if (run != m_threadRun) return; // canceled as it ended: m_build is gone (stopBuild), maybe another one runs
        m_build->wait();
        delete m_build;
        m_build = nullptr;
        --g_running;
        if (!graph->error.isEmpty()) {
            fail(graph->error);
            return;
        }
        m_renderer = std::move(graph->renderer);
        m_mixer = std::move(graph->mixer);
        m_colorspace = graph->colorspace;
        m_length = graph->length;
        QString error;
        if (!launch(&error)) {
            fail(error);
            return;
        }
        emit started();
    });
    ++g_running; // the render cache waits already
    m_build->start();
    emit progress(0);
    return true;
}

int Exporter::framesWritten() const
{
    return m_writer ? m_writer->framesWritten() : 0;
}

bool Exporter::launch(QString* error)
{
    const ExportSettings& s = m_settings;
    // The writer opens the encoder itself and reports when it cannot (fallback in pollWriter). A hardware encoder that
    // already failed in this session (could not be opened): software right away
    if (HardwareEncoders::isHardwareCodec(s.videoCodec) && HardwareEncoders::cachedResult(s.videoCodec, outputSize()) < 0) {
        if (!fallBackToSoftware(error)) return false;
        ++g_running;
        m_timer.start();
        return true;
    }
    if (!startWriter(s.videoCodec, error)) return false;
    ++g_running;
    m_timer.start();
    return true;
}

QVector<int> Exporter::cutFrames(const Timeline& tl, int from, int length, FrameRate timelineRate, FrameRate outputRate)
{
    // The lowest visible video track with pictures inside the range is the edit's main picture: its hard cuts (no
    // transition at the clip's start) to another source are where the picture changes as a whole. Titles and overlays
    // above change only a part. Not forced:
    // - a cut within the same file right after the clip before it (a split, a talking head's jump cut, the same
    //   recording a bit later): the picture hardly changes there, a key frame would only cost bits; a real change
    //   is still found by the encoder's scene detection
    // - a cut closer than kOwnMinKeyDistance output frames to the last forced key frame (or the first picture): a
    //   montage of 2-3 frame clips must not become a row of key frames
    QVector<int> cuts;
    const int end = from + length;
    for (const Track& t : tl.video) {
        if (t.hidden) continue;
        QVector<const Clip*> clips;
        for (const Clip& c : t.clips)
            if (c.enabled && c.kind != ClipKind::Title && c.start < end && c.end() > from) clips << &c;
        if (clips.isEmpty()) continue;
        std::sort(clips.begin(), clips.end(), [](const Clip* a, const Clip* b) { return a->start < b->start; });
        int lastKey = 0;
        for (qsizetype i = 0; i < clips.size(); ++i) {
            const Clip& c = *clips[i];
            if (c.transIn > 0 || c.start <= from) continue;
            const Clip* before = i > 0 ? clips[i - 1] : nullptr;
            if (before && before->end() == c.start && !c.mediaPath.isEmpty() && before->mediaPath == c.mediaPath
                && before->kind == c.kind)
                continue;
            // First output frame showing the clip: output frame k shows timeline frame from + floor(k * tl / out)
            const int64_t a = int64_t(c.start - from) * outputRate.num * timelineRate.den;
            const int64_t b = int64_t(outputRate.den) * timelineRate.num;
            const int k = int(b > 0 ? (a + b - 1) / b : 0);
            if (k - lastKey < kOwnMinKeyDistance) continue;
            cuts << k;
            lastKey = k;
        }
        break;
    }
    return cuts;
}

QString Exporter::fallbackCodec(const QString& failed) const
{
    // A hardware encoder: the software one chosen with it; x264/x265 of the extension (a separate program): the free one
    if (HardwareEncoders::isHardwareCodec(failed)) return m_settings.softwareCodec;
    if (!HardwareEncoders::externalProgram(failed).isEmpty()) return HardwareEncoders::freeReplacement(failed);
    return {};
}

bool Exporter::fallBackToSoftware(QString* error)
{
    const QString failed = m_usedCodec.isEmpty() ? m_settings.videoCodec : m_usedCodec;
    const QString next = fallbackCodec(failed);
    if (next.isEmpty() || next == failed) {
        if (error) *error = T("Hardware-Encoder %1 funktioniert nicht.").arg(failed);
        return false;
    }
    qWarning("Export: encoder %s failed, rendering with %s", qPrintable(failed), qPrintable(next));
    m_note = HardwareEncoders::isHardwareCodec(failed)
                 ? T("Hardware-Encoder %1 funktioniert nicht, mit Software gerendert.")
                       .arg(videoEncoderLabel(HardwareEncoders::encoderOfCodec(failed)))
                 : T("Die Erweiterung x264/x265 funktioniert nicht, mit %1 gerendert.").arg(next);
    return startWriter(next, error);
}

QVector<QPair<QByteArray, QByteArray>> Exporter::lowMemoryOptions(const QString& codec)
{
    // x264's lookahead holds about 6 MB per frame at 1080x1920, 40 frames at preset medium = more than a 415 MB
    // board has free: 10 like preset veryfast. (sync-lookahead 0 would save 37 MB more at 2 threads, but runs the
    // lookahead in the encoder's thread: on the Pi Zero 2 W the CS2 short rendered at half the speed.) x265:
    // lookahead 10 too (20-25 at its presets)
    if (codec == "libx264") return {{QByteArrayLiteral("rc-lookahead"), QByteArrayLiteral("10")}};
    if (codec == "libx265") return {{QByteArrayLiteral("x265-params"), QByteArrayLiteral("rc-lookahead=10")}};
    // The own H.264 encoder keeps whole pictures in its lookahead (40 by default): 10 as well
    if (ExportWriter::isOwnH264(codec)) return {{QByteArrayLiteral("lookahead"), QByteArrayLiteral("10")}};
    return {};
}

QVector<QPair<QByteArray, QByteArray>> Exporter::encoderOptions(const QString& codec) const
{
    const ExportSettings& s = m_settings;
    QVector<QPair<QByteArray, QByteArray>> o;
    if (s.videoCodec.isEmpty()) return o;
    if (ExportWriter::isOwnH264(codec)) {
        // Own encoder: a fixed QP from the quality level; "Web" adds the cap as "bitrate" (the writer keeps the QP when
        // the clip stays below the cap with it, else it encodes in two passes towards the cap, see ExportWriter)
        o.append({"qp", QByteArray::number(ExportWriter::ownH264Qp(s.crf > 0 ? s.crf : 20))});
        if (s.maxBitrateK > 0) o.append({"bitrate", QByteArray::number(s.maxBitrateK)});
        if (m_shortLookahead) o += lowMemoryOptions(codec); // (8K, low memory)
    } else if (HardwareEncoders::isHardwareCodec(codec)) {
        for (const auto& [key, value] : HardwareEncoders::codecOptions(codec, s.crf)) o.append({key.toUtf8(), value.toUtf8()});
    } else {
        const auto own = HardwareEncoders::softwareOptions(codec, s.crf, outputSize(), outputFps());
        if (!own.isEmpty()) { // OpenH264, kvazaar: no crf of their own
            for (const auto& [key, value] : own) o.append({key.toUtf8(), value.toUtf8()});
        } else if (s.crf > 0) {
            o.append({"crf", QByteArray::number(s.crf)}); // H.264/H.265/AV1/VP9; fixed-quality formats have 0
        }
        if (codec == "libx264" || codec == "libx265") o.append({"preset", s.preset.toUtf8()});
        // Threads lowered a lot for the RAM (lowMemoryLookahead), or the lookahead alone would not fit
        // (shortLookahead, also with threads asked for: 8K, 40 frames = 4 GB): shorter lookahead
        if (m_shortLookahead) o += lowMemoryOptions(codec);
    }
    if (s.maxBitrateK > 0 && !ExportWriter::isOwnH264(codec)) { // quality target as before, but capped (VBV)
        o.append({"maxrate", QByteArray::number(s.maxBitrateK) + "k"});
        o.append({"bufsize", QByteArray::number(s.maxBitrateK * 2) + "k"});
    }
    for (const QString& option : s.codecOptions) { // "key=value" from the format (profile, speed …)
        const qsizetype eq = option.indexOf('=');
        if (eq > 0) o.append({option.left(eq).toUtf8(), option.mid(eq + 1).toUtf8()});
    }
    return o;
}

QString Exporter::audioSampleFormat() const
{
    return m_sampleFormat;
}

int Exporter::audioBitrateK() const
{
    const QString& c = m_settings.audioCodec;
    if (c.isEmpty() || c.startsWith("pcm_") || c == "alac" || c == "flac") return 0;
    return c == "libmp3lame" ? std::min(m_settings.audioBitrateK, 320) : m_settings.audioBitrateK;
}

bool Exporter::startWriter(const QString& codec, QString* error)
{
    const ExportSettings& s = m_settings;
    m_writer.reset(); // again (fallback): the old one has stopped
    ExportWriter::Settings w;
    w.path = m_path;
    w.videoCodec = s.videoCodec.isEmpty() ? QString() : codec;
    w.pixFmt = HardwareEncoders::isHardwareCodec(codec) ? HardwareEncoders::pixelFormat(codec) : s.pixFmt;
    w.videoOptions = encoderOptions(codec);
    w.audioCodec = s.audioCodec;
    if (!s.audioCodec.isEmpty()) {
        w.sampleFormat = audioSampleFormat();
        w.audioBitrateK = audioBitrateK();
        if (s.audioRate > 0) w.audioRate = s.audioRate;
        if (s.audioChannels > 0) w.audioChannels = s.audioChannels;
    }
    const QString ext = QFileInfo(m_path).suffix().toLower();
    w.fastStart = ext == "mp4" || ext == "mov" || ext == "m4a";
    w.threads = m_threads;
    w.length = m_length;
    w.copy = m_copy;
    w.maxBitrateK = s.maxBitrateK;
    w.keyframes = m_keyframes;
    w.subtitles = m_textTracks;
    if (!s.videoCodec.isEmpty()) w.externalProgram = HardwareEncoders::externalProgram(codec);
    m_usedCodec = s.videoCodec.isEmpty() ? QString() : codec;
    {
        if (s.output == ExportSettings::Output::ImageSequence) w.muxer = "image2";
        if (codec.endsWith("_vaapi")) w.hwDevice = HardwareEncoders::vaapiDevice();
        ExportWriter::Own o;
        o.renderer = m_renderer;
        o.mixer = m_mixer;
        o.size = m_outSize;
        o.rate = m_outRate;
        o.timelineRate = s.format.rate;
        o.colorspace = m_colorspace;
        o.from = m_from;
        o.audioBits = fileBits(s.audioCodec, w.sampleFormat);
        o.parallel = m_frames;
        qInfo("Export: %s%s, %d threads, %d frames at once%s",
              s.videoCodec.isEmpty() ? "audio only" : qPrintable(codec),
              w.externalProgram.isEmpty() ? "" : " (x264/x265 extension)", m_threads, m_frames,
              m_convertRate ? ", frame rate converted" : "");
        m_writer = std::make_unique<ExportWriter>(w, std::move(o));
    }
    m_writer->start();
    if (m_writer->state() == ExportWriter::State::Failed) {
        qWarning("Export: %s", qPrintable(m_writer->error()));
        // A hardware encoder that cannot be opened may fail this early (a fast machine) or later (pollWriter):
        // the same fallback to software both times
        if (!fallbackCodec(codec).isEmpty() && m_writer->encoderFailed()) {
            m_writer.reset();
            if (HardwareEncoders::isHardwareCodec(codec)) HardwareEncoders::remember(codec, outputSize(), false); // not again
            QFile::remove(m_path);
            return fallBackToSoftware(error);
        }
        if (error) *error = T("Export konnte nicht gestartet werden.") + " (" + m_writer->error() + ")";
        m_writer.reset();
        return false;
    }
    return true;
}

void Exporter::pollWriter()
{
    switch (m_writer->state()) {
    case ExportWriter::State::Running: {
        // GIF: the conversion afterwards gets the last tenth
        const int scale = m_settings.output == ExportSettings::Output::Gif ? 90 : 100;
        emit progress(std::clamp(int(m_writer->progress() * scale), 0, scale - 1));
        return;
    }
    case ExportWriter::State::Failed: {
        const QString message = m_writer->error();
        qWarning("Export: %s", qPrintable(message));
        if (m_writer->smartFailed()) {
            QString error;
            if (smartFallback(&error)) return;
            fail(error);
            return;
        }
        // Hardware encoder (or the x264/x265 extension) could not be opened or failed while encoding: render again with
        // software
        if (!fallbackCodec(m_usedCodec).isEmpty() && m_writer->encoderFailed()) {
            m_writer.reset();
            if (HardwareEncoders::isHardwareCodec(m_usedCodec))
                HardwareEncoders::remember(m_usedCodec, outputSize(), false); // not again this session
            QFile::remove(m_path);
            QString error;
            if (fallBackToSoftware(&error)) return;
            fail(error);
            return;
        }
        fail(T("Export fehlgeschlagen: %1").arg(m_target) + " (" + message + ")");
        return;
    }
    case ExportWriter::State::Done: {
        if (const int copied = m_writer->framesCopied(); copied > 0) {
            m_copiedPercent = std::clamp(int(std::lround(copied * 100.0 / std::max(1, m_length))), 1, 100);
            qInfo("Export: smart render copied %d of %d frames", copied, m_length);
            const QString smart = T("Smart Render: %1 % kopiert, %2 % kodiert").arg(m_copiedPercent).arg(100 - m_copiedPercent);
            m_note = m_note.isEmpty() ? smart : m_note + " · " + smart;
            if (copied >= m_length) m_usedCodec = "copy"; // the encoder wrote no frame
        }
        if (!wroteOutput()) {
            fail(T("Export fehlgeschlagen: %1").arg(m_target));
            return;
        }
        cleanup();
        if (m_settings.output == ExportSettings::Output::Gif) {
            convertToGif();
            return;
        }
        succeed();
        return;
    }
    }
}

bool Exporter::smartFallback(QString* error)
{
    qWarning("Export: smart render failed, encoding everything");
    m_writer.reset();
    QFile::remove(m_path);
    m_copy.clear();
    return startWriter(m_usedCodec, error); // the writer takes the sound from the mixer either way
}

void Exporter::poll()
{
    if (m_writer) pollWriter();
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
                  "-loop", "0", "-f", "gif", "-y", m_part});
    emit progress(90);
}

void Exporter::finishGif(bool ok, const QString& log)
{
    if (!m_gif) return;
    m_gif->disconnect(this);
    m_gif.release()->deleteLater(); // inside one of its signals
    --g_running;
    if (!ok) {
        if (!log.isEmpty()) qWarning("Export: GIF conversion failed: %s", qPrintable(log));
        fail(T("Export fehlgeschlagen: %1").arg(m_target)
             + (log.isEmpty() ? QString() : " (GIF conversion: " + log.section('\n', -1).trimmed() + ")"));
        return;
    }
    m_usedCodec = "gif"; // what wrote the file (not the intermediate)
    succeed();
}

bool Exporter::writable(const ExportSettings& s, const QString& sampleFormat, QString* why) const
{
    const QString pixFmt = s.videoCodec.isEmpty() ? QString()
                           : HardwareEncoders::isHardwareCodec(s.videoCodec) ? HardwareEncoders::pixelFormat(s.videoCodec)
                                                                             : s.pixFmt;
    return ExportWriter::supportsOwn(m_path, s.output == ExportSettings::Output::ImageSequence ? "image2" : QString(),
                                     s.videoCodec, pixFmt, s.audioCodec, sampleFormat, why);
}

QSize Exporter::outputSize() const
{
    return m_outSize;
}

double Exporter::outputFps() const
{
    return m_outRate.fps();
}

bool Exporter::runAnalyses(const Timeline& tl, const ExportSettings& s)
{
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
    const quint64 run = ++m_threadRun;
    connect(m_analysis, &QThread::finished, this, [this, ok, error, run] {
        if (run != m_threadRun) return; // canceled as it ended: m_analysis is gone (cancel)
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
    // Not left running detached: it would still work while the program ends
    *m_buildCancel = true;
    ++m_threadRun; // its finished() may be queued already
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
        fail(T("Export abgebrochen."));
        return;
    }
    if (m_analysis) { // still analyzing for stabilization
        *m_analysisCancel = true;
        ++m_threadRun; // its finished() may be queued already
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
        fail(T("Export abgebrochen."));
        return;
    }
    if (!isRunning()) return;
    fail(T("Export abgebrochen.")); // the half-finished file goes, an older one of the target's name stays
}

void Exporter::succeed()
{
    QString error;
    if (!commitOutput(&error)) {
        fail(error);
        return;
    }
    emit progress(100);
    emit finished(true, T("Export fertig: %1").arg(m_target));
}

bool Exporter::commitOutput(QString* error)
{
    bool ok = true;
    if (m_settings.output == ExportSettings::Output::ImageSequence) {
        // Replace the frames of an older sequence of the same name (a shorter export would leave its higher numbers),
        // nothing else in that folder
        const QFileInfo target(m_target);
        const QDir dir(imageSequenceDir(m_target));
        QDir().mkpath(dir.path());
        const QRegularExpression frame(QString("^%1_\\d{5,}\\.%2$")
                                           .arg(QRegularExpression::escape(target.completeBaseName()),
                                                QRegularExpression::escape(target.suffix())));
        for (const QString& old : dir.entryList(QDir::Files))
            if (frame.match(old).hasMatch()) QFile::remove(dir.filePath(old));
        const QDir part(m_part);
        for (const QString& name : part.entryList(QDir::Files))
            ok = QFile::rename(part.filePath(name), dir.filePath(name)) && ok;
    } else if (!m_settings.replace) {
        // Without replacing: QFile::rename never overwrites (atomic on Linux and Windows), so a file another
        // render finished under this name meanwhile stays as it is
        ok = QFile::rename(m_part, m_target);
        if (!ok && QFileInfo::exists(m_target)) {
            if (error) *error = T("Export fehlgeschlagen: %1 gibt es inzwischen (ein anderes Rendern?)").arg(m_target);
            return false;
        }
    } else {
        ok = replaceFile(m_part, m_target);
    }
    if (!ok) {
        if (error) *error = T("Export fehlgeschlagen: %1").arg(m_target);
        return false;
    }
    for (const auto& srt : m_subtitles)
        if (!srt->commit()) {
            if (error) *error = T("Untertiteldatei kann nicht geschrieben werden: %1").arg(srt->fileName());
            return false;
        }
    m_subtitles.clear();
    discardOutput(); // only the intermediates are left (GIF, the empty folder of an image sequence)
    if (m_settings.output != ExportSettings::Output::ImageSequence) removeStaleParts(m_target);
    return true;
}

void Exporter::fail(const QString& message)
{
    cleanup();
    discardOutput();
    emit finished(false, message);
}

void Exporter::discardOutput()
{
    m_subtitles.clear(); // not committed: their temporary files go, existing SRTs stay
    if (!m_part.isEmpty()) {
        if (m_settings.output == ExportSettings::Output::ImageSequence) QDir(m_part).removeRecursively();
        else QFile::remove(m_part);
    }
    if (m_settings.output == ExportSettings::Output::Gif && !m_path.isEmpty()) QFile::remove(m_path); // intermediate
    m_part.clear();
    m_path.clear();
}

void Exporter::cleanup()
{
    if (m_timer.isActive()) --g_running;
    m_timer.stop();
    m_writer.reset(); // stops its threads and render workers, releases every frame before the renderer goes
    m_copy.clear();
    m_renderer.reset();
    m_mixer.reset();
    Picture::releasePools();
    MemoryRelease::requestRelease(); // the app gives the heap back once nothing renders any more
}
