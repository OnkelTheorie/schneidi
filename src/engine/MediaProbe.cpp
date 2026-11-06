// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/MediaProbe.h"
#include "engine/OpenMedia.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/samplefmt.h>
}

#include <QFileInfo>
#include <QImageReader>
#include <QStringList>

#include <algorithm>
#include <climits>
#include <cmath>
#include <memory>

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

struct CloseInput {
    void operator()(AVFormatContext* f) const { avformat_close_input(&f); }
};

// The streams as Engine::probe reports them; false = not decidable from what is known now
bool fill(const AVFormatContext* fmt, bool audioFile, MediaInfo* info)
{
    bool video = false, sound = false;
    QVector<AudioStreamInfo> streams;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* st = fmt->streams[i];
        const AVCodecParameters* p = st->codecpar;
        if (p->codec_type != AVMEDIA_TYPE_VIDEO && p->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        const AVCodec* codec = avcodec_find_decoder(p->codec_id);
        if (!codec) return false; // no decoder in this FFmpeg: not playable here
        if (p->codec_type == AVMEDIA_TYPE_VIDEO) {
            video = true; // also a cover picture: the picture stream when there is no other
            continue;
        }
        if (p->ch_layout.nb_channels <= 0) return false;
        const Sound s = soundOf(p, codec);
        if (s == Sound::Unknown) return false;
        sound = sound || s == Sound::Playable;
        AudioStreamInfo a;
        a.channels = p->ch_layout.nb_channels;
        // No empty and no "und" (undetermined) tag values
        if (const AVDictionaryEntry* t = av_dict_get(st->metadata, "title", nullptr, 0); t && qstrcmp(t->value, "und") != 0)
            a.title = QString::fromUtf8(t->value).trimmed();
        streams << a;
    }
    if (!video && !sound) return false; // nothing playable
    info->hasVideo = !audioFile && video;
    info->hasAudio = sound;
    if (sound) info->audioStreams = streams;
    return true;
}

} // namespace

bool isAudioFile(const QString& path)
{
    static const QStringList audioExt{"mp3", "wav", "flac", "ogg", "opus", "m4a", "aac", "wma", "aiff"};
    return audioExt.contains(QFileInfo(path).suffix().toLower());
}

std::optional<MediaInfo> read(const QString& path, FrameRate rate)
{
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
    AVFormatContext* raw = nullptr;
    if (OpenMedia::input(&raw, path.toUtf8().constData()) < 0) return std::nullopt;
    const std::unique_ptr<AVFormatContext, CloseInput> fmt(raw);
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
        duration = fmt->duration;
        if (!known(duration) || !fill(fmt.get(), audioFile, &info)) return std::nullopt;
    }
    // In the project frame rate, rounded to whole frames. A broken header can claim absurd lengths (time scale 1,
    // duration 2^32 s): capped at "unknown" (INT_MAX), never overflowing into a negative length
    // A sound shorter than half a frame (a click, a 1 ms effect) still is one frame long, not "no length"
    const double frames = std::max(1.0, std::nearbyint(double(duration) * rate.fps() / AV_TIME_BASE));
    info.length = frames < double(INT_MAX) ? int(frames) : INT_MAX;
    return info;
}

QString whyUnreadable(const QString& path)
{
    const QFileInfo fi(path);
    if (!fi.exists()) return fi.isSymLink() ? "a link to a file that does not exist" : "the file does not exist";
    if (fi.isDir()) return "it is a folder, not a file";
    if (!fi.isFile()) return "not a regular file (a named pipe, device or socket)";
    if (!fi.isReadable()) return "no permission to read it";
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

} // namespace MediaProbe
