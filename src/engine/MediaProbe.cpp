// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/MediaProbe.h"
#include "engine/OpenMedia.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/samplefmt.h>
}

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QThread>
#include <QStringList>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX // std::min/std::max below
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <memory>
#include <mutex>

namespace MediaProbe {

namespace {

// Extensions that are no media for schneidi (project files of other editors, titles, animations, documents) or still
// pictures Qt cannot read: not opened with libavformat (it would guess a format for some of them)
bool forOtherProducer(const QString& suffix)
{
    static const QStringList list{
        "mlt", "westley", "kdenlive", "melt", "inigo", "aep", "avd", "avif", "bmp", "dds", "exr", "gif", "graphics", "heic",
        "heif", "htm", "html", "ico", "jfx", "jef", "jfif", "jpg", "jpeg", "json", "jxl", "kdenlivetitle", "kino", "kra",
        "lottie", "mpl", "pcx", "pgm", "png", "psd", "qml", "rawr", "riv", "story", "svg", "svgz", "tga", "tgs", "tif",
        "tiff", "txt", "webp", "xcf", "xml"};
    return list.contains(suffix);
}

// Containers whose header holds what probe needs (stream durations, channels, codecs): libavformat's stream analysis
// would only add decoded details (pixel format, sample format), and it is the slow part of opening (it decodes the
// first frames of every stream: ~200 ms for a 1440p recording with three AAC streams)
bool headerSuffices(const AVInputFormat* f)
{
    static const char* const names[] = {"matroska,webm", "mov,mp4,m4a,3gp,3g2,mj2", "wav", "flac", "mp3", "avi"};
    return std::any_of(std::begin(names), std::end(names), [f](const char* n) { return qstrcmp(f->name, n) == 0; });
}

// Still picture suffixes: decided here when Qt can read the picture, like the still source (engine/video/Still) does
bool stillSuffix(const QString& suffix)
{
    static const QStringList list{"avif", "bmp",  "dds",  "exr", "gif", "heic", "heif", "ico", "jfif", "jpg", "jpeg", "jxl",
                                  "kra",  "pcx",  "png",  "psd", "svg", "svgz", "tga",  "tif", "tiff", "webp", "xcf"};
    return list.contains(suffix);
}

// Single pictures and picture sequences (a still image with another extension)
bool pictureDemuxer(const AVInputFormat* f)
{
    const QByteArray name(f->name);
    return name == "image2" || name.endsWith("_pipe");
}

// Sample formats the sound path plays. A stream decoding to another one (64-bit integer or double) is not its
// default sound: a file with only such streams has no sound for it.
bool playable(int format)
{
    switch (format) {
    case AV_SAMPLE_FMT_U8: case AV_SAMPLE_FMT_S16: case AV_SAMPLE_FMT_S32: case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_U8P: case AV_SAMPLE_FMT_S16P: case AV_SAMPLE_FMT_S32P: case AV_SAMPLE_FMT_FLTP:
        return true;
    default:
        return false;
    }
}

enum class Sound { Playable, NotPlayable, Unknown };

// Whether the stream's decoder delivers a playable sample format: known after the stream analysis, from the header
// only when the decoder declares formats and all of them agree
Sound soundOf(const AVCodecParameters* p, const AVCodec* codec)
{
    if (p->format != AV_SAMPLE_FMT_NONE) return playable(p->format) ? Sound::Playable : Sound::NotPlayable;
    const void* list = nullptr;
    int n = 0;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &list, &n) < 0) return Sound::Unknown;
#else
    list = codec->sample_fmts;
    for (const AVSampleFormat* f = codec->sample_fmts; f && *f != AV_SAMPLE_FMT_NONE; ++f) ++n;
#endif
    if (!list || n <= 0) {
        // Common decoders that declare nothing but only deliver playable formats (Opus: float, ALAC: 16/32-bit
        // integer)
        const AVCodecID id = p->codec_id;
        return id == AV_CODEC_ID_OPUS || id == AV_CODEC_ID_ALAC ? Sound::Playable : Sound::Unknown;
    }
    const auto* formats = static_cast<const AVSampleFormat*>(list);
    const int good = int(std::count_if(formats, formats + n, [](AVSampleFormat f) { return playable(f); }));
    return good == n ? Sound::Playable : good == 0 ? Sound::NotPlayable : Sound::Unknown;
}

