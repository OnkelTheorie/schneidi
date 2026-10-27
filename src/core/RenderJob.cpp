#include "core/RenderJob.h"

#include "core/I18n.h"
#include "core/ProjectFormat.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>

const QVector<RenderFormatInfo>& renderFormats()
{
    using K = RenderKind;
    // Order = order on the Deliver page (separator between the groups); unknown ids fall back to the first one
    static const QVector<RenderFormatInfo> formats{
        // Delivery: small files with a quality choice
        {"h264", "H.264 (MP4)", "mp4", "libx264", "aac", "yuv420p", K::Video, {18, 22, 28}, true, true, ""},
        {"h265", "H.265 (MP4)", "mp4", "libx265", "aac", "yuv420p", K::Video, {18, 22, 28}, true, true, ""},
        // SVT-AV1: CRF 0-63, preset 8 = about as fast as x264 "medium" at a clearly smaller file
        {"av1", "AV1 (MP4)", "mp4", "libsvtav1", "aac", "yuv420p", K::Video, {28, 35, 42}, false, false, "preset=8"},
        // VP9 constant quality needs bit rate 0; "good" + cpu-used 4 + row-mt: reasonable speed
        {"vp9", "VP9 (WebM)", "webm", "libvpx-vp9", "libopus", "yuv420p", K::Video, {24, 31, 37}, false, false,
         "vb=0 deadline=good cpu-used=4 row-mt=1"},
        // Mastering / exchange with other editors: fixed (high) quality, uncompressed sound
        {"prores", "ProRes 422 HQ (MOV)", "mov", "prores_ks", "pcm_s24le", "yuv422p10le", K::Video, {}, false, false, "vprofile=3"},
        {"prores422", "ProRes 422 (MOV)", "mov", "prores_ks", "pcm_s24le", "yuv422p10le", K::Video, {}, false, false, "vprofile=2"},
        {"proreslt", "ProRes 422 LT (MOV)", "mov", "prores_ks", "pcm_s24le", "yuv422p10le", K::Video, {}, false, false, "vprofile=1"},
        {"proresproxy", "ProRes 422 Proxy (MOV)", "mov", "prores_ks", "pcm_s24le", "yuv422p10le", K::Video, {}, false, false,
         "vprofile=0"},
        {"prores4444", "ProRes 4444 (MOV)", "mov", "prores_ks", "pcm_s24le", "yuv444p10le", K::Video, {}, false, false, "vprofile=4"},
        {"dnxhr-sq", "DNxHR SQ (MOV)", "mov", "dnxhd", "pcm_s24le", "yuv422p", K::Video, {}, false, false, "vprofile=dnxhr_sq"},
        {"dnxhr-hq", "DNxHR HQ (MOV)", "mov", "dnxhd", "pcm_s24le", "yuv422p", K::Video, {}, false, false, "vprofile=dnxhr_hq"},
        {"dnxhr-hqx", N_("DNxHR HQX 10 Bit (MOV)"), "mov", "dnxhd", "pcm_s24le", "yuv422p10le", K::Video, {}, false, false,
         "vprofile=dnxhr_hqx"},
        // No sound: GIF (lossless intermediate in RGB, then palette per ffmpeg), image sequences
        {"gif", N_("Animiertes GIF"), "gif", "png", "", "rgb24", K::Gif, {}, false, false, ""},
        {"png", N_("Einzelbilder – PNG"), "png", "png", "", "rgb24", K::ImageSequence, {}, false, false, ""},
        {"tiff", N_("Einzelbilder – TIFF"), "tif", "tiff", "", "rgb24", K::ImageSequence, {}, false, false, ""},
        {"jpeg", N_("Einzelbilder – JPEG"), "jpg", "mjpeg", "", "yuvj444p", K::ImageSequence, {}, false, false, "qscale=2"},
        {"aac", N_("Nur Audio – AAC (M4A)"), "m4a", "", "aac", "", K::Audio, {}, false, false, ""},
        {"mp3", N_("Nur Audio – MP3"), "mp3", "", "libmp3lame", "", K::Audio, {}, false, false, ""},
        {"opus", N_("Nur Audio – Opus"), "opus", "", "libopus", "", K::Audio, {}, false, false, ""},
        {"wav", N_("Nur Audio – WAV"), "wav", "", "pcm_s24le", "", K::Audio, {}, false, false, ""},
        {"aiff", N_("Nur Audio – AIFF (Apple)"), "aiff", "", "pcm_s24be", "", K::Audio, {}, false, false, ""},
        {"flac", N_("Nur Audio – FLAC"), "flac", "", "flac", "", K::Audio, {}, false, false, ""},
        {"alac", N_("Nur Audio – Apple Lossless (M4A)"), "m4a", "", "alac", "", K::Audio, {}, false, false, ""},
    };
    return formats;
}

