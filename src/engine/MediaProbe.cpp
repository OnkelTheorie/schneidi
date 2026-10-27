#include "engine/MediaProbe.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <QFileInfo>
#include <QStringList>

#include <algorithm>
#include <climits>
#include <cmath>
#include <memory>

namespace MediaProbe {

namespace {

// Extensions MLT's loader (loader.dict) gives to another producer than avformat first: still images, MLT/Kdenlive
// projects, titles, animations. Their probing stays MLT's.
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

// Single pictures and picture sequences (a still image with another extension): MLT treats them its own way
bool pictureDemuxer(const AVInputFormat* f)
{
    const QByteArray name(f->name);
    return name == "image2" || name.endsWith("_pipe");
}

// Sample formats MLT's avformat producer plays. A stream decoding to another one (64-bit integer or double) is not its
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
        // integer); test_media_probe holds both against MLT
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
    if (fmt->duration != AV_NOPTS_VALUE) return fmt->duration;
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
        if (!codec) return false; // MLT skips such streams, its stream numbering with them: left to MLT
        if (p->codec_type == AVMEDIA_TYPE_VIDEO) {
            video = true; // also a cover picture: MLT's picture stream when there is no other
            continue;
        }
        if (p->ch_layout.nb_channels <= 0) return false;
        const Sound s = soundOf(p, codec);
        if (s == Sound::Unknown) return false;
        sound = sound || s == Sound::Playable;
        AudioStreamInfo a;
        a.channels = p->ch_layout.nb_channels;
        // MLT keeps no empty and no "und" (undetermined) tag values
        if (const AVDictionaryEntry* t = av_dict_get(st->metadata, "title", nullptr, 0); t && qstrcmp(t->value, "und") != 0)
            a.title = QString::fromUtf8(t->value).trimmed();
        streams << a;
    }
    if (!video && !sound) return false; // nothing MLT plays: its producer does not open (length 0)
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
    if (!fi.isFile() || forOtherProducer(fi.suffix().toLower())) return std::nullopt;
    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, path.toUtf8().constData(), nullptr, nullptr) < 0) return std::nullopt;
    const std::unique_ptr<AVFormatContext, CloseInput> fmt(raw);
    if (pictureDemuxer(fmt->iformat)) return std::nullopt;

    MediaInfo info;
    info.path = path;
    info.name = fi.fileName();
    const bool audioFile = isAudioFile(path);
    int64_t duration = headerSuffices(fmt->iformat) ? headerDuration(fmt.get()) : AV_NOPTS_VALUE;
    if (duration == AV_NOPTS_VALUE || !fill(fmt.get(), audioFile, &info)) {
        // What MLT's producer runs on opening: decodes the first frames, estimates the length
        if (avformat_find_stream_info(fmt.get(), nullptr) < 0) return std::nullopt;
        duration = fmt->duration;
        if (duration == AV_NOPTS_VALUE || !fill(fmt.get(), audioFile, &info)) return std::nullopt;
    }
    // In the project frame rate, rounded to whole frames like MLT's producer
    info.length = int(std::lrint(double(duration) * rate.fps() / AV_TIME_BASE));
    return info;
}

} // namespace MediaProbe
