#include "core/ProjectFile.h"

#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/Retime.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <algorithm>

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

QJsonObject titleToJson(const TitleStyle& t)
{
    return {
        {"text", t.text},     {"font", t.font},       {"size", t.size},
        {"color", t.color.name(QColor::HexArgb)},     {"bold", t.bold},
        {"italic", t.italic}, {"align", t.align},     {"posX", t.posX},
        {"posY", t.posY},     {"outlineOn", t.outlineOn},
        {"outlineColor", t.outlineColor.name(QColor::HexArgb)}, {"outlineWidth", t.outlineWidth},
        {"boxOn", t.boxOn},   {"boxColor", t.boxColor.name(QColor::HexArgb)}, {"boxPad", t.boxPad},
    };
}

TitleStyle titleFromJson(const QJsonObject& o)
{
    TitleStyle t;
    auto color = [&](const char* key, const QColor& def) {
        const QColor c(o.value(key).toString());
        return c.isValid() ? c : def;
    };
    t.text = o.value("text").toString(t.text);
    t.font = o.value("font").toString(t.font);
    t.size = o.value("size").toDouble(t.size);
    t.color = color("color", t.color);
    t.bold = o.value("bold").toBool(t.bold);
    t.italic = o.value("italic").toBool(t.italic);
    t.align = std::clamp(o.value("align").toInt(t.align), 0, 2);
    t.posX = o.value("posX").toDouble(t.posX);
    t.posY = o.value("posY").toDouble(t.posY);
    t.outlineOn = o.value("outlineOn").toBool(t.outlineOn);
    t.outlineColor = color("outlineColor", t.outlineColor);
    t.outlineWidth = o.value("outlineWidth").toDouble(t.outlineWidth);
    t.boxOn = o.value("boxOn").toBool(t.boxOn);
    t.boxColor = color("boxColor", t.boxColor);
    t.boxPad = o.value("boxPad").toDouble(t.boxPad);
    return t;
}

QJsonObject transitionStyleToJson(const TransitionStyle& s)
{
    static const char* aligns[] = {"center", "start", "end"};
    QJsonObject o{{"type", transitionTypeInfo(s.type).id}, {"align", aligns[int(s.align)]}};
    const TransitionStyle d;
    if (s.color != d.color) o["color"] = s.color.name(QColor::HexRgb);
    if (s.softness != d.softness) o["softness"] = s.softness;
    if (s.border != d.border) o["border"] = s.border;
    if (s.borderColor != d.borderColor) o["borderColor"] = s.borderColor.name(QColor::HexRgb);
    if (s.audio != d.audio) o["audioCurve"] = audioCurveInfo(s.audio).id;
    return o;
}

TransitionStyle transitionStyleFromJson(const QJsonObject& o)
{
    TransitionStyle s;
    auto color = [&](const char* key, const QColor& def) {
        const QColor c(o.value(key).toString());
        return c.isValid() ? c : def;
    };
    const QString type = o.value("type").toString();
    for (const auto& i : kTransitionTypes)
        if (type == i.id) s.type = i.type;
    s.color = color("color", s.color);
    // ältere Dateien: Abblende über Schwarz/Weiß als eigene Arten
    if (type == "dip_black" || type == "dip_white") {
        s.type = TransitionType::DipToColor;
        s.color = type == "dip_black" ? Qt::black : Qt::white;
    }
    const QString align = o.value("align").toString();
    if (align == "start") s.align = TransitionAlign::Start;
    else if (align == "end") s.align = TransitionAlign::End;
    s.softness = std::clamp(o.value("softness").toDouble(s.softness), 0.0, 100.0);
    s.border = std::max(0.0, o.value("border").toDouble(s.border));
    s.borderColor = color("borderColor", s.borderColor);
    const QString curve = o.value("audioCurve").toString();
    for (const auto& i : kAudioCurves)
        if (curve == i.id) s.audio = i.curve;
    return s;
}