// Length in µs from the header: the container's, else from the earliest stream start to the latest stream end
int64_t headerDuration(const AVFormatContext* fmt)
{
    if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) return fmt->duration;
    int64_t start = INT64_MAX, end = INT64_MIN, longest = AV_NOPTS_VALUE;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* st = fmt->streams[i];
        if (st->duration == AV_NOPTS_VALUE) continue;
        const int64_t d = av_rescale_q(st->duration, st->time_base, AV_TIME_BASE_Q);
        longest = std::max(longest, d);
        if (st->start_time == AV_NOPTS_VALUE) continue;
        const int64_t s = av_rescale_q(st->start_time, st->time_base, AV_TIME_BASE_Q);
        start = std::min(start, s);
        end = std::max(end, s + d);
    }
    if (start != INT64_MAX && end != INT64_MIN) longest = std::max(longest, end - start);
    return longest;
}

// Can a decoder here turn this still picture into pixels? Qt reads the header only (canRead); a broken header can
// state an absurd size (a flipped bit: 4294967167x2147483647), and a picture beyond FFmpeg's limit for one image
// ((w+128)*(h+128) < INT_MAX/8, so not even 16384x16384) that Qt cannot decode at a smaller size (only JPEG can) and
// Qt's own memory limit refuses as well comes out as no picture at all: black in a render, "could not decode" in
// `frames`. Such a file is unreadable from the start instead.
bool decodableStill(QImageReader& reader)
{
    const QSize size = reader.size();
    if (!size.isValid() || size.isEmpty()) return false;
    const double pixels = double(size.width() + 128) * double(size.height() + 128);
    return pixels < double(INT_MAX / 8) || reader.supportsOption(QImageIOHandler::ScaledSize);
}

// Length in µs from the last packets in the file's tail (Matroska of a recorder that crashed or was killed, a live
// stream: no duration in the header, and the stream analysis only sees the start), counted from the file's start
// time (OpenMedia::scanTail: more of the tail when no timed packet was there, kTailScanMaxBytes in all at most).
// AV_NOPTS_VALUE if none was found. Moves the demuxer (and leaves its file in an error state once the limit was hit).
int64_t tailDuration(AVFormatContext* fmt)
{
    if (!fmt->pb) return AV_NOPTS_VALUE;
    const int64_t start = fmt->start_time != AV_NOPTS_VALUE ? fmt->start_time : 0;
    int64_t end = AV_NOPTS_VALUE;
    OpenMedia::scanTail(fmt, [&](const AVPacket* p) {
        const int64_t ts = p->pts != AV_NOPTS_VALUE ? p->pts : p->dts;
        if (ts == AV_NOPTS_VALUE || p->stream_index < 0 || unsigned(p->stream_index) >= fmt->nb_streams) return false;
        const AVStream* st = fmt->streams[p->stream_index];
        const int64_t e = av_rescale_q(ts + std::max<int64_t>(0, p->duration), st->time_base, AV_TIME_BASE_Q);
        if (end == AV_NOPTS_VALUE || e > end) end = e;
        return true;
    });
    return end == AV_NOPTS_VALUE ? AV_NOPTS_VALUE : end - start;
}

struct CloseInput {
    void operator()(AVFormatContext* f) const { avformat_close_input(&f); }
};