const RenderFormatInfo& renderFormat(const QString& id)
{
    const auto& f = renderFormats();
    for (const RenderFormatInfo& i : f)
        if (id == QLatin1String(i.id)) return i;
    return f.first();
}

QString videoEncoderId(VideoEncoder e)
{
    switch (e) {
    case VideoEncoder::Auto: return "auto";
    case VideoEncoder::Software: return "software";
    case VideoEncoder::Nvenc: return "nvenc";
    case VideoEncoder::Vaapi: return "vaapi";
    case VideoEncoder::Qsv: return "qsv";
    }
    return "auto";
}

VideoEncoder videoEncoderFromId(const QString& id, VideoEncoder fallback, bool* ok)
{
    for (VideoEncoder e : {VideoEncoder::Auto, VideoEncoder::Software, VideoEncoder::Nvenc, VideoEncoder::Vaapi, VideoEncoder::Qsv})
        if (id.compare(videoEncoderId(e), Qt::CaseInsensitive) == 0) {
            if (ok) *ok = true;
            return e;
        }
    if (ok) *ok = false;
    return fallback;
}

QString videoEncoderLabel(VideoEncoder e)
{
    switch (e) {
    case VideoEncoder::Auto: return T("Automatisch");
    case VideoEncoder::Software: return T("Prozessor (Software)");
    case VideoEncoder::Nvenc: return "NVIDIA (NVENC)";
    case VideoEncoder::Vaapi: return "AMD/Intel (VAAPI)";
    case VideoEncoder::Qsv: return "Intel Quick Sync";
    }
    return {};
}

bool RenderSettings::audioOnly() const { return !*renderFormat(format).videoCodec; }

bool RenderSettings::hasAudio() const { return *renderFormat(format).audioCodec; }

RenderKind RenderSettings::kind() const { return renderFormat(format).kind; }

bool RenderSettings::hasEncoderChoice() const { return renderFormat(format).hardware; }

bool RenderSettings::hasAudioBitrate() const
{
    const QLatin1String c(renderFormat(format).audioCodec);
    return c == QLatin1String("aac") || c == QLatin1String("libmp3lame") || c == QLatin1String("libopus");
}

QVector<int> RenderSettings::audioBitChoices() const
{
    // 64 bit does not exist: MLT mixes in 32-bit float at most
    const QLatin1String c(renderFormat(format).audioCodec);
    if (c.startsWith(QLatin1String("pcm_"))) return {16, 24, 32};
    if (c == QLatin1String("alac") || c == QLatin1String("flac")) return {16, 24};
    return {};
}

static int validBits(const RenderSettings& s)
{
    const QVector<int> choices = s.audioBitChoices();
    if (choices.contains(s.audioBits)) return s.audioBits;
    return choices.contains(24) ? 24 : 0;
}

QString RenderSettings::audioCodec() const
{
    const QString c = renderFormat(format).audioCodec;
    if (!c.startsWith("pcm_")) return c;
    const QString endian = c.right(2); // le/be
    switch (validBits(*this)) {
    case 16: return "pcm_s16" + endian;
    case 32: return "pcm_f32" + endian;
    default: return "pcm_s24" + endian;
    }
}

