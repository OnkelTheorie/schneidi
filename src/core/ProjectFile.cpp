#include "core/ProjectFile.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {

constexpr int kFormatVersion = 1;

QJsonObject transformToJson(const ClipTransform& t)
{
    return {
        {"zoomX", t.zoomX},         {"zoomY", t.zoomY},         {"posX", t.posX},
        {"posY", t.posY},           {"rotation", t.rotation},   {"cropLeft", t.cropLeft},
        {"cropRight", t.cropRight}, {"cropTop", t.cropTop},     {"cropBottom", t.cropBottom},
        {"opacity", t.opacity},     {"transformOn", t.transformOn}, {"cropOn", t.cropOn},
        {"compositeOn", t.compositeOn},
    };
}

ClipTransform transformFromJson(const QJsonObject& o)
{
    ClipTransform t;
    t.zoomX = o.value("zoomX").toDouble(t.zoomX);
    t.zoomY = o.value("zoomY").toDouble(t.zoomY);
    t.posX = o.value("posX").toDouble(t.posX);
    t.posY = o.value("posY").toDouble(t.posY);
    t.rotation = o.value("rotation").toDouble(t.rotation);
    t.cropLeft = o.value("cropLeft").toDouble(t.cropLeft);
    t.cropRight = o.value("cropRight").toDouble(t.cropRight);
    t.cropTop = o.value("cropTop").toDouble(t.cropTop);
    t.cropBottom = o.value("cropBottom").toDouble(t.cropBottom);
    t.opacity = o.value("opacity").toDouble(t.opacity);
    t.transformOn = o.value("transformOn").toBool(t.transformOn);
    t.cropOn = o.value("cropOn").toBool(t.cropOn);
    t.compositeOn = o.value("compositeOn").toBool(t.compositeOn);
    return t;
}

// Clips verweisen per Index auf die Medienliste -> Pfad steht nur einmal in der Datei
QJsonObject clipToJson(const Clip& c, int mediaIndex)
{
    QJsonObject o{{"id", c.id}, {"media", mediaIndex}, {"start", c.start}, {"in", c.in}, {"out", c.out}};
    if (c.linkId) o["link"] = c.linkId;
    if (c.volumeDb != 0.0) o["volumeDb"] = c.volumeDb;
    if (c.pan != 0.0) o["pan"] = c.pan;
    if (!c.enabled) o["enabled"] = false;
    if (c.transIn > 0) o["transIn"] = c.transIn; // Übergänge (Frames); fehlend = keiner
    if (c.transOut > 0) o["transOut"] = c.transOut;
    if (!c.transform.isIdentity() || !c.transform.transformOn || !c.transform.cropOn || !c.transform.compositeOn)
        o["transform"] = transformToJson(c.transform);
    if (!c.effects.isEmpty()) {
        QJsonArray fx;
        for (const EffectInstance& e : c.effects)
            fx << QJsonObject{{"id", e.effectId}, {"enabled", e.enabled}, {"params", QJsonObject::fromVariantMap(e.params)}};
        o["effects"] = fx;
    }
    return o;
}

Clip clipFromJson(const QJsonObject& o, const QVector<MediaInfo>& media)
{
    Clip c;
    c.id = o.value("id").toInt();
    c.mediaPath = media.value(o.value("media").toInt(-1)).path;
    c.start = o.value("start").toInt();
    c.in = o.value("in").toInt();
    c.out = o.value("out").toInt();
    c.linkId = o.value("link").toInt();
    c.volumeDb = o.value("volumeDb").toDouble(0.0);
    c.pan = o.value("pan").toDouble(0.0);
    c.enabled = o.value("enabled").toBool(true);
    c.transIn = std::max(0, o.value("transIn").toInt());
    c.transOut = std::max(0, o.value("transOut").toInt());
    if (o.contains("transform")) c.transform = transformFromJson(o.value("transform").toObject());
    for (const QJsonValue& v : o.value("effects").toArray()) {
        const QJsonObject e = v.toObject();
        c.effects << EffectInstance{e.value("id").toString(), e.value("params").toObject().toVariantMap(),
                                    e.value("enabled").toBool(true)};
    }
    return c;
}

QString resolvePath(const QJsonObject& m, const QDir& projectDir)
{
    const QString abs = m.value("path").toString();
    if (QFileInfo::exists(abs)) return abs;
    const QString rel = m.value("relPath").toString();
    if (!rel.isEmpty()) {
        const QString candidate = QDir::cleanPath(projectDir.absoluteFilePath(rel));
        if (QFileInfo::exists(candidate)) return candidate;
    }
    return abs; // bleibt offline, bis neu verknüpft
}

} // namespace