// The streams as Engine::probe reports them; false = not decidable from what is known now (`lenient`: decided from
// what is known: streams that cannot be played are left out, false only when no stream can be played at all).
// A sound stream that cannot be played keeps its place in the list (the sound reader counts a file's audio streams by
// their place), as one that decodes to a sample format the sound path does not play.
bool fill(const AVFormatContext* fmt, bool audioFile, MediaInfo* info, bool lenient = false)
{
    bool video = false, sound = false, moving = false; // moving: a picture stream that is no cover
    QVector<AudioStreamInfo> streams;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* st = fmt->streams[i];
        const AVCodecParameters* p = st->codecpar;
        if (p->codec_type != AVMEDIA_TYPE_VIDEO && p->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        const AVCodec* codec = avcodec_find_decoder(p->codec_id);
        const bool isVideo = p->codec_type == AVMEDIA_TYPE_VIDEO;
        // No decoder in this FFmpeg (an unknown codec, one the slim package build leaves out), values unknown
        Sound s = Sound::NotPlayable;
        if (!codec) {
            if (!lenient) return false;
            if (isVideo) continue;
        } else if (isVideo) {
            video = true; // also a cover picture: the picture stream when there is no other
            moving = moving || !(st->disposition & AV_DISPOSITION_ATTACHED_PIC);
            continue;
        } else if (p->ch_layout.nb_channels <= 0 || p->sample_rate <= 0) {
            if (!lenient) return false;
        } else {
            s = soundOf(p, codec);
            if (s == Sound::Unknown && !lenient) return false;
        }
        sound = sound || s == Sound::Playable;
        AudioStreamInfo a;
        a.channels = p->ch_layout.nb_channels > 0 ? p->ch_layout.nb_channels : a.channels;
        a.playable = s == Sound::Playable;
        // No empty and no "und" (undetermined) tag values
        if (const AVDictionaryEntry* t = av_dict_get(st->metadata, "title", nullptr, 0); t && qstrcmp(t->value, "und") != 0)
            a.title = QString::fromUtf8(t->value).trimmed();
        streams << a;
    }
    // Nothing playable; or only a cover picture beside sound that cannot be played (the cover shows with sound)
    if (!sound && !moving) return false;
    info->hasVideo = !audioFile && video;
    info->hasAudio = sound;
    if (sound) info->audioStreams = streams;
    return true;
}

// A stream with a decoder whose values the stream analysis did not find: no packet of it within the analysed start
// (sound that starts late in an MPEG-TS, an empty sound PID, a picture after seconds of sound). Reading further may
// find them.
int unsettledCount(const AVFormatContext* fmt)
{
    int n = 0;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVCodecParameters* p = fmt->streams[i]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(p->codec_id);
        if (!codec) continue; // reading further finds no decoder
        if (p->codec_type == AVMEDIA_TYPE_VIDEO && (p->width <= 0 || p->height <= 0)) ++n;
        if (p->codec_type == AVMEDIA_TYPE_AUDIO
            && (p->ch_layout.nb_channels <= 0 || p->sample_rate <= 0 || soundOf(p, codec) == Sound::Unknown))
            ++n;
    }
    return n;
}

bool unsettled(const AVFormatContext* fmt)
{
    return unsettledCount(fmt) > 0;
}

// What the longer analysis (kLongAnalysis) did for a file, while its size and time of change stay: found what the
// short one missed (open it with the long options at once next time), or nothing more (not read that far again)
constexpr int64_t kLongAnalysis = 60; // seconds; and 2 MB per second to read at most
enum class LongOutcome { Helped, InVain };
struct Analysed {
    qint64 size = -1, modified = 0;
    LongOutcome outcome = LongOutcome::InVain;
};
std::mutex g_longMutex;
QHash<QString, Analysed>& longAnalyses()
{
    static QHash<QString, Analysed> files;
    return files;
}
std::atomic<int> g_longRuns{0};

std::optional<LongOutcome> longOutcome(const QString& path, const QFileInfo& fi)
{
    std::lock_guard lock(g_longMutex);
    const auto it = longAnalyses().constFind(path);
    if (it == longAnalyses().cend() || it->size != fi.size() || it->modified != fi.lastModified().toMSecsSinceEpoch())
        return std::nullopt;
    return it->outcome;
}

void rememberLong(const QString& path, const QFileInfo& fi, LongOutcome outcome)
{
    std::lock_guard lock(g_longMutex);
    longAnalyses().insert(path, Analysed{fi.size(), fi.lastModified().toMSecsSinceEpoch(), outcome});
}

enum class Access { Open, Locked, Failed };