QString RenderSettings::audioSampleFormat() const
{
    const QLatin1String c(renderFormat(format).audioCodec);
    if (c == QLatin1String("alac")) return validBits(*this) == 16 ? "s16p" : "s32p"; // s32p = 24 bit
    if (c == QLatin1String("flac")) return validBits(*this) == 16 ? "s16" : "s32";   // s32 = 24 bit
    return {};
}

FrameRate RenderSettings::outputRate(FrameRate timeline) const
{
    if (rateNum <= 0 || rateDen <= 0 || audioOnly()) return timeline;
    return {rateNum, rateDen};
}

QString imageSequenceDir(const QString& path)
{
    const QFileInfo fi(path);
    return fi.dir().filePath(fi.completeBaseName());
}

QString imageSequencePattern(const QString& path)
{
    const QFileInfo fi(path);
    return QDir(imageSequenceDir(path)).filePath(fi.completeBaseName() + "_%05d." + fi.suffix());
}

QString firstOutputFile(const QString& path, const RenderSettings& s)
{
    if (s.kind() != RenderKind::ImageSequence) return path;
    return imageSequencePattern(path).replace("%05d", "00001");
}

FrameRate nearestExportRate(double fps)
{
    FrameRate best = nearestFrameRate(fps);
    for (const FrameRate& r : kExportOnlyRates)
        if (std::abs(r.fps() - fps) < std::abs(best.fps() - fps)) best = r;
    return best;
}

QSize sizeForShortSide(QSize timeline, int shortSide)
{
    const int w = timeline.width(), h = timeline.height();
    if (w <= 0 || h <= 0) return {};
    if (w >= h) return {int(std::lround(double(shortSide) * w / h)) & ~1, shortSide & ~1};
    return {shortSide & ~1, int(std::lround(double(shortSide) * h / w)) & ~1};
}

QSize RenderSettings::outputSize(QSize timeline) const
{
    if (audioOnly()) return {};
    if (size.isValid() && !size.isEmpty()) return {size.width() & ~1, size.height() & ~1};
    if (shortSide > 0) return sizeForShortSide(timeline, shortSide);
    return {timeline.width() & ~1, timeline.height() & ~1};
}

int RenderSettings::crf() const
{
    // Web = high quality with a bit rate cap
    return renderFormat(format).crf[quality == kQualityWeb ? 0 : std::clamp(quality, 0, 2)];
}

int RenderSettings::maxBitrateK(QSize output, FrameRate rate) const
{
    if (quality != kQualityWeb || !renderFormat(format).webQuality || audioOnly() || output.isEmpty() || rate.num <= 0
        || rate.den <= 0)
        return 0;
    // YouTube (SDR): 1080p30 8 Mbit/s, 1080p60 12, 4K30 35-45, 4K60 53-68 -> bits per pixel, high frame rates
    // need fewer per frame
    const double fps = rate.fps();
    const double bitsPerPixel = fps > 31 ? 0.125 : 0.165;
    return int(std::lround(double(output.width()) * output.height() * fps * bitsPerPixel / 1000.0));
}

QString RenderSettings::qualityLabel() const
{
    static const char* names[] = {N_("Hoch"), N_("Mittel"), N_("Klein"), N_("Web (begrenzte Bitrate)")};
    return T(names[std::clamp(quality, 0, kQualityWeb)]);
}