namespace ProjectFile {

QByteArray toJson(const ProjectData& data, const QString& projectPath)
{
    const QDir projectDir = QFileInfo(projectPath).absoluteDir();
    QHash<QString, int> index;
    QJsonArray media;
    for (const MediaInfo& m : data.media) {
        index[m.path] = media.size();
        media << QJsonObject{{"path", m.path}, {"relPath", projectDir.relativeFilePath(m.path)},
                             {"name", m.name}, {"length", m.length}, {"hasVideo", m.hasVideo},
                             {"hasAudio", m.hasAudio}, {"isImage", m.isImage}};
    }

    auto tracks = [&](const QVector<Track>& list) {
        QJsonArray arr;
        for (const Track& t : list) {
            QJsonArray clips;
            for (const Clip& c : t.clips) {
                if (!index.contains(c.mediaPath)) { // sollte nicht vorkommen, aber nichts verlieren
                    index[c.mediaPath] = media.size();
                    media << QJsonObject{{"path", c.mediaPath}, {"relPath", projectDir.relativeFilePath(c.mediaPath)},
                                         {"name", QFileInfo(c.mediaPath).fileName()}};
                }
                clips << clipToJson(c, index.value(c.mediaPath));
            }
            QJsonObject o{{"name", t.name}, {"clips", clips}};
            if (t.muted) o["muted"] = true;
            if (t.hidden) o["hidden"] = true;
            arr << o;
        }
        return arr;
    };
    const QJsonArray video = tracks(data.timeline.video);
    const QJsonArray audio = tracks(data.timeline.audio);

    QJsonArray markers;
    for (int m : data.timeline.markers) markers << m;

    const QJsonObject root{
        {"app", "schneidi"},       {"version", kFormatVersion}, {"fps", data.fps},
        {"playhead", data.playhead}, {"lastClipId", data.lastClipId}, {"lastLinkId", data.lastLinkId},
        {"media", media},
        {"timeline", QJsonObject{{"video", video}, {"audio", audio}, {"markers", markers},
                                 {"markIn", data.timeline.markIn}, {"markOut", data.timeline.markOut}}},
    };
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool fromJson(const QByteArray& json, const QString& projectPath, ProjectData* data, QString* error)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    const QJsonObject root = doc.object();
    if (pe.error != QJsonParseError::NoError || root.value("app").toString() != "schneidi") {
        if (error) *error = "Keine gültige schneidi-Projektdatei.";
        return false;
    }
    if (root.value("version").toInt() > kFormatVersion) {
        if (error) *error = "Die Projektdatei stammt aus einer neueren schneidi-Version.";
        return false;
    }

    const QDir projectDir = QFileInfo(projectPath).absoluteDir();
    ProjectData d;
    d.fps = root.value("fps").toInt(25);
    d.playhead = root.value("playhead").toInt();
    d.lastClipId = root.value("lastClipId").toInt();
    d.lastLinkId = root.value("lastLinkId").toInt();
    for (const QJsonValue& v : root.value("media").toArray()) {
        const QJsonObject o = v.toObject();
        MediaInfo m;
        m.path = resolvePath(o, projectDir);
        m.name = o.value("name").toString(QFileInfo(m.path).fileName());
        m.length = o.value("length").toInt();
        m.hasVideo = o.value("hasVideo").toBool();
        m.hasAudio = o.value("hasAudio").toBool();
        m.isImage = o.value("isImage").toBool();
        d.media << m;
    }

    const QJsonObject tl = root.value("timeline").toObject();
    auto tracks = [&](const QJsonArray& arr, TrackKind kind) {
        QVector<Track> list;
        for (const QJsonValue& v : arr) {
            const QJsonObject o = v.toObject();
            Track t;
            t.kind = kind;
            t.name = o.value("name").toString();
            t.muted = o.value("muted").toBool();
            t.hidden = o.value("hidden").toBool();
            for (const QJsonValue& cv : o.value("clips").toArray()) {
                Clip c = clipFromJson(cv.toObject(), d.media);
                d.lastClipId = std::max(d.lastClipId, c.id);
                d.lastLinkId = std::max(d.lastLinkId, c.linkId);
                t.clips << c;
            }
            std::sort(t.clips.begin(), t.clips.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
            list << t;
        }
        return list;
    };
    d.timeline.video = tracks(tl.value("video").toArray(), TrackKind::Video);
    d.timeline.audio = tracks(tl.value("audio").toArray(), TrackKind::Audio);
    for (const QJsonValue& v : tl.value("markers").toArray()) d.timeline.markers << v.toInt();
    std::sort(d.timeline.markers.begin(), d.timeline.markers.end());
    d.timeline.markIn = tl.value("markIn").toInt(-1);
    d.timeline.markOut = tl.value("markOut").toInt(-1);

    *data = d;
    return true;
}

bool save(const ProjectData& data, const QString& path, QString* error)
{
    // QSaveFile: erst vollständig schreiben, dann umbenennen -> nie eine halbe Projektdatei
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(toJson(data, path)) < 0 || !f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

bool load(const QString& path, ProjectData* data, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    return fromJson(f.readAll(), path, data, error);
}

QStringList missingMedia(const ProjectData& data)
{
    QStringList missing;
    for (const MediaInfo& m : data.media)
        if (!QFileInfo::exists(m.path)) missing << m.path;
    return missing;
}

int relink(ProjectData* data, const QString& searchDir)
{
    QHash<QString, QString> wanted; // Dateiname -> alter Pfad
    for (const QString& p : missingMedia(*data)) wanted.insert(QFileInfo(p).fileName(), p);
    if (wanted.isEmpty()) return 0;

    QHash<QString, QString> moved; // alter Pfad -> neuer Pfad
    QDirIterator it(searchDir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext() && moved.size() < wanted.size()) {
        const QString path = it.next();
        const QString old = wanted.value(it.fileName());
        if (!old.isEmpty() && !moved.contains(old)) moved.insert(old, path);
    }

    for (MediaInfo& m : data->media)
        if (moved.contains(m.path)) m.path = moved.value(m.path);
    for (QVector<Track>* list : {&data->timeline.video, &data->timeline.audio})
        for (Track& t : *list)
            for (Clip& c : t.clips)
                if (moved.contains(c.mediaPath)) c.mediaPath = moved.value(c.mediaPath);
    return moved.size();
}

} // namespace ProjectFile