// Can the file be opened for reading now? Locked: another program holds it without sharing read access (Windows:
// ERROR_SHARING_VIOLATION or ERROR_LOCK_VIOLATION - a recorder, a copy in progress, a virus scan of a fresh
// download; elsewhere EBUSY, or EACCES although the permissions allow reading). FFmpeg's own error says "permission
// denied" then. Failed: any other reason, said in `error`.
Access access(const QFileInfo& fi, QString* error = nullptr)
{
    if (!fi.isFile()) return Access::Failed;
    QFile f(fi.filePath());
    if (f.open(QIODevice::ReadOnly)) return Access::Open;
    if (error) *error = f.errorString();
#ifdef Q_OS_WIN
    // The long-path form: \\?\C:\... for a drive, \\?\UNC\server\share\... for a network share
    const QString plain = QDir::toNativeSeparators(fi.absoluteFilePath());
    const QString native = plain.startsWith("\\\\") ? "\\\\?\\UNC\\" + plain.mid(2) : "\\\\?\\" + plain;
    const HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(native.utf16()), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return Access::Open; // free again meanwhile
    }
    const DWORD e = GetLastError();
    return e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION ? Access::Locked : Access::Failed;
#else
    const int fd = ::open(QFile::encodeName(fi.filePath()).constData(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ::close(fd);
        return Access::Open;
    }
    // Only EBUSY means "in use" here. EACCES although the permission bits allow reading comes from a security
    // module (AppArmor, SELinux, a Flatpak/Snap sandbox): no other program holds the file, waiting does not help -
    // said with the system's message (QFile's errorString) instead of retrying
    return errno == EBUSY ? Access::Locked : Access::Failed;
#endif
}

// Written to this many seconds ago or less: maybe still being written
constexpr qint64 kFreshSeconds = 30;

bool fresh(const QFileInfo& fi)
{
    const QDateTime changed = fi.lastModified();
    return changed.isValid() && std::abs(changed.secsTo(QDateTime::currentDateTime())) <= kFreshSeconds;
}

} // namespace

Busy busy(const QString& path)
{
    const QFileInfo fi(path);
    if (!fi.isFile()) return Busy::No;
    if (access(fi) == Access::Locked) return Busy::Locked;
    return fresh(fi) ? Busy::Written : Busy::No;
}

int longAnalysesForTest()
{
    return g_longRuns.load();
}

void forgetForTest()
{
    std::lock_guard lock(g_longMutex);
    longAnalyses().clear();
}

namespace {
QString why(const QString& path);
} // namespace

QString whyUnreadable(const QString& path)
{
    QString reason = why(path);
    const QFileInfo fi(path);
    if (fi.isFile() && access(fi) == Access::Open && fresh(fi))
        reason += "; it was written to moments ago - maybe it is still being recorded, downloaded or copied";
    return reason;
}

bool isAudioFile(const QString& path)
{
    static const QStringList audioExt{"mp3", "wav", "flac", "ogg", "opus", "m4a", "aac", "wma", "aiff"};
    return audioExt.contains(QFileInfo(path).suffix().toLower());
}