// Keyframes: {"zoomX": [[Quell-Frame, Wert], [Frame, Wert, "easeIn"], …], …}; Verlauf nur, wenn nicht linear.
// Bezier: [Frame, Wert, "bezier", ein dt, ein dv, aus dt, aus dv] (ältere Versionen lesen das als linear)
const char* const kEaseIds[] = {"linear", "easeIn", "easeOut", "easeInOut", "bezier"}; // Index = KeyEase

QJsonObject keysToJson(const Clip& c)
{
    QJsonObject o;
    for (auto it = c.keys.cbegin(); it != c.keys.cend(); ++it) {
        if (it->isEmpty()) continue;
        QJsonArray list;
        for (const Keyframe& k : *it) {
            QJsonArray e{k.frame, k.value};
            if (k.ease != KeyEase::Linear) e << kEaseIds[int(k.ease)];
            if (k.ease == KeyEase::Bezier) e << k.inDt << k.inDv << k.outDt << k.outDv;
            list << e;
        }
        o[Keys::info(it.key()).id] = list;
    }
    return o;
}

void keysFromJson(Clip& c, const QJsonObject& o)
{
    for (auto it = o.begin(); it != o.end(); ++it) {
        AnimParam p;
        if (!Keys::fromId(it.key(), &p)) continue; // unbekannt (neuere Version) -> ignorieren
        KeyTrack track;
        for (const QJsonValue& v : it.value().toArray()) {
            const QJsonArray e = v.toArray();
            if (e.size() < 2) continue;
            Keyframe k{e.at(0).toInt(), e.at(1).toDouble(), KeyEase::Linear};
            const QString ease = e.at(2).toString();
            for (int i = 0; i < 5; ++i)
                if (ease == QLatin1String(kEaseIds[i])) k.ease = KeyEase(i);
            if (k.ease == KeyEase::Bezier) {
                k.inDt = std::min(0.0, e.at(3).toDouble());
                k.inDv = e.at(4).toDouble();
                k.outDt = std::max(0.0, e.at(5).toDouble());
                k.outDv = e.at(6).toDouble();
            }
            track << k;
        }
        std::sort(track.begin(), track.end(), [](const Keyframe& a, const Keyframe& b) { return a.frame < b.frame; });
        if (!track.isEmpty()) c.keys.insert(p, track);
    }
}

// Pfad-Parameter eines Effekts (z. B. LUT der Color-Seite) relativ zur Projektdatei
QJsonObject pathParamsRelative(const EffectInstance& e, const QDir& projectDir)
{
    QJsonObject rel;
    if (const EffectDescriptor* d = EffectRegistry::find(e.effectId))
        for (const EffectParam& p : d->params) {
            const QString path = e.params.value(p.key).toString();
            if (p.type == EffectParam::Path && !path.isEmpty()) rel[p.key] = projectDir.relativeFilePath(path);
        }
    return rel;
}

// Wie bei Medien: absoluter Pfad fehlt -> relativ zur Projektdatei versuchen (Ordner verschoben)
void resolvePathParams(EffectInstance& e, const QJsonObject& rel, const QDir& projectDir)
{
    for (auto it = rel.begin(); it != rel.end(); ++it) {
        const QString abs = e.params.value(it.key()).toString();
        if (abs.isEmpty() || QFileInfo::exists(abs)) continue;
        const QString candidate = QDir::cleanPath(projectDir.absoluteFilePath(it.value().toString()));
        if (QFileInfo::exists(candidate)) e.params[it.key()] = candidate;
    }
}