QString RenderSettings::summary(QSize output) const
{
    const RenderFormatInfo& f = renderFormat(format);
    QStringList parts{T(f.label)};
    if (!audioOnly()) parts << resolutionLabel(output.width(), output.height());
    if (f.hasQuality()) parts << (quality == kQualityWeb && !f.webQuality ? T("Hoch") : qualityLabel());
    if (hasEncoderChoice() && encoder != VideoEncoder::Auto && encoder != VideoEncoder::Software)
        parts << videoEncoderLabel(encoder);
    if (!audioOnly() && rateNum > 0) parts << QString("%1 fps").arg(FrameRate{rateNum, rateDen}.label());
    if (QLatin1String(f.audioCodec) == QLatin1String("aac")) parts << "AAC " + T("%1 kbit/s").arg(audioBitrateK);
    else if (QLatin1String(f.audioCodec) == QLatin1String("libopus") && !audioOnly())
        parts << "Opus " + T("%1 kbit/s").arg(audioBitrateK);
    else if (hasAudioBitrate()) parts << T("%1 kbit/s").arg(audioBitrateK);
    if (const int bits = validBits(*this)) parts << (bits == 32 ? T("32 Bit Float") : T("%1 Bit").arg(bits));
    if (subtitles == BurnSubtitles && !audioOnly()) parts << T("Untertitel eingebrannt");
    if (subtitles == SrtFile) parts << T("Untertitel als SRT");
    return parts.join(QStringLiteral(" · "));
}

QJsonObject RenderSettings::toJson() const
{
    QJsonObject o{{"format", format}, {"quality", quality}, {"audioBitrate", audioBitrateK}};
    if (shortSide > 0) o["shortSide"] = shortSide;
    if (audioBits != 24) o["audioBits"] = audioBits;
    if (rateNum > 0) o["rate"] = QString("%1/%2").arg(rateNum).arg(rateDen);
    if (subtitles == BurnSubtitles) o["subtitles"] = "burn";
    if (subtitles == SrtFile) o["subtitles"] = "srt";
    o["encoder"] = videoEncoderId(encoder);
    if (!smartRender) o["smartRender"] = false;
    if (size.isValid() && !size.isEmpty()) {
        o["width"] = size.width();
        o["height"] = size.height();
    }
    return o;
}

RenderSettings RenderSettings::fromJson(const QJsonObject& o)
{
    RenderSettings s;
    s.format = renderFormat(o.value("format").toString()).id;
    s.quality = std::clamp(o.value("quality").toInt(0), 0, kQualityWeb);
    s.audioBitrateK = std::clamp(o.value("audioBitrate").toInt(320), 32, 512);
    s.shortSide = std::max(0, o.value("shortSide").toInt());
    s.audioBits = o.value("audioBits").toInt(24);
    const QStringList rate = o.value("rate").toString().split('/');
    if (rate.size() == 2 && rate[0].toInt() > 0 && rate[1].toInt() > 0) {
        s.rateNum = rate[0].toInt();
        s.rateDen = rate[1].toInt();
    }
    const int w = o.value("width").toInt(), h = o.value("height").toInt();
    if (w > 0 && h > 0) s.size = QSize(w, h);
    const QString sub = o.value("subtitles").toString();
    s.subtitles = sub == "burn" ? BurnSubtitles : sub == "srt" ? SrtFile : NoSubtitles;
    // Older files know only the software encoder: keep it, so nothing changes for them
    s.encoder = videoEncoderFromId(o.value("encoder").toString(), VideoEncoder::Software);
    s.smartRender = o.value("smartRender").toBool(true);
    return s;
}

bool RenderSettings::operator==(const RenderSettings& o) const
{
    const auto normSize = [](QSize s) { return s.isValid() && !s.isEmpty() ? s : QSize(); };
    return format == o.format && shortSide == o.shortSide && normSize(size) == normSize(o.size)
           && quality == o.quality && audioBitrateK == o.audioBitrateK && subtitles == o.subtitles
           && audioBits == o.audioBits && encoder == o.encoder && smartRender == o.smartRender && rateNum == o.rateNum && (rateNum <= 0 || rateDen == o.rateDen);
}

// --- Presets