std::optional<MediaInfo> read(const QString& path, FrameRate rate, const ReadOptions& options)
{
    if (options.partial) *options.partial = false;
    const QFileInfo fi(path);
    if (!fi.isFile()) return std::nullopt;
    const QString suffix = fi.suffix().toLower();
    if (stillSuffix(suffix)) {
        // Still picture: 5 seconds as a default length; QImageReader only reads the header here
        QImageReader reader(path);
        if (!reader.canRead() || !decodableStill(reader)) return std::nullopt;
        MediaInfo info;
        info.path = path;
        info.name = fi.fileName();
        info.isImage = true;
        info.hasVideo = true;
        info.length = 5 * rate.timebase();
        return info;
    }
    if (forOtherProducer(suffix)) return std::nullopt;
    const QByteArray file = path.toUtf8();
    // A file whose short analysis missed streams the long one found: the long one at once (no second open)
    const std::optional<LongOutcome> before = longOutcome(path, fi);
    bool longRan = false;
    const auto openLong = [&](AVFormatContext** out) {
        AVDictionary* o = nullptr;
        av_dict_set_int(&o, "analyzeduration", kLongAnalysis * AV_TIME_BASE, 0);
        av_dict_set_int(&o, "probesize", kLongAnalysis * 2 * 1024 * 1024, 0);
        const int r = OpenMedia::input(out, file.constData(), o);
        av_dict_free(&o);
        if (r >= 0) longRan = true;
        return r;
    };
    const bool longAtOnce = options.longAnalysis && before == LongOutcome::Helped;
    int shortUnsettled = -1; // streams the short analysis left unsettled, when the long one followed
    const auto openIt = [&](AVFormatContext** out) {
        return longAtOnce ? openLong(out) : OpenMedia::input(out, file.constData());
    };
    AVFormatContext* raw = nullptr;
    int opened = openIt(&raw);
    // Held by another program a moment (a virus scan of a fresh download, a copy finishing): a few tries, ~1 s in all.
    // Only failures pay for it, and not the quick look.
    for (int tries = 0; options.longAnalysis && opened < 0 && tries < 4 && access(fi) == Access::Locked; ++tries) {
        QThread::msleep(250);
        opened = openIt(&raw);
    }
    if (opened < 0) return std::nullopt;
    std::unique_ptr<AVFormatContext, CloseInput> fmt(raw);
    if (pictureDemuxer(fmt->iformat)) {
        // A still picture with another extension: as a still when Qt reads it
        QImageReader reader(path);
        if (!reader.canRead() || !decodableStill(reader)) return std::nullopt;
        MediaInfo info;
        info.path = path;
        info.name = fi.fileName();
        info.isImage = true;
        info.hasVideo = true;
        info.length = 5 * rate.timebase();
        return info;
    }

    MediaInfo info;
    info.path = path;
    info.name = fi.fileName();
    const bool audioFile = isAudioFile(path);
    // A length of 0 or less is no length (fragmented MP4 of an aborted recording: mvhd duration 0)
    const auto known = [](int64_t d) { return d != AV_NOPTS_VALUE && d > 0; };
    int64_t duration = headerSuffices(fmt->iformat) ? headerDuration(fmt.get()) : AV_NOPTS_VALUE;
    if (!known(duration) || !fill(fmt.get(), audioFile, &info)) {
        // libavformat's stream analysis: decodes the first frames, estimates the length
        if (avformat_find_stream_info(fmt.get(), nullptr) < 0) return std::nullopt;
        if (!longRan && !fill(fmt.get(), audioFile, &info) && unsettled(fmt.get()) && before != LongOutcome::InVain) {
            // A stream without a packet in the analysed start (5 s): once more, reading up to 60 s (as the preview's
            // reader does for a picture that starts late). Not on a quick look (the result is partial then).
            if (!options.longAnalysis) {
                if (options.partial) *options.partial = true;
            } else if (AVFormatContext* longer = nullptr; openLong(&longer) >= 0) {
                ++g_longRuns; // a second open: what remembering it saves next time
                shortUnsettled = unsettledCount(fmt.get());
                fmt.reset(longer);
                if (avformat_find_stream_info(fmt.get(), nullptr) < 0) {
                    rememberLong(path, fi, LongOutcome::InVain);
                    return std::nullopt;
                }
            }
        }
        duration = fmt->duration;
        if (!known(duration)) duration = tailDuration(fmt.get());
        // What can be played (a stream that cannot does not refuse the others)
        const bool accepted = known(duration) && fill(fmt.get(), audioFile, &info, true);
        // Remembered after a second open: helped when it settled any stream the short analysis had not (a late sound
        // beside an empty PID: the late one counts), else in vain (not read that far again). A file opened with the
        // long analysis at once keeps what it was remembered with.
        if (shortUnsettled >= 0)
            rememberLong(path, fi,
                         accepted && unsettledCount(fmt.get()) < shortUnsettled ? LongOutcome::Helped : LongOutcome::InVain);
        if (!accepted) return std::nullopt;
    }
    // In the project frame rate, rounded to whole frames. A broken header can claim absurd lengths (time scale 1,
    // duration 2^32 s): capped at "unknown" (INT_MAX), never overflowing into a negative length
    // A sound shorter than half a frame (a click, a 1 ms effect) still is one frame long, not "no length"
    const double frames = std::max(1.0, std::nearbyint(double(duration) * rate.fps() / AV_TIME_BASE));
    info.length = frames < double(INT_MAX) ? int(frames) : INT_MAX;
    return info;
}