// Clips verweisen per Index auf die Medienliste -> Pfad steht nur einmal in der Datei
QJsonObject clipToJson(const Clip& c, int mediaIndex, const QDir& projectDir)
{
    QJsonObject o{{"id", c.id}, {"start", c.start}, {"in", c.in}, {"out", c.out}};
    if (c.isTitle()) o["title"] = titleToJson(c.title); // Titel haben keinen Medienverweis
    else if (c.isCompound()) o["sequence"] = c.sequenceId; // Compound Clip: Verweis auf die Sequenz
    else o["media"] = mediaIndex;
    if (c.linkId) o["link"] = c.linkId;
    if (c.volumeDb != 0.0) o["volumeDb"] = c.volumeDb;
    if (c.pan != 0.0) o["pan"] = c.pan;
    if (!c.enabled) o["enabled"] = false;
    if (c.transIn > 0) o["transIn"] = c.transIn; // Übergänge (Frames); fehlend = keiner
    if (c.transOut > 0) o["transOut"] = c.transOut;
    if (c.transIn > 0 && c.transInStyle != TransitionStyle{}) o["transInStyle"] = transitionStyleToJson(c.transInStyle);
    if (c.transOut > 0 && c.transOutStyle != TransitionStyle{}) o["transOutStyle"] = transitionStyleToJson(c.transOutStyle);
    if (c.fadeIn > 0) o["fadeIn"] = c.fadeIn; // Fade-Griffe (Frames)
    if (c.fadeOut > 0) o["fadeOut"] = c.fadeOut;
    if (c.speed != 1.0) o["speed"] = c.speed;
    if (c.reverse) o["reverse"] = true;
    if (c.freeze) o["freeze"] = true;
    if (!c.keepPitch) o["keepPitch"] = false;
    if (!c.ramp.isEmpty()) { // Speed Ramp: [{source, speed, smooth}]
        QJsonArray ramp;
        for (const SpeedPoint& p : c.ramp) {
            QJsonObject j{{"source", p.source}, {"speed", p.speed}};
            if (p.smooth > 0) j["smooth"] = p.smooth;
            ramp.append(j);
        }
        o["ramp"] = ramp;
    }
    if (c.renderCache) o["renderCache"] = true;
    if (!c.transform.isIdentity() || !c.transform.transformOn || !c.transform.cropOn || !c.transform.compositeOn)
        o["transform"] = transformToJson(c.transform);
    if (!c.effects.isEmpty()) {
        QJsonArray fx;
        for (const EffectInstance& e : c.effects) {
            QJsonObject eo{{"id", e.effectId}, {"enabled", e.enabled}, {"params", QJsonObject::fromVariantMap(e.params)}};
            // Dateipfade (LUT) wie Medien zusätzlich relativ zur Projektdatei
            const QJsonObject rel = pathParamsRelative(e, projectDir);
            if (!rel.isEmpty()) eo["relPaths"] = rel;
            fx << eo;
        }
        o["effects"] = fx;
    }
    if (Keys::hasKeys(c)) o["keys"] = keysToJson(c);
    return o;
}