namespace RenderPresets {

QVector<RenderPreset> builtins()
{
    auto make = [](const QString& name, const QString& format, int shortSide, int quality, QSize size = {}) {
        RenderPreset p;
        p.name = name;
        p.builtin = true;
        p.settings.format = format;
        p.settings.shortSide = shortSide;
        p.settings.quality = quality;
        p.settings.size = size;
        return p;
    };
    // GIF: small and at 15 fps, otherwise the file gets huge
    RenderPreset gif = make(T("Animiertes GIF (480p, 15 fps)"), "gif", 480, 0);
    gif.settings.rateNum = 15;
    gif.settings.rateDen = 1;
    return {
        make("YouTube 1080p", "h264", 1080, RenderSettings::kQualityWeb),
        make("YouTube 4K", "h264", 2160, RenderSettings::kQualityWeb),
        make(T("Handy / Hochformat 9:16"), "h264", 0, RenderSettings::kQualityWeb, QSize(1080, 1920)),
        make(T("Klein (zum Verschicken)"), "h264", 720, 2),
        make(T("H.265 (kleine Datei, hohe Qualität)"), "h265", 0, 0),
        make(T("AV1 (sehr kleine Datei)"), "av1", 0, 0),
        make(T("Hohe Qualität / Archiv (ProRes 422 HQ)"), "prores", 0, 0),
        make(T("Schnitt-Austausch (DNxHR HQ)"), "dnxhr-hq", 0, 0),
        gif,
        make(T("Nur Audio (WAV)"), "wav", 0, 0),
        make(T("Nur Audio (FLAC, verlustfrei)"), "flac", 0, 0),
        make(T("Nur Audio (AAC)"), "aac", 0, 0),
        make(T("Nur Audio (MP3)"), "mp3", 0, 0),
    };
}

QString userFile()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)).filePath("render-presets.json");
}

QVector<RenderPreset> loadUser()
{
    QFile f(userFile());
    if (!f.open(QIODevice::ReadOnly)) return {};
    QVector<RenderPreset> list;
    for (const QJsonValue& v : QJsonDocument::fromJson(f.readAll()).object().value("presets").toArray()) {
        const QJsonObject o = v.toObject();
        RenderPreset p;
        p.name = o.value("name").toString().trimmed();
        if (p.name.isEmpty()) continue;
        p.settings = RenderSettings::fromJson(o.value("settings").toObject());
        list << p;
    }
    return list;
}

bool saveUser(const QVector<RenderPreset>& presets)
{
    QJsonArray arr;
    for (const RenderPreset& p : presets)
        if (!p.builtin) arr << QJsonObject{{"name", p.name}, {"settings", p.settings.toJson()}};
    QDir().mkpath(QFileInfo(userFile()).absolutePath());
    QSaveFile f(userFile());
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(QJsonObject{{"presets", arr}}).toJson(QJsonDocument::Indented));
    return f.commit();
}

QVector<RenderPreset> all()
{
    return builtins() + loadUser();
}

} // namespace RenderPresets

// --- Queue

bool RenderJob::operator==(const RenderJob& o) const
{
    return id == o.id && path == o.path && preset == o.preset && settings == o.settings && size == o.size
           && inOut == o.inOut && sequence == o.sequence && from == o.from && to == o.to && status == o.status && message == o.message
           && renderMs == o.renderMs && encoderUsed == o.encoderUsed;
}

namespace {
const char* kStatusIds[] = {"queued", "rendering", "done", "failed", "canceled"};
}