namespace {

QString why(const QString& path)
{
    const QFileInfo fi(path);
    if (!fi.exists()) return fi.isSymLink() ? "a link to a file that does not exist" : "the file does not exist";
    if (fi.isDir()) return "it is a folder, not a file";
    if (!fi.isFile()) return "not a regular file (a named pipe, device or socket)";
    if (!fi.isReadable()) return "no permission to read it";
    QString openError;
    switch (access(fi, &openError)) {
    case Access::Locked:
        return "another program holds the file (still being recorded, copied or scanned?) - try again in a moment";
    case Access::Failed: return QString("it cannot be opened (%1)").arg(openError);
    case Access::Open: break;
    }
    if (fi.size() == 0) return "the file is empty (0 bytes)";
    const QString suffix = fi.suffix().toLower();
    if (stillSuffix(suffix)) {
        QImageReader reader(path);
        if (!reader.canRead()) return "not a picture Qt can read (broken or another format than its extension says)";
        const QSize size = reader.size();
        if (!size.isValid() || size.isEmpty()) return "the picture's header is broken (no valid size)";
        if (!decodableStill(reader))
            return QString("the picture is too large to decode (%1x%2 pixels)").arg(size.width()).arg(size.height());
        return "the picture cannot be read";
    }
    if (forOtherProducer(suffix)) return QString("'.%1' files are no media schneidi plays").arg(suffix);
    AVFormatContext* raw = nullptr;
    if (const int r = OpenMedia::input(&raw, path.toUtf8().constData()); r < 0) {
        char text[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(r, text, sizeof text);
        return QString("no known media format, or its header is damaged (%1)").arg(QString::fromUtf8(text));
    }
    const std::unique_ptr<AVFormatContext, CloseInput> fmt(raw);
    avformat_find_stream_info(fmt.get(), nullptr);
    int video = 0, audio = 0, other = 0;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVCodecParameters* p = fmt->streams[i]->codecpar;
        const char* codec = avcodec_get_name(p->codec_id);
        if (p->codec_type == AVMEDIA_TYPE_VIDEO) ++video;
        else if (p->codec_type == AVMEDIA_TYPE_AUDIO) ++audio;
        else { ++other; continue; }
        const AVCodec* decoder = avcodec_find_decoder(p->codec_id);
        if (!decoder) return QString("no decoder for its %1 stream (%2)").arg(p->codec_type == AVMEDIA_TYPE_VIDEO ? "video" : "audio", codec);
        if (p->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        if (p->ch_layout.nb_channels <= 0) return QString("its sound stream (%1) has no channels").arg(codec);
        if (p->sample_rate <= 0) return QString("its sound stream (%1) has no sample rate").arg(codec);
        if (soundOf(p, decoder) == Sound::NotPlayable) {
            const char* sampleFormat = av_get_sample_fmt_name(AVSampleFormat(p->format));
            return QString("its sound is in a sample format schneidi does not play (%1, %2)")
                .arg(codec, sampleFormat ? sampleFormat : "?");
        }
    }
    if (video == 0 && audio == 0)
        return other > 0 ? QString("it has no picture and no sound (only %1 subtitle or data stream%2)").arg(other).arg(other == 1 ? "" : "s")
                         : QString("it has no picture and no sound");
    if (fmt->duration == AV_NOPTS_VALUE || fmt->duration <= 0) return "its length is unknown (a damaged or unfinished file)";
    return "damaged or unsupported";
}

} // namespace

namespace {

// A stream's bit rate from its parameters or a Matroska statistics tag ("BPS", "BPS-eng"), 0 = unknown
int64_t streamBitRate(const AVStream* st)
{
    if (st->codecpar->bit_rate > 0) return st->codecpar->bit_rate;
    for (const char* key : {"BPS", "BPS-eng"})
        if (const AVDictionaryEntry* e = av_dict_get(st->metadata, key, nullptr, 0)) {
            const qint64 v = QByteArray(e->value).toLongLong();
            if (v > 0) return v;
        }
    return 0;
}

struct RateEntry {
    qint64 size = -1, modified = 0;
    std::optional<SourceBitrate::Video> video;
};

std::optional<SourceBitrate::Video> readVideoRate(const QString& path, const QFileInfo& fi)
{
    if (stillSuffix(fi.suffix().toLower()) || forOtherProducer(fi.suffix().toLower()) || isAudioFile(path))
        return std::nullopt;
    AVFormatContext* raw = nullptr;
    if (OpenMedia::input(&raw, path.toUtf8().constData()) < 0) return std::nullopt;
    std::unique_ptr<AVFormatContext, CloseInput> fmt(raw);
    if (pictureDemuxer(fmt->iformat)) return std::nullopt;
    const auto bestVideo = [&] {
        const int v = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        return v >= 0 && !(fmt->streams[v]->disposition & AV_DISPOSITION_ATTACHED_PIC) ? v : -1;
    };
    int v = bestVideo();
    const auto settled = [&](int i) {
        const AVCodecParameters* p = fmt->streams[i]->codecpar;
        return p->width > 0 && p->height > 0 && fmt->duration > 0;
    };
    // Other containers (MPEG-TS, …) know sizes and lengths only after the stream analysis
    if (!headerSuffices(fmt->iformat) || v < 0 || !settled(v)) {
        if (avformat_find_stream_info(fmt.get(), nullptr) < 0) return std::nullopt;
        v = bestVideo();
    }
    if (v < 0) return std::nullopt;
    const AVStream* st = fmt->streams[v];
    SourceBitrate::Video out;
    if (st->codecpar->width > 0 && st->codecpar->height > 0) out.size = QSize(st->codecpar->width, st->codecpar->height);
    const AVRational r = st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0 ? st->avg_frame_rate : st->r_frame_rate;
    if (r.num > 0 && r.den > 0) out.fps = av_q2d(r);
    if (out.fps > 1000) out.fps = 0; // a container's time base, not a frame rate
    int64_t bits = streamBitRate(st);
    if (bits <= 0) {
        // The whole file's rate, without the sound (and other streams) where their rates are known
        int64_t total = fmt->bit_rate;
        if (total <= 0 && fmt->duration > 0 && fi.size() > 0)
            total = int64_t(double(fi.size()) * 8 * AV_TIME_BASE / double(fmt->duration));
        int64_t others = 0;
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            if (int(i) == v) continue;
            const AVStream* o = fmt->streams[i];
            int64_t b = streamBitRate(o);
            const AVCodecParameters* p = o->codecpar;
            if (b <= 0 && p->codec_type == AVMEDIA_TYPE_AUDIO && p->sample_rate > 0 && p->ch_layout.nb_channels > 0) {
                const int depth = av_get_bits_per_sample(p->codec_id); // PCM: exact
                if (depth > 0) b = int64_t(p->sample_rate) * p->ch_layout.nb_channels * depth;
            }
            others += std::max<int64_t>(0, b);
        }
        bits = total - others;
        if (total > 0 && bits < total / 10) bits = total / 10; // rates that do not add up: never next to nothing
    }
    if (bits <= 0) return std::nullopt;
    out.kbit = double(bits) / 1000.0;
    return out;
}

} // namespace

std::optional<SourceBitrate::Video> videoRate(const QString& path)
{
    static std::mutex mutex;
    static QHash<QString, RateEntry> cache;
    const QFileInfo fi(path);
    if (!fi.isFile()) return std::nullopt;
    const qint64 size = fi.size(), modified = fi.lastModified().toMSecsSinceEpoch();
    {
        std::lock_guard lock(mutex);
        const auto it = cache.constFind(path);
        if (it != cache.cend() && it->size == size && it->modified == modified) return it->video;
    }
    RateEntry e{size, modified, readVideoRate(path, fi)};
    std::lock_guard lock(mutex);
    if (cache.size() > 2000) cache.clear(); // a long session with very many files: start over
    cache.insert(path, e);
    return e.video;
}

} // namespace MediaProbe