Clip clipFromJson(const QJsonObject& o, const QVector<MediaInfo>& media, const QDir& projectDir)
{
    Clip c;
    c.id = o.value("id").toInt();
    if (o.contains("title")) {
        c.kind = ClipKind::Title;
        c.title = titleFromJson(o.value("title").toObject());
    } else if (o.contains("sequence")) {
        c.kind = ClipKind::Compound;
        c.sequenceId = o.value("sequence").toInt();
    } else {
        c.mediaPath = media.value(o.value("media").toInt(-1)).path;
    }
    c.start = o.value("start").toInt();
    c.in = o.value("in").toInt();
    c.out = o.value("out").toInt();
    c.linkId = o.value("link").toInt();
    c.volumeDb = o.value("volumeDb").toDouble(0.0);
    c.pan = o.value("pan").toDouble(0.0);
    c.enabled = o.value("enabled").toBool(true);
    c.transIn = std::max(0, o.value("transIn").toInt());
    c.transOut = std::max(0, o.value("transOut").toInt());
    c.transInStyle = transitionStyleFromJson(o.value("transInStyle").toObject());
    c.transOutStyle = transitionStyleFromJson(o.value("transOutStyle").toObject());
    c.fadeIn = std::max(0, o.value("fadeIn").toInt());
    c.fadeOut = std::max(0, o.value("fadeOut").toInt());
    c.speed = o.value("speed").toDouble(1.0);
    if (c.speed <= 0) c.speed = 1.0;
    c.reverse = o.value("reverse").toBool();
    c.freeze = o.value("freeze").toBool();
    c.keepPitch = o.value("keepPitch").toBool(true);
    for (const QJsonValue& v : o.value("ramp").toArray()) {
        const QJsonObject j = v.toObject();
        SpeedPoint p;
        p.source = j.value("source").toDouble();
        p.speed = j.value("speed").toDouble(1.0);
        p.smooth = j.value("smooth").toInt();
        c.ramp << p;
    }
    Retime::normalize(c.ramp, 0); // kaputte Werte verwerfen (Dateilänge hier unbekannt)
    c.renderCache = o.value("renderCache").toBool();
    if (o.contains("transform")) c.transform = transformFromJson(o.value("transform").toObject());
    for (const QJsonValue& v : o.value("effects").toArray()) {
        const QJsonObject e = v.toObject();
        EffectInstance inst{e.value("id").toString(), e.value("params").toObject().toVariantMap(),
                            e.value("enabled").toBool(true)};
        resolvePathParams(inst, e.value("relPaths").toObject(), projectDir);
        c.effects << inst;
    }
    keysFromJson(c, o.value("keys").toObject()); // fehlt in älteren Dateien
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

QJsonObject clipJson(const Clip& c)
{
    QJsonObject o = clipToJson(c, -1, QDir::root()); // fester Bezug -> relPaths stabil (Cache-Schlüssel)
    if (!c.isTitle() && !c.isCompound()) o["media"] = c.mediaPath;
    return o;
}

QByteArray toJson(const ProjectData& data, const QString& projectPath)
{
    const QDir projectDir = QFileInfo(projectPath).absoluteDir();
    QHash<QString, int> index;
    QJsonArray media;
    for (const MediaInfo& m : data.media) {
        index[m.path] = media.size();
        QJsonObject o{{"path", m.path}, {"relPath", projectDir.relativeFilePath(m.path)},
                      {"name", m.name}, {"length", m.length}, {"hasVideo", m.hasVideo},
                      {"hasAudio", m.hasAudio}, {"isImage", m.isImage}};
        if (m.markIn >= 0) o["markIn"] = m.markIn; // Quell-In/Out (optional, alte Dateien ohne)
        if (m.markOut >= 0) o["markOut"] = m.markOut;
        if (m.bin != 0) o["bin"] = m.bin; // Media-Pool-Organisation (optional, alte Dateien ohne)
        if (!m.clipColor.isEmpty()) o["clipColor"] = m.clipColor;
        if (!m.flags.isEmpty()) o["flags"] = QJsonArray::fromStringList(m.flags);
        media << o;
    }

    auto tracks = [&](const QVector<Track>& list, TrackKind kind) {
        QJsonArray arr;
        for (const Track& t : list) {
            QJsonArray clips;
            for (const Clip& c : t.clips) {
                if (c.isTitle() || c.isCompound()) {
                    clips << clipToJson(c, -1, projectDir);
                    continue;
                }
                if (!index.contains(c.mediaPath)) { // sollte nicht vorkommen, aber nichts verlieren
                    index[c.mediaPath] = media.size();
                    media << QJsonObject{{"path", c.mediaPath}, {"relPath", projectDir.relativeFilePath(c.mediaPath)},
                                         {"name", QFileInfo(c.mediaPath).fileName()}, {"notInPool", true}};
                }
                clips << clipToJson(c, index.value(c.mediaPath), projectDir);
            }
            // "name" = Kürzel (ältere Versionen zeigen es an), eigener Name in "label"
            QJsonObject o{{"name", trackShortName({kind, int(arr.size())})}, {"clips", clips}};
            if (!t.name.isEmpty()) o["label"] = t.name;
            if (!t.color.isEmpty()) o["color"] = t.color;
            if (t.locked) o["locked"] = true;
            if (t.muted) o["muted"] = true;
            if (t.hidden) o["hidden"] = true;
            if (t.volumeDb != 0.0) o["volumeDb"] = t.volumeDb;
            if (t.pan != 0.0) o["pan"] = t.pan;
            if (t.solo) o["solo"] = true;
            arr << o;
        }
        return arr;
    };
    auto timelineJson = [&](const Timeline& timeline) {
        QJsonArray subtitles; // Untertitelspuren (optional, alte Dateien ohne)
        for (const SubtitleTrack& t : timeline.subtitles) {
            QJsonArray cues;
            for (const SubtitleCue& c : t.cues)
                cues << QJsonObject{{"id", c.id}, {"start", c.start}, {"end", c.end}, {"text", c.text}};
            QJsonObject style = titleToJson(t.style);
            style.remove("text");
            QJsonObject o{{"style", style}, {"cues", cues}};
            if (!t.name.isEmpty()) o["label"] = t.name;
            if (!t.enabled) o["enabled"] = false;
            if (t.locked) o["locked"] = true;
            subtitles << o;
        }
        QJsonArray markers;
        for (int m : timeline.markers) markers << m;
        QJsonObject o{{"video", tracks(timeline.video, TrackKind::Video)},
                      {"audio", tracks(timeline.audio, TrackKind::Audio)}, {"markers", markers},
                      {"markIn", timeline.markIn}, {"markOut", timeline.markOut},
                      {"masterVolumeDb", timeline.masterVolumeDb}, {"masterLimiter", timeline.masterLimiter},
                      {"masterLimiterDb", timeline.masterLimiterDb}};
        if (!subtitles.isEmpty()) o["subtitles"] = subtitles;
        return o;
    };

    // Erst alle Timelines schreiben: Clips mit Dateien außerhalb des Media Pools ergänzen dabei die Medienliste
    const QJsonObject current = timelineJson(data.timeline);
    // Weitere Timelines und Compound Clips (optional, alte Dateien ohne); die geöffnete steht in "timeline"
    QJsonArray seqs;
    for (const Sequence& s : data.sequences) {
        QJsonObject o{{"id", s.id}, {"name", s.name}};
        if (s.compound) o["compound"] = true;
        if (s.bin != 0) o["bin"] = s.bin;
        if (s.id != data.currentSequence) o["timeline"] = timelineJson(s.timeline);
        seqs << o;
    }
    QJsonArray bins;
    for (const MediaBin& b : data.bins) bins << QJsonObject{{"id", b.id}, {"parent", b.parent}, {"name", b.name}};

    QJsonObject root{
        {"app", "schneidi"},       {"version", kFormatVersion},
        {"fps", data.format.rate.timebase()}, // für ältere Versionen
        {"format", QJsonObject{{"width", data.format.width}, {"height", data.format.height},
                               {"fpsNum", data.format.rate.num}, {"fpsDen", data.format.rate.den}}},
        {"playhead", data.playhead}, {"lastClipId", data.lastClipId}, {"lastLinkId", data.lastLinkId},
        {"media", media},           {"bins", bins},
        {"timeline", current}, // geöffnete Timeline (ältere Versionen lesen nur diese)
    };
    if (!seqs.isEmpty()) {
        root["sequences"] = seqs;
        root["currentSequence"] = data.currentSequence;
    }
    if (!data.renderQueue.isEmpty()) root["renderQueue"] = RenderQueueJson::toJson(data.renderQueue);
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool fromJson(const QByteArray& json, const QString& projectPath, ProjectData* data, QString* error)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    const QJsonObject root = doc.object();
    if (pe.error != QJsonParseError::NoError || root.value("app").toString() != "schneidi") {
        if (error) *error = T("Keine gültige schneidi-Projektdatei.");
        return false;
    }
    if (root.value("version").toInt() > kFormatVersion) {
        if (error) *error = T("Die Projektdatei stammt aus einer neueren schneidi-Version.");
        return false;
    }

    const QDir projectDir = QFileInfo(projectPath).absoluteDir();
    ProjectData d;
    // Projekteinstellungen; ältere Dateien kennen nur "fps" (ganzzahlig) und waren immer 1920 × 1080
    const QJsonObject fmt = root.value("format").toObject();
    d.format.width = std::max(16, fmt.value("width").toInt(1920));
    d.format.height = std::max(16, fmt.value("height").toInt(1080));
    d.format.rate.num = std::max(1, fmt.value("fpsNum").toInt(root.value("fps").toInt(25)));
    d.format.rate.den = std::max(1, fmt.value("fpsDen").toInt(1));
    d.playhead = root.value("playhead").toInt();
    d.lastClipId = root.value("lastClipId").toInt();
    d.lastLinkId = root.value("lastLinkId").toInt();
    // Alle Einträge lösen die Clip-Verweise auf; "notInPool" (Datei nur in der Timeline) kommt nicht in den Media Pool
    QVector<MediaInfo> allMedia;
    for (const QJsonValue& v : root.value("media").toArray()) {
        const QJsonObject o = v.toObject();
        MediaInfo m;
        m.path = resolvePath(o, projectDir);
        m.name = o.value("name").toString(fileNameAnyOs(m.path));
        m.length = o.value("length").toInt();
        m.hasVideo = o.value("hasVideo").toBool();
        m.hasAudio = o.value("hasAudio").toBool();
        m.isImage = o.value("isImage").toBool();
        m.markIn = o.value("markIn").toInt(-1);
        m.markOut = o.value("markOut").toInt(-1);
        m.bin = std::max(0, o.value("bin").toInt()); // fehlende Bins setzt Project::load auf Master
        if (trackColorInfo(o.value("clipColor").toString())) m.clipColor = o.value("clipColor").toString();
        for (const QJsonValue& f : o.value("flags").toArray())
            if (flagColorInfo(f.toString()) && !m.flags.contains(f.toString())) m.flags << f.toString();
        allMedia << m;
        if (!o.value("notInPool").toBool()) d.media << m;
    }
    for (const QJsonValue& v : root.value("bins").toArray()) {
        const QJsonObject o = v.toObject();
        const MediaBin b{o.value("id").toInt(), std::max(0, o.value("parent").toInt()), o.value("name").toString()};
        if (b.id <= 0 || b.name.isEmpty()) continue;
        if (std::any_of(d.bins.cbegin(), d.bins.cend(), [&](const MediaBin& x) { return x.id == b.id; })) continue;
        d.bins << b;
    }

    auto tracks = [&](const QJsonArray& arr, TrackKind kind) {
        QVector<Track> list;
        for (const QJsonValue& v : arr) {
            const QJsonObject o = v.toObject();
            Track t;
            t.kind = kind;
            t.name = o.value("label").toString(); // ältere Dateien: nur Kürzel in "name" -> Standardname
            if (trackColorInfo(o.value("color").toString())) t.color = o.value("color").toString();
            t.locked = o.value("locked").toBool();
            t.muted = o.value("muted").toBool();
            t.hidden = o.value("hidden").toBool();
            t.volumeDb = o.value("volumeDb").toDouble(0.0);
            t.pan = o.value("pan").toDouble(0.0);
            t.solo = o.value("solo").toBool();
            for (const QJsonValue& cv : o.value("clips").toArray()) {
                Clip c = clipFromJson(cv.toObject(), allMedia, projectDir);
                d.lastClipId = std::max(d.lastClipId, c.id);
                d.lastLinkId = std::max(d.lastLinkId, c.linkId);
                t.clips << c;
            }
            std::sort(t.clips.begin(), t.clips.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
            list << t;
        }
        return list;
    };
    auto readTimeline = [&](const QJsonObject& tl) {
        Timeline timeline;
        timeline.video = tracks(tl.value("video").toArray(), TrackKind::Video);
        timeline.audio = tracks(tl.value("audio").toArray(), TrackKind::Audio);
        for (const QJsonValue& v : tl.value("subtitles").toArray()) {
            const QJsonObject o = v.toObject();
            SubtitleTrack t;
            t.name = o.value("label").toString();
            t.enabled = o.value("enabled").toBool(true);
            t.locked = o.value("locked").toBool();
            if (o.contains("style")) {
                QJsonObject style = o.value("style").toObject();
                style["text"] = QString(); // Stil ohne Text
                // fehlende Werte: Untertitel-Standard statt Titel-Standard
                const TitleStyle base = subtitleBaseStyle();
                const QJsonObject def = titleToJson(base);
                for (auto it = def.begin(); it != def.end(); ++it)
                    if (!style.contains(it.key())) style[it.key()] = it.value();
                t.style = titleFromJson(style);
            }
            for (const QJsonValue& cv : o.value("cues").toArray()) {
                const QJsonObject c = cv.toObject();
                SubtitleCue cue{c.value("id").toInt(), c.value("start").toInt(), c.value("end").toInt(),
                                c.value("text").toString()};
                if (cue.end <= cue.start) continue;
                t.cues << cue;
            }
            std::sort(t.cues.begin(), t.cues.end(), [](const SubtitleCue& a, const SubtitleCue& b) { return a.start < b.start; });
            timeline.subtitles << t;
        }
        // ids eindeutig über Clips und Untertitel; fehlende/doppelte (Hand-Änderungen) bekommen eine neue
        QSet<int> usedIds;
        for (const auto* list : {&timeline.video, &timeline.audio})
            for (const Track& t : *list)
                for (const Clip& c : t.clips) usedIds.insert(c.id);
        for (SubtitleTrack& t : timeline.subtitles)
            for (SubtitleCue& c : t.cues) {
                if (c.id <= 0 || usedIds.contains(c.id)) {
                    c.id = -1;
                    continue;
                }
                usedIds.insert(c.id);
                d.lastClipId = std::max(d.lastClipId, c.id);
            }
        for (SubtitleTrack& t : timeline.subtitles)
            for (SubtitleCue& c : t.cues)
                if (c.id < 0) c.id = ++d.lastClipId;
        for (const QJsonValue& v : tl.value("markers").toArray()) timeline.markers << v.toInt();
        timeline.masterVolumeDb = tl.value("masterVolumeDb").toDouble(0.0);
        timeline.masterLimiter = tl.value("masterLimiter").toBool(false);
        timeline.masterLimiterDb = std::clamp(tl.value("masterLimiterDb").toDouble(kDefaultLimiterDb), -24.0, 0.0);
        std::sort(timeline.markers.begin(), timeline.markers.end());
        timeline.markIn = tl.value("markIn").toInt(-1);
        timeline.markOut = tl.value("markOut").toInt(-1);
        return timeline;
    };
    d.timeline = readTimeline(root.value("timeline").toObject());
    for (const QJsonValue& v : root.value("sequences").toArray()) {
        const QJsonObject o = v.toObject();
        Sequence seq;
        seq.id = o.value("id").toInt();
        seq.name = o.value("name").toString();
        seq.compound = o.value("compound").toBool();
        seq.bin = std::max(0, o.value("bin").toInt());
        if (seq.id <= 0) continue;
        if (o.contains("timeline")) seq.timeline = readTimeline(o.value("timeline").toObject());
        d.sequences << seq;
    }
    d.currentSequence = root.value("currentSequence").toInt();
    d.renderQueue = RenderQueueJson::fromJson(root.value("renderQueue").toArray());

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
    for (const QString& p : missingMedia(*data)) wanted.insert(fileNameAnyOs(p), p);
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
    auto relinkTimeline = [&](Timeline& tl) {
        for (QVector<Track>* list : {&tl.video, &tl.audio})
            for (Track& t : *list)
                for (Clip& c : t.clips)
                    if (moved.contains(c.mediaPath)) c.mediaPath = moved.value(c.mediaPath);
    };
    relinkTimeline(data->timeline);
    for (Sequence& s : data->sequences) relinkTimeline(s.timeline);
    return moved.size();
}

QString fileNameAnyOs(const QString& path)
{
    const qsizetype sep = std::max(path.lastIndexOf('/'), path.lastIndexOf('\\'));
    return path.mid(sep + 1);
}

} // namespace ProjectFile