namespace RenderQueueJson {

QJsonArray toJson(const QVector<RenderJob>& jobs)
{
    QJsonArray arr;
    for (const RenderJob& j : jobs) {
        QJsonObject o{{"id", j.id}, {"path", j.path}, {"settings", j.settings.toJson()},
                      {"status", kStatusIds[int(j.status)]}};
        if (!j.preset.isEmpty()) o["preset"] = j.preset;
        if (j.size.isValid() && !j.size.isEmpty()) {
            o["width"] = j.size.width();
            o["height"] = j.size.height();
        }
        if (j.sequence) o["sequence"] = j.sequence;
        if (j.inOut) {
            o["inOut"] = true;
            o["from"] = j.from;
            o["to"] = j.to;
        }
        if (!j.message.isEmpty()) o["message"] = j.message;
        if (j.renderMs > 0) o["renderMs"] = double(j.renderMs);
        if (!j.encoderUsed.isEmpty()) o["encoderUsed"] = j.encoderUsed;
        arr << o;
    }
    return arr;
}

QVector<RenderJob> fromJson(const QJsonArray& arr)
{
    QVector<RenderJob> jobs;
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        RenderJob j;
        j.id = o.value("id").toInt();
        j.path = o.value("path").toString();
        if (j.path.isEmpty()) continue;
        if (j.id <= 0 || std::any_of(jobs.cbegin(), jobs.cend(), [&](const RenderJob& x) { return x.id == j.id; })) {
            int maxId = 0;
            for (const RenderJob& x : jobs) maxId = std::max(maxId, x.id);
            j.id = maxId + 1;
        }
        j.preset = o.value("preset").toString();
        j.settings = RenderSettings::fromJson(o.value("settings").toObject());
        const int w = o.value("width").toInt(), h = o.value("height").toInt();
        if (w > 0 && h > 0 && !j.settings.audioOnly()) j.size = QSize(w, h);
        j.sequence = std::max(0, o.value("sequence").toInt());
        j.inOut = o.value("inOut").toBool();
        if (j.inOut) {
            j.from = std::max(0, o.value("from").toInt());
            j.to = o.value("to").toInt(-1);
        }
        const QString st = o.value("status").toString();
        for (int i = 0; i < 5; ++i)
            if (st == QLatin1String(kStatusIds[i])) j.status = RenderStatus(i);
        if (j.status == RenderStatus::Rendering) j.status = RenderStatus::Queued;
        j.message = o.value("message").toString();
        j.renderMs = qint64(o.value("renderMs").toDouble());
        j.encoderUsed = o.value("encoderUsed").toString();
        jobs << j;
    }
    return jobs;
}

} // namespace RenderQueueJson

QString renderStatusText(const RenderJob& job)
{
    switch (job.status) {
    case RenderStatus::Queued: return T("Wartet");
    case RenderStatus::Rendering: return T("Rendert…");
    case RenderStatus::Done: {
        const qint64 s = (job.renderMs + 500) / 1000;
        return T("Fertig (%1)").arg(QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0')));
    }
    case RenderStatus::Failed: return job.message.isEmpty() ? T("Fehler") : T("Fehler: %1").arg(job.message);
    case RenderStatus::Canceled: return T("Abgebrochen");
    }
    return {};
}

QVector<RenderJob> moveRenderJobs(const QVector<RenderJob>& jobs, const QVector<int>& ids, int beforeId, int fixedId)
{
    QVector<RenderJob> moved, rest;
    int target = -1;         // index in rest to insert at (-1 = the end)
    bool reached = false;    // the drop target has been passed (a moved target: the next job that stays)
    for (const RenderJob& j : jobs) {
        if (j.id == beforeId) reached = true;
        if (ids.contains(j.id) && j.id != fixedId) {
            moved << j;
            continue;
        }
        if (reached && target < 0) target = int(rest.size());
        rest << j;
    }
    if (moved.isEmpty()) return jobs;
    if (target < 0) target = int(rest.size());
    for (int i = 0; i < moved.size(); ++i) rest.insert(target + i, moved[i]);
    return rest;
}

QString renderFileBaseName(const QString& name)
{
    QString s;
    for (const QChar c : name) s += c.unicode() < 0x20 || QStringLiteral("<>:\"/\\|?*").contains(c) ? QChar('_') : c;
    s = s.trimmed();
    if (s.size() > 200) s = s.left(200);
    // Windows drops a dot or space at the end (another name than shown)
    while (s.endsWith('.') || s.endsWith(' ')) s.chop(1);
    if (s.isEmpty()) return QStringLiteral("Export");
    // Windows device names are no files there, also with an extension ("NUL.mp4")
    const QString stem = s.section('.', 0, 0);
    const QString upper = stem.trimmed().toUpper();
    const bool device = QStringList{"CON", "PRN", "AUX", "NUL"}.contains(upper)
                        || (upper.size() == 4 && (upper.startsWith("COM") || upper.startsWith("LPT")) && upper[3] >= '1'
                            && upper[3] <= '9');
    if (device) s.insert(stem.size(), '_');
    return s;
}
