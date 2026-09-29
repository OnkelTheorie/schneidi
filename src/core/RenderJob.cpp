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
    static const QVector<RenderFormatInfo> formats{
        {"h264", "H.264 (MP4)", "mp4", "libx264", "aac", "yuv420p", true},
        {"h265", "H.265 (MP4)", "mp4", "libx265", "aac", "yuv420p", true},
        {"prores", "ProRes 422 HQ (MOV)", "mov", "prores_ks", "pcm_s24le", "yuv422p10le", false},
        {"aac", N_("Nur Audio – AAC (M4A)"), "m4a", "", "aac", "", false},
        {"wav", N_("Nur Audio – WAV (24 Bit)"), "wav", "", "pcm_s24le", "", false},
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

bool RenderSettings::audioOnly() const { return !*renderFormat(format).videoCodec; }

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
    static const int crfs[] = {18, 22, 28};
    return crfs[std::clamp(quality, 0, 2)];
}

QString RenderSettings::qualityLabel() const
{
    static const char* names[] = {N_("Hoch"), N_("Mittel"), N_("Klein")};
    return T(names[std::clamp(quality, 0, 2)]);
}

QString RenderSettings::summary(QSize output) const
{
    const RenderFormatInfo& f = renderFormat(format);
    QStringList parts{T(f.label)};
    if (!audioOnly()) parts << resolutionLabel(output.width(), output.height());
    if (f.hasQuality) parts << qualityLabel();
    if (QLatin1String(f.audioCodec) == QLatin1String("aac")) parts << QString("AAC %1 kbit/s").arg(audioBitrateK);
    return parts.join(QStringLiteral(" · "));
}

QJsonObject RenderSettings::toJson() const
{
    QJsonObject o{{"format", format}, {"quality", quality}, {"audioBitrate", audioBitrateK}};
    if (shortSide > 0) o["shortSide"] = shortSide;
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
    s.quality = std::clamp(o.value("quality").toInt(0), 0, 2);
    s.audioBitrateK = std::clamp(o.value("audioBitrate").toInt(320), 32, 512);
    s.shortSide = std::max(0, o.value("shortSide").toInt());
    const int w = o.value("width").toInt(), h = o.value("height").toInt();
    if (w > 0 && h > 0) s.size = QSize(w, h);
    return s;
}

bool RenderSettings::operator==(const RenderSettings& o) const
{
    const auto normSize = [](QSize s) { return s.isValid() && !s.isEmpty() ? s : QSize(); };
    return format == o.format && shortSide == o.shortSide && normSize(size) == normSize(o.size)
           && quality == o.quality && audioBitrateK == o.audioBitrateK;
}

// --- Vorlagen

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
    return {
        make("YouTube 1080p", "h264", 1080, 0),
        make("YouTube 4K", "h264", 2160, 0),
        make(T("Handy / Hochformat 9:16"), "h264", 0, 0, QSize(1080, 1920)),
        make(T("Klein (zum Verschicken)"), "h264", 720, 2),
        make(T("H.265 (kleine Datei, hohe Qualität)"), "h265", 0, 0),
        make(T("Hohe Qualität / Archiv (ProRes 422 HQ)"), "prores", 0, 0),
        make(T("Nur Audio (WAV)"), "wav", 0, 0),
        make(T("Nur Audio (AAC)"), "aac", 0, 0),
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

// --- Warteschlange

bool RenderJob::operator==(const RenderJob& o) const
{
    return id == o.id && path == o.path && preset == o.preset && settings == o.settings && size == o.size
           && inOut == o.inOut && from == o.from && to == o.to && status == o.status && message == o.message
           && renderMs == o.renderMs;
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
        if (j.inOut) {
            o["inOut"] = true;
            o["from"] = j.from;
            o["to"] = j.to;
        }
        if (!j.message.isEmpty()) o["message"] = j.message;
        if (j.renderMs > 0) o["renderMs"] = double(j.renderMs);
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
