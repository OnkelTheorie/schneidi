#include "core/ProjectFile.h"

#include "core/Dynamics.h"
#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/Equalizer.h"
#include "core/Keyframes.h"
#include "core/MediaAspect.h"
#include "core/Retime.h"

#include <QColor>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QDateTime>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>

namespace {

// Frame numbers and ids from the file (edited by hand or by an AI: may say anything) stay inside +-2^30, so that the
// sum of two (start + length, the next id) cannot overflow
int intOf(const QJsonValue& v, int fallback = 0)
{
    constexpr int kLimit = 1 << 30;
    return std::clamp(v.toInt(fallback), -kLimit, kLimit);
}

// Milliseconds (transcript words), likewise: a double beyond qint64 would not even convert
qint64 msOf(const QJsonValue& v)
{
    constexpr double kLimit = double(qint64(1) << 50);
    return qint64(std::clamp(v.toDouble(), -kLimit, kLimit));
}

// Track EQ/compressor: only written when not the default; loading clamps the values (hand-edited files)
QJsonObject eqToJson(const TrackEq& eq)
{
    QJsonArray bands;
    for (const EqBand& b : eq.bands)
        bands << QJsonObject{{"on", b.on}, {"freq", b.freq}, {"gainDb", b.gainDb}, {"q", b.q}};
    return QJsonObject{{"on", eq.on}, {"bands", bands}};
}

TrackEq eqFromJson(const QJsonObject& o)
{
    TrackEq eq;
    eq.on = o.value("on").toBool();
    const QJsonArray bands = o.value("bands").toArray();
    for (int i = 0; i < std::min<int>(bands.size(), int(eq.bands.size())); ++i) {
        const QJsonObject b = bands[i].toObject();
        EqBand& band = eq.bands[size_t(i)];
        band.on = b.value("on").toBool(band.on);
        band.freq = b.value("freq").toDouble(band.freq);
        band.gainDb = b.value("gainDb").toDouble(band.gainDb);
        band.q = b.value("q").toDouble(band.q);
    }
    return Equalizer::clamped(eq);
}

QJsonObject dynamicsToJson(const TrackDynamics& d)
{
    return QJsonObject{{"on", d.on},           {"thresholdDb", d.thresholdDb}, {"ratio", d.ratio},
                       {"attackMs", d.attackMs}, {"releaseMs", d.releaseMs},     {"kneeDb", d.kneeDb},
                       {"makeupDb", d.makeupDb}};
}

TrackDynamics dynamicsFromJson(const QJsonObject& o)
{
    TrackDynamics d;
    d.on = o.value("on").toBool();
    d.thresholdDb = o.value("thresholdDb").toDouble(d.thresholdDb);
    d.ratio = o.value("ratio").toDouble(d.ratio);
    d.attackMs = o.value("attackMs").toDouble(d.attackMs);
    d.releaseMs = o.value("releaseMs").toDouble(d.releaseMs);
    d.kneeDb = o.value("kneeDb").toDouble(d.kneeDb);
    d.makeupDb = o.value("makeupDb").toDouble(d.makeupDb);
    return Compressor::clamped(d);
}

// Format versions. Raise only when an older schneidi would silently misread the new data (it then rejects the
// file); new optional fields need no new version (written only when set, read with a default).
//  1: first format (0.1, until 2026-09-30)
//  2: audio streams per file (clip "audioStream"). Older versions would play only stream 1 and lose the assignment.
//  3: crop measured on the clip's picture as it fits into the frame (0.12, convertCrop). Older versions would crop
//     pictures of another shape than the frame wrongly.
constexpr int kFormatVersion = 3;

// Migrations: kMigrations[i] turns the JSON of version i + 1 into version i + 2 before the reader sees it. A step
// converts what the reader of the new version no longer understands (renamed/restructured fields); a new version
// without anything to convert gets an empty step. The reader itself keeps reading old optional fields with defaults.
using Migration = void (*)(QJsonObject& root);
const std::array<Migration, kFormatVersion - 1> kMigrations = {
    [](QJsonObject&) {}, // 1 -> 2: nothing to convert (clips without "audioStream" use stream 0)
    [](QJsonObject&) {}, // 2 -> 3: crop values are converted while reading (convertCrop needs the media's shape)
};

// Format 3: crop values are timeline pixels on the clip's picture fitted into the frame (ClipTransform). Up to format 2
// the engine measured them on the frame: without a transform from the frame's edges (letterbox bars included), with
// one as a share of the frame size laid onto the picture (x of the width, y of the height). For a picture shaped like
// the frame all three agree; other clips get values that crop as before (a linear crop animation that crossed the
// edge of a letterbox bar gets a keyframe there, ease curves change a little). Shape unknown (offline): left as it is.

// Linear keyframe sections of `k` that cross one of the levels get a keyframe there (nearest frame), so that
// clamping the values afterwards keeps the course: before, values beyond the level simply had no effect
void addCrossings(KeyTrack& k, std::initializer_list<double> levels)
{
    KeyTrack out;
    for (int i = 0; i < k.size(); ++i) {
        out << k[i];
        if (i + 1 >= k.size() || k[i].ease != KeyEase::Linear || k[i + 1].ease != KeyEase::Linear) continue;
        const Keyframe& a = k[i];
        const Keyframe& b = k[i + 1];
        QVector<Keyframe> between;
        for (double level : levels) {
            if ((a.value - level) * (b.value - level) >= 0) continue; // not crossing (or touching at a key)
            const int f = int(std::lround(a.frame + (level - a.value) / (b.value - a.value) * (b.frame - a.frame)));
            if (f <= a.frame || f >= b.frame) continue;
            Keyframe x;
            x.frame = f;
            x.value = level;
            between << x;
        }
        std::sort(between.begin(), between.end(), [](const Keyframe& x, const Keyframe& y) { return x.frame < y.frame; });
        for (const Keyframe& x : between)
            if (out.last().frame < x.frame) out << x;
    }
    k = out;
}

void convertCrop(Clip& c, QSizeF frame)
{
    constexpr AnimParam crops[] = {AnimParam::CropLeft, AnimParam::CropRight, AnimParam::CropTop, AnimParam::CropBottom};
    if (c.kind != ClipKind::Media) return; // titles, compound and multicam clips fill the frame
    if (std::none_of(std::begin(crops), std::end(crops),
                     [&](AnimParam p) { return Keys::staticValue(c, p) != 0 || Keys::animated(c, p); }))
        return;
    const QSizeF fit = MediaAspect::fit(MediaAspect::of(c.mediaPath), frame);
    if (std::abs(fit.width() - frame.width()) < 0.01 && std::abs(fit.height() - frame.height()) < 0.01) return;
    const bool transformed = Keys::hasTransform(c); // decided the old way of measuring
    for (AnimParam p : crops) {
        const bool x = p == AnimParam::CropLeft || p == AnimParam::CropRight;
        const double frameLen = x ? frame.width() : frame.height(), pictureLen = x ? fit.width() : fit.height();
        if (transformed) {
            const double k = pictureLen / frameLen;
            Keys::convert(c, p, [k](double v) { return v * k; }, k);
        } else {
            const double bar = (frameLen - pictureLen) / 2;
            if (c.keys.contains(p)) addCrossings(c.keys[p], {bar, bar + pictureLen});
            Keys::convert(c, p, [=](double v) { return std::clamp(v - bar, 0.0, pictureLen); }, 1.0);
        }
    }
}

void migrate(QJsonObject& root)
{
    for (int v = std::max(1, root.value("version").toInt(1)); v < kFormatVersion; ++v) kMigrations[size_t(v - 1)](root);
    root["version"] = kFormatVersion;
}

QJsonObject transformToJson(const ClipTransform& t)
{
    QJsonObject o{
        {"zoomX", t.zoomX},         {"zoomY", t.zoomY},         {"posX", t.posX},
        {"posY", t.posY},           {"rotation", t.rotation},   {"cropLeft", t.cropLeft},
        {"cropRight", t.cropRight}, {"cropTop", t.cropTop},     {"cropBottom", t.cropBottom},
        {"opacity", t.opacity},     {"transformOn", t.transformOn}, {"cropOn", t.cropOn},
        {"compositeOn", t.compositeOn},
    };
    // Fill around the fitted picture (0.12): written only when not the default, older projects stay as they were
    const ClipTransform def;
    if (t.fill != def.fill)
        for (const FitFillInfo& f : kFitFills)
            if (f.fill == t.fill) o["fill"] = f.id;
    if (t.fillBlur != def.fillBlur) o["fillBlur"] = t.fillBlur;
    if (t.fillDim != def.fillDim) o["fillDim"] = t.fillDim;
    return o;
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
    for (const FitFillInfo& f : kFitFills) // unknown (newer) fills: none
        if (o.value("fill").toString() == QLatin1String(f.id)) t.fill = f.fill;
    t.fillBlur = o.value("fillBlur").toDouble(t.fillBlur);
    t.fillDim = o.value("fillDim").toDouble(t.fillDim);
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

// Gradient image: absolute, own files additionally relative to the project file (like LUTs)
QJsonObject transitionStyleToJson(const TransitionStyle& s, const QDir& projectDir)
{
    static const char* aligns[] = {"center", "start", "end"};
    QJsonObject o{{"type", transitionTypeInfo(s.type).id}, {"align", aligns[int(s.align)]}};
    const TransitionStyle d;
    if (s.color != d.color) o["color"] = s.color.name(QColor::HexRgb);
    if (s.softness != d.softness) o["softness"] = s.softness;
    if (s.border != d.border) o["border"] = s.border;
    if (s.borderColor != d.borderColor) o["borderColor"] = s.borderColor.name(QColor::HexRgb);
    if (s.audio != d.audio) o["audioCurve"] = audioCurveInfo(s.audio).id;
    if (!s.luma.isEmpty()) {
        o["luma"] = s.luma;
        if (!EffectFolders::isBuiltin(s.luma)) o["lumaRel"] = projectDir.relativeFilePath(s.luma);
    }
    if (s.invert) o["invert"] = true;
    return o;
}

TransitionStyle transitionStyleFromJson(const QJsonObject& o, const QDir& projectDir)
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
    // older files: fade through black/white as separate styles
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
    s.luma = o.value("luma").toString();
    s.invert = o.value("invert").toBool();
    // Image missing: relative to the project file, then look for a file of the same name in the own Transitions folder
    if (!s.luma.isEmpty() && !EffectFolders::isBuiltin(s.luma) && !QFileInfo::exists(s.luma)) {
        const QString rel = o.value("lumaRel").toString();
        const QString candidate = rel.isEmpty() ? QString() : QDir::cleanPath(projectDir.absoluteFilePath(rel));
        if (!candidate.isEmpty() && QFileInfo::exists(candidate)) s.luma = candidate;
        else if (const QString found =
                     EffectFolders::findUserTransition(QString(s.luma).replace('\\', '/').section('/', -1));
                 !found.isEmpty())
            s.luma = found;
    }
    return s;
}

// Keyframes: {"zoomX": [[source frame, value], [frame, value, "easeIn"], ...], ...}; interpolation only if not linear.
// Bezier: [frame, value, "bezier", in dt, in dv, out dt, out dv] (older versions read that as linear)
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
        if (!Keys::fromId(it.key(), &p)) continue; // unknown (newer version) -> ignore
        KeyTrack track;
        for (const QJsonValue& v : it.value().toArray()) {
            const QJsonArray e = v.toArray();
            if (e.size() < 2) continue;
            Keyframe k{intOf(e.at(0)), e.at(1).toDouble(), KeyEase::Linear};
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

// Path parameter of an effect (e.g. LUT of the Color page) relative to the project file
QJsonObject pathParamsRelative(const EffectInstance& e, const QDir& projectDir)
{
    QJsonObject rel;
    if (const EffectDescriptor* d = EffectRegistry::find(e.effectId))
        for (const EffectParam& p : d->params) {
            const QString path = e.params.value(p.key).toString();
            if (p.type == EffectParam::Path && !path.isEmpty() && !EffectFolders::isBuiltin(path))
                rel[p.key] = projectDir.relativeFilePath(path);
        }
    return rel;
}

// As with media: absolute path missing -> try relative to the project file (folder moved), for LUTs then
// a file of the same name in the own LUTs folder (project from another machine, e.g. Linux <-> Windows)
void resolvePathParams(EffectInstance& e, const QJsonObject& rel, const QDir& projectDir)
{
    const EffectDescriptor* d = EffectRegistry::find(e.effectId);
    if (!d) return;
    for (const EffectParam& p : d->params) {
        const QString abs = e.params.value(p.key).toString();
        if (p.type != EffectParam::Path || abs.isEmpty() || QFileInfo::exists(abs)) continue;
        if (rel.contains(p.key)) {
            const QString candidate = QDir::cleanPath(projectDir.absoluteFilePath(rel.value(p.key).toString()));
            if (QFileInfo::exists(candidate)) {
                e.params[p.key] = candidate;
                continue;
            }
        }
        if (EffectFolders::isLutFile(abs)) // file name also from Windows paths ("C:\...\look.cube")
            if (const QString found = EffectFolders::findUserLut(QString(abs).replace('\\', '/').section('/', -1));
                !found.isEmpty())
                e.params[p.key] = found;
    }
}

// JSON has no colors: bring values back to the registry's type (color as QColor, checkbox as bool), otherwise a
// loaded clip would count as changed (undo, render cache). Unknown effects stay as read.
void normalizeParams(EffectInstance& e)
{
    const EffectDescriptor* d = EffectRegistry::find(e.effectId);
    if (!d) return;
    for (const EffectParam& p : d->params) {
        auto it = e.params.find(p.key);
        if (it == e.params.end()) continue;
        switch (p.type) {
        case EffectParam::Color: *it = QColor(it->toString()); break;
        case EffectParam::Bool: *it = it->toBool(); break;
        case EffectParam::Double: *it = it->toDouble(); break;
        case EffectParam::Choice:
        case EffectParam::Path: *it = it->toString(); break;
        }
    }
}

// Clips refer to the media list by index -> path appears only once in the file
QJsonObject clipToJson(const Clip& c, int mediaIndex, const QDir& projectDir)
{
    QJsonObject o{{"id", c.id}, {"start", c.start}, {"in", c.in}, {"out", c.out}};
    if (c.isTitle()) o["title"] = titleToJson(c.title); // titles have no media reference
    else if (c.isCompound()) o["sequence"] = c.sequenceId; // compound clip: reference to the sequence
    else if (c.isMulticam()) { // multicam segment: sequence + angle shown
        o["multicam"] = c.sequenceId;
        if (c.angle != 0) o["angle"] = c.angle;
    } else o["media"] = mediaIndex;
    if (c.linkId) o["link"] = c.linkId;
    if (c.volumeDb != 0.0) o["volumeDb"] = c.volumeDb;
    if (c.pan != 0.0) o["pan"] = c.pan;
    if (c.audioStream != 0) o["audioStream"] = c.audioStream;
    if (c.subframe != 0) o["subframe"] = c.subframe;
    if (!c.enabled) o["enabled"] = false;
    if (c.transIn > 0) o["transIn"] = c.transIn; // transitions (frames); missing = none
    if (c.transOut > 0) o["transOut"] = c.transOut;
    if (c.transInAlone) o["transInAlone"] = true;
    if (c.transOutAlone) o["transOutAlone"] = true;
    if (c.transIn > 0 && c.transInStyle != TransitionStyle{})
        o["transInStyle"] = transitionStyleToJson(c.transInStyle, projectDir);
    if (c.transOut > 0 && c.transOutStyle != TransitionStyle{})
        o["transOutStyle"] = transitionStyleToJson(c.transOutStyle, projectDir);
    if (c.fadeIn > 0) o["fadeIn"] = c.fadeIn; // fade handles (frames)
    if (c.fadeOut > 0) o["fadeOut"] = c.fadeOut;
    if (c.speed != 1.0) o["speed"] = c.speed;
    if (c.reverse) o["reverse"] = true;
    if (c.freeze) o["freeze"] = true;
    if (c.freeze && c.freezeFrame >= 0) o["freezeFrame"] = c.freezeFrame;
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
    if (c.transform != ClipTransform{}) o["transform"] = transformToJson(c.transform); // any value or switch changed
    if (!c.effects.isEmpty()) {
        QJsonArray fx;
        for (const EffectInstance& e : c.effects) {
            QJsonObject eo{{"id", e.effectId}, {"enabled", e.enabled}, {"params", QJsonObject::fromVariantMap(e.params)}};
            // File paths (LUT) additionally relative to the project file, like media
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
    c.id = intOf(o.value("id"));
    if (o.contains("title")) {
        c.kind = ClipKind::Title;
        c.title = titleFromJson(o.value("title").toObject());
    } else if (o.contains("sequence")) {
        c.kind = ClipKind::Compound;
        c.sequenceId = intOf(o.value("sequence"));
    } else if (o.contains("multicam")) {
        c.kind = ClipKind::Multicam;
        c.sequenceId = intOf(o.value("multicam"));
        c.angle = std::max(0, o.value("angle").toInt());
    } else {
        c.mediaPath = media.value(o.value("media").toInt(-1)).path;
    }
    c.start = std::max(0, intOf(o.value("start")));
    c.in = intOf(o.value("in"));
    c.out = std::max(c.in, intOf(o.value("out"))); // file edited by hand: at least 1 frame long
    c.linkId = intOf(o.value("link"));
    c.volumeDb = o.value("volumeDb").toDouble(0.0);
    c.pan = o.value("pan").toDouble(0.0);
    c.audioStream = std::max(0, o.value("audioStream").toInt());
    c.subframe = std::clamp(o.value("subframe").toInt(), 0, 99);
    c.enabled = o.value("enabled").toBool(true);
    c.transIn = std::max(0, intOf(o.value("transIn")));
    c.transOut = std::max(0, intOf(o.value("transOut")));
    c.transInAlone = c.transIn > 0 && o.value("transInAlone").toBool();
    c.transOutAlone = c.transOut > 0 && o.value("transOutAlone").toBool();
    c.transInStyle = transitionStyleFromJson(o.value("transInStyle").toObject(), projectDir);
    c.transOutStyle = transitionStyleFromJson(o.value("transOutStyle").toObject(), projectDir);
    c.fadeIn = std::max(0, intOf(o.value("fadeIn")));
    c.fadeOut = std::max(0, intOf(o.value("fadeOut")));
    c.speed = o.value("speed").toDouble(1.0);
    if (c.speed <= 0) c.speed = 1.0;
    c.reverse = o.value("reverse").toBool();
    c.freeze = o.value("freeze").toBool();
    // Older files without freezeFrame: pin the frame = `in` on loading (splitting/trimming shifts `in` later); a frame
    // before the file's start (edited by hand) is its first one - a negative one is not saved and came back as `in`
    c.freezeFrame = c.freeze ? std::max(0, intOf(o.value("freezeFrame"), c.in)) : -1;
    c.keepPitch = o.value("keepPitch").toBool(true);
    for (const QJsonValue& v : o.value("ramp").toArray()) {
        const QJsonObject j = v.toObject();
        SpeedPoint p;
        p.source = j.value("source").toDouble();
        p.speed = j.value("speed").toDouble(1.0);
        p.smooth = j.value("smooth").toInt();
        c.ramp << p;
    }
    Retime::normalize(c.ramp, 0); // discard broken values (file length unknown here)
    c.renderCache = o.value("renderCache").toBool();
    if (o.contains("transform")) c.transform = transformFromJson(o.value("transform").toObject());
    for (const QJsonValue& v : o.value("effects").toArray()) {
        const QJsonObject e = v.toObject();
        EffectInstance inst{e.value("id").toString(), e.value("params").toObject().toVariantMap(),
                            e.value("enabled").toBool(true)};
        resolvePathParams(inst, e.value("relPaths").toObject(), projectDir);
        normalizeParams(inst);
        c.effects << inst;
    }
    keysFromJson(c, o.value("keys").toObject()); // missing in older files
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
    return abs; // stays offline until relinked
}

} // namespace

namespace ProjectFile {

QJsonObject clipJson(const Clip& c)
{
    QJsonObject o = clipToJson(c, -1, QDir::root()); // fixed reference -> relPaths stable (cache key)
    if (!c.isTitle() && !c.isNested()) o["media"] = c.mediaPath;
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
        if (m.markIn >= 0) o["markIn"] = m.markIn; // source in/out (optional, absent in old files)
        if (m.markOut >= 0) o["markOut"] = m.markOut;
        if (!m.markers.isEmpty()) { // source markers (optional)
            QJsonArray markers;
            for (int f : m.markers) markers << f;
            o["markers"] = markers;
        }
        if (m.bin != 0) o["bin"] = m.bin; // Media Pool organization (optional, absent in old files)
        if (!m.clipColor.isEmpty()) o["clipColor"] = m.clipColor;
        if (!m.flags.isEmpty()) o["flags"] = QJsonArray::fromStringList(m.flags);
        if (!m.audioStreams.isEmpty()) {
            QJsonArray streams;
            for (const AudioStreamInfo& a : m.audioStreams) {
                QJsonObject so{{"channels", a.channels}};
                if (!a.title.isEmpty()) so["title"] = a.title;
                streams << so;
            }
            o["audioStreams"] = streams;
        }
        if (!m.transcripts.isEmpty()) { // words compact as [from ms, to ms, text]
            QJsonArray list;
            for (const MediaTranscript& t : m.transcripts) {
                QJsonArray words;
                for (const TranscriptWord& w : t.words) words << QJsonArray{double(w.from), double(w.to), w.text};
                list << QJsonObject{{"stream", t.audioStream}, {"language", t.language}, {"words", words}};
            }
            o["transcripts"] = list;
        }
        media << o;
    }

    auto tracks = [&](const QVector<Track>& list, TrackKind kind) {
        QJsonArray arr;
        for (const Track& t : list) {
            QJsonArray clips;
            for (const Clip& c : t.clips) {
                if (c.isTitle() || c.isNested()) {
                    clips << clipToJson(c, -1, projectDir);
                    continue;
                }
                if (!index.contains(c.mediaPath)) { // should not happen, but lose nothing
                    index[c.mediaPath] = media.size();
                    media << QJsonObject{{"path", c.mediaPath}, {"relPath", projectDir.relativeFilePath(c.mediaPath)},
                                         {"name", QFileInfo(c.mediaPath).fileName()}, {"notInPool", true}};
                }
                clips << clipToJson(c, index.value(c.mediaPath), projectDir);
            }
            // "name" = abbreviation (older versions display it), own name in "label"
            QJsonObject o{{"name", trackShortName({kind, int(arr.size())})}, {"clips", clips}};
            if (!t.name.isEmpty()) o["label"] = t.name;
            if (!t.color.isEmpty()) o["color"] = t.color;
            if (t.locked) o["locked"] = true;
            if (t.muted) o["muted"] = true;
            if (t.hidden) o["hidden"] = true;
            if (t.volumeDb != 0.0) o["volumeDb"] = t.volumeDb;
            if (t.pan != 0.0) o["pan"] = t.pan;
            if (t.solo) o["solo"] = true;
            if (t.eq != TrackEq{}) o["eq"] = eqToJson(t.eq);
            if (t.dynamics != TrackDynamics{}) o["dynamics"] = dynamicsToJson(t.dynamics);
            arr << o;
        }
        return arr;
    };
    auto timelineJson = [&](const Timeline& timeline) {
        QJsonArray subtitles; // subtitle tracks (optional, absent in old files)
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

    // Write all timelines first: clips with files outside the Media Pool extend the media list in the process
    const QJsonObject current = timelineJson(data.timeline);
    // Further timelines and compound clips (optional, absent in old files); the opened one is in "timeline"
    QJsonArray seqs;
    for (const Sequence& s : data.sequences) {
        QJsonObject o{{"id", s.id}, {"name", s.name}};
        if (s.compound) o["compound"] = true;
        if (s.multicam) { // angles: name, video track (-1 = none), audio tracks
            QJsonArray angles;
            for (const MulticamAngle& a : s.angles) {
                QJsonArray audio;
                for (int t : a.audioTracks) audio << t;
                angles << QJsonObject{{"name", a.name}, {"video", a.videoTrack}, {"audio", audio}};
            }
            o["multicam"] = true;
            o["angles"] = angles;
        }
        if (s.bin != 0) o["bin"] = s.bin;
        if (s.id != data.currentSequence) o["timeline"] = timelineJson(s.timeline);
        seqs << o;
    }
    QJsonArray bins;
    for (const MediaBin& b : data.bins) bins << QJsonObject{{"id", b.id}, {"parent", b.parent}, {"name", b.name}};

    QJsonObject root{
        {"app", "schneidi"},       {"version", kFormatVersion},
        {"fps", data.format.rate.timebase()}, // for older versions
        {"format", QJsonObject{{"width", data.format.width}, {"height", data.format.height},
                               {"fpsNum", data.format.rate.num}, {"fpsDen", data.format.rate.den}}},
        {"playhead", data.playhead}, {"lastClipId", data.lastClipId}, {"lastLinkId", data.lastLinkId},
        {"media", media},           {"bins", bins},
        {"timeline", current}, // opened timeline (older versions read only this one)
    };
    if (!seqs.isEmpty()) {
        root["sequences"] = seqs;
        root["currentSequence"] = data.currentSequence;
    }
    if (!data.renderQueue.isEmpty()) root["renderQueue"] = RenderQueueJson::toJson(data.renderQueue);
    if (data.fixedResolution || data.fixedFrameRate) { // optional, absent in old files
        QJsonObject fixed;
        if (data.fixedResolution) fixed["resolution"] = true;
        if (data.fixedFrameRate) fixed["frameRate"] = true;
        root["fixedFormat"] = fixed;
    }
    if (data.importPresets) root["importPresets"] = data.importPresets->toJson(); // optional
    // Watch folders (optional, absent in old files; older versions ignore them): folder also relative to the project
    // like media, so a project moved together with its recording folder keeps watching it
    if (!data.watchFolders.isEmpty()) {
        QJsonArray watches;
        for (const WatchFolder& w : data.watchFolders) {
            QJsonObject o{{"path", w.path}, {"relPath", projectDir.relativeFilePath(w.path)},
                          {"types", QJsonArray::fromStringList(WatchFolders::typeNames(w))}};
            if (w.subfolders) o["subfolders"] = true;
            if (w.bin != 0) o["bin"] = w.bin;
            if (w.since.isValid()) o["since"] = w.since.toUTC().toString(Qt::ISODateWithMs);
            if (!w.ignored.isEmpty()) o["ignored"] = QJsonArray::fromStringList(w.ignored);
            watches << o;
        }
        root["watchFolders"] = watches;
    }
    if (data.watchRevision > 0) {
        QJsonArray files;
        for (const WatchArrival& a : data.watchLog)
            files << QJsonObject{{"revision", a.revision}, {"path", a.path}, {"folder", a.folder},
                                 {"time", a.time.toUTC().toString(Qt::ISODateWithMs)}};
        root["watchLog"] = QJsonObject{{"revision", data.watchRevision}, {"files", files}};
    }
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool fromJson(const QByteArray& json, const QString& projectPath, ProjectData* data, QString* error)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    QJsonObject root = doc.object();
    if (pe.error != QJsonParseError::NoError || root.value("app").toString() != "schneidi") {
        if (error) *error = T("Keine gültige schneidi-Projektdatei.");
        return false;
    }
    if (root.value("version").toInt() > kFormatVersion) {
        if (error) *error = T("Die Projektdatei stammt aus einer neueren schneidi-Version.");
        return false;
    }
    const int fileVersion = std::max(1, root.value("version").toInt(1));
    migrate(root);

    const QDir projectDir = QFileInfo(projectPath).absoluteDir();
    ProjectData d;
    // Project settings; older files know only "fps" (integer) and were always 1920 x 1080
    const QJsonObject fmt = root.value("format").toObject();
    d.format.width = fmt.value("width").toInt(1920);
    d.format.height = fmt.value("height").toInt(1080);
    d.format.rate.num = fmt.value("fpsNum").toInt(root.value("fps").toInt(25));
    d.format.rate.den = fmt.value("fpsDen").toInt(1);
    d.format = withinLimits(d.format);
    d.playhead = intOf(root.value("playhead"));
    d.lastClipId = intOf(root.value("lastClipId"));
    d.lastLinkId = intOf(root.value("lastLinkId"));
    // All entries resolve the clip references; "notInPool" (file only in the timeline) does not go into the Media Pool
    QVector<MediaInfo> allMedia;
    for (const QJsonValue& v : root.value("media").toArray()) {
        const QJsonObject o = v.toObject();
        MediaInfo m;
        m.path = resolvePath(o, projectDir);
        m.name = o.value("name").toString(fileNameAnyOs(m.path));
        m.length = intOf(o.value("length"));
        m.hasVideo = o.value("hasVideo").toBool();
        m.hasAudio = o.value("hasAudio").toBool();
        m.isImage = o.value("isImage").toBool();
        m.markIn = intOf(o.value("markIn"), -1);
        m.markOut = intOf(o.value("markOut"), -1);
        for (const QJsonValue& f : o.value("markers").toArray())
            if (intOf(f, -1) >= 0 && !m.markers.contains(intOf(f))) m.markers << intOf(f);
        std::sort(m.markers.begin(), m.markers.end());
        m.bin = std::max(0, o.value("bin").toInt()); // missing bins are set to Master by Project::load
        if (trackColorInfo(o.value("clipColor").toString())) m.clipColor = o.value("clipColor").toString();
        for (const QJsonValue& f : o.value("flags").toArray())
            if (flagColorInfo(f.toString()) && !m.flags.contains(f.toString())) m.flags << f.toString();
        if (m.hasAudio) // missing in older files -> audioStreamCount() assumes 1 stream
            for (const QJsonValue& a : o.value("audioStreams").toArray())
                m.audioStreams << AudioStreamInfo{std::max(1, a.toObject().value("channels").toInt(2)),
                                                  a.toObject().value("title").toString()};
        for (const QJsonValue& v : o.value("transcripts").toArray()) {
            const QJsonObject to = v.toObject();
            MediaTranscript t;
            t.audioStream = std::max(0, to.value("stream").toInt());
            t.language = to.value("language").toString();
            for (const QJsonValue& w : to.value("words").toArray()) {
                const QJsonArray a = w.toArray();
                if (a.size() >= 3) t.words << TranscriptWord{msOf(a[0]), msOf(a[1]), a[2].toString()};
            }
            if (!t.words.isEmpty() && !m.transcript(t.audioStream)) m.transcripts << t;
        }
        allMedia << m;
        if (!o.value("notInPool").toBool()) d.media << m;
    }
    for (const QJsonValue& v : root.value("bins").toArray()) {
        const QJsonObject o = v.toObject();
        const MediaBin b{intOf(o.value("id")), std::max(0, intOf(o.value("parent"))), o.value("name").toString()};
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
            t.name = o.value("label").toString(); // older files: only abbreviation in "name" -> default name
            if (trackColorInfo(o.value("color").toString())) t.color = o.value("color").toString();
            t.locked = o.value("locked").toBool();
            t.muted = o.value("muted").toBool();
            t.hidden = o.value("hidden").toBool();
            t.volumeDb = o.value("volumeDb").toDouble(0.0);
            t.pan = o.value("pan").toDouble(0.0);
            t.solo = o.value("solo").toBool();
            if (o.contains("eq")) t.eq = eqFromJson(o.value("eq").toObject());
            if (o.contains("dynamics")) t.dynamics = dynamicsFromJson(o.value("dynamics").toObject());
            for (const QJsonValue& cv : o.value("clips").toArray()) {
                Clip c = clipFromJson(cv.toObject(), allMedia, projectDir);
                if (fileVersion < 3 && kind == TrackKind::Video) convertCrop(c, QSizeF(d.format.width, d.format.height));
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
                style["text"] = QString(); // style without text
                // missing values: subtitle default instead of title default
                const TitleStyle base = subtitleBaseStyle();
                const QJsonObject def = titleToJson(base);
                for (auto it = def.begin(); it != def.end(); ++it)
                    if (!style.contains(it.key())) style[it.key()] = it.value();
                t.style = titleFromJson(style);
            }
            for (const QJsonValue& cv : o.value("cues").toArray()) {
                const QJsonObject c = cv.toObject();
                SubtitleCue cue{intOf(c.value("id")), intOf(c.value("start")), intOf(c.value("end")),
                                c.value("text").toString()};
                if (cue.end <= cue.start) continue;
                t.cues << cue;
            }
            std::sort(t.cues.begin(), t.cues.end(), [](const SubtitleCue& a, const SubtitleCue& b) { return a.start < b.start; });
            timeline.subtitles << t;
        }
        // ids unique across clips and subtitles; missing/duplicate ones (hand edits) get a new one
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
        for (const QJsonValue& v : tl.value("markers").toArray()) timeline.markers << intOf(v);
        timeline.masterVolumeDb = tl.value("masterVolumeDb").toDouble(0.0);
        timeline.masterLimiter = tl.value("masterLimiter").toBool(false);
        timeline.masterLimiterDb = std::clamp(tl.value("masterLimiterDb").toDouble(kDefaultLimiterDb), -24.0, 0.0);
        std::sort(timeline.markers.begin(), timeline.markers.end());
        timeline.markIn = intOf(tl.value("markIn"), -1);
        timeline.markOut = intOf(tl.value("markOut"), -1);
        return timeline;
    };
    d.timeline = readTimeline(root.value("timeline").toObject());
    for (const QJsonValue& v : root.value("sequences").toArray()) {
        const QJsonObject o = v.toObject();
        Sequence seq;
        seq.id = intOf(o.value("id"));
        seq.name = o.value("name").toString();
        seq.compound = o.value("compound").toBool();
        seq.multicam = o.value("multicam").toBool();
        for (const QJsonValue& av : o.value("angles").toArray()) {
            const QJsonObject a = av.toObject();
            MulticamAngle angle{a.value("name").toString(), a.value("video").toInt(-1), {}};
            for (const QJsonValue& t : a.value("audio").toArray()) angle.audioTracks << t.toInt(-1);
            seq.angles << angle;
        }
        seq.bin = std::max(0, o.value("bin").toInt());
        if (seq.id <= 0) continue;
        if (o.contains("timeline")) seq.timeline = readTimeline(o.value("timeline").toObject());
        d.sequences << seq;
    }
    d.currentSequence = intOf(root.value("currentSequence"));
    d.renderQueue = RenderQueueJson::fromJson(root.value("renderQueue").toArray());
    const QJsonObject fixed = root.value("fixedFormat").toObject();
    d.fixedResolution = fixed.value("resolution").toBool();
    d.fixedFrameRate = fixed.value("frameRate").toBool();
    if (root.value("importPresets").isObject())
        d.importPresets = StreamPresets::fromJson(root.value("importPresets").toObject());
    for (const QJsonValue& v : root.value("watchFolders").toArray()) {
        const QJsonObject o = v.toObject();
        WatchFolder w;
        w.path = resolvePath(o, projectDir);
        if (w.path.isEmpty() || WatchFolders::indexOf(d.watchFolders, w.path) >= 0) continue;
        w.subfolders = o.value("subfolders").toBool();
        // Kinds of a newer version are left out; a watch with none this version knows is no watch here
        QStringList types;
        for (const QJsonValue& t : o.value("types").toArray())
            if (WatchFolder known; WatchFolders::setTypes(known, {t.toString()})) types << t.toString();
        if (o.contains("types") && !WatchFolders::setTypes(w, types)) continue;
        w.bin = std::max(0, o.value("bin").toInt()); // a missing bin is set to Master by Project::load
        w.since = QDateTime::fromString(o.value("since").toString(), Qt::ISODateWithMs);
        for (const QJsonValue& f : o.value("ignored").toArray())
            if (!f.toString().isEmpty()) w.ignored << f.toString();
        d.watchFolders << w;
    }
    const QJsonObject watchLog = root.value("watchLog").toObject();
    d.watchRevision = std::max(0, watchLog.value("revision").toInt());
    for (const QJsonValue& v : watchLog.value("files").toArray()) {
        const QJsonObject o = v.toObject();
        const WatchArrival a{o.value("revision").toInt(), o.value("path").toString(), o.value("folder").toString(),
                             QDateTime::fromString(o.value("time").toString(), Qt::ISODateWithMs)};
        if (a.revision > 0 && a.revision <= d.watchRevision && !a.path.isEmpty()) d.watchLog << a;
    }
    std::sort(d.watchLog.begin(), d.watchLog.end(),
              [](const WatchArrival& a, const WatchArrival& b) { return a.revision < b.revision; });
    if (d.watchLog.size() > WatchFolders::kLogSize) d.watchLog.remove(0, d.watchLog.size() - WatchFolders::kLogSize);

    *data = d;
    return true;
}

bool save(const ProjectData& data, const QString& path, QString* error)
{
    // QSaveFile: write completely first, then rename -> never half a project file
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(toJson(data, path)) < 0 || !f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

QString backupDir(const QString& projectPath)
{
    const QString root = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("backups");
    if (projectPath.isEmpty()) return root;
    // Name + short hash of the path: projects of the same name in different folders do not get mixed up
    const QFileInfo fi(projectPath);
    const QString hash = QCryptographicHash::hash(fi.absoluteFilePath().toUtf8(), QCryptographicHash::Sha1).toHex().left(8);
    return QDir(root).filePath(fi.completeBaseName() + "-" + hash);
}

namespace {
// Backup copies old -> new. By modification time, not by name: the name carries local time, and at the
// autumn clock change (03:00 -> 02:00) new copies sorted before the old ones -> the newest were deleted
QStringList backupsOldestFirst(const QDir& dir)
{
    QFileInfoList list = dir.entryInfoList({QString("*.%1").arg(Extension)}, QDir::Files);
    std::sort(list.begin(), list.end(), [](const QFileInfo& a, const QFileInfo& b) {
        const qint64 ta = a.lastModified().toMSecsSinceEpoch(), tb = b.lastModified().toMSecsSinceEpoch();
        return ta != tb ? ta < tb : a.fileName() < b.fileName();
    });
    QStringList names;
    for (const QFileInfo& fi : list) names << fi.fileName();
    return names;
}
} // namespace

QStringList backups(const QString& projectPath)
{
    const QDir dir(backupDir(projectPath));
    QStringList out;
    const QStringList names = backupsOldestFirst(dir);
    for (auto it = names.rbegin(); it != names.rend(); ++it) out << dir.filePath(*it);
    return out;
}

QString backup(const QString& projectPath, int keep)
{
    QFile current(projectPath);
    if (!current.open(QIODevice::ReadOnly)) return {}; // never saved yet
    const QByteArray bytes = current.readAll();
    const QDir dir(backupDir(projectPath));
    if (!QDir().mkpath(dir.path())) return {};
    QStringList existing = backupsOldestFirst(dir);
    if (!existing.isEmpty()) {
        QFile newest(dir.filePath(existing.last()));
        if (newest.open(QIODevice::ReadOnly) && newest.readAll() == bytes) return {};
    }
    // Timestamp without colons (Windows), with milliseconds: names sort chronologically
    QString target;
    for (QDateTime t = QDateTime::currentDateTime(); target.isEmpty() || QFileInfo::exists(target); t = t.addMSecs(1))
        target = dir.filePath(QString("%1.%2").arg(t.toString("yyyy-MM-dd HH-mm-ss-zzz"), Extension));
    QFile out(target);
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size()) return {};
    out.close();
    existing = backupsOldestFirst(dir);
    for (int i = 0; i + keep < existing.size(); ++i) QFile::remove(dir.filePath(existing[i]));
    return target;
}

std::unique_ptr<QLockFile> lock(const QString& projectPath, int timeoutMs)
{
    const QDir dir(QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath("schneidi-locks"));
    dir.mkpath(".");
    const QByteArray hash =
        QCryptographicHash::hash(QFileInfo(projectPath).absoluteFilePath().toUtf8(), QCryptographicHash::Sha1).toHex();
    auto l = std::make_unique<QLockFile>(dir.filePath(QString::fromLatin1(hash.left(16)) + ".lock"));
    if (!l->tryLock(timeoutMs)) return nullptr;
    return l;
}

QByteArray fingerprint(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha1);
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

QStringList missingEffects(const ProjectData& data)
{
    QSet<QString> out;
    auto scan = [&](const Timeline& tl) {
        for (const auto* tracks : {&tl.video, &tl.audio})
            for (const Track& t : *tracks)
                for (const Clip& c : t.clips)
                    for (const EffectInstance& e : c.effects)
                        if (!EffectRegistry::find(e.effectId)) out << e.effectId;
    };
    scan(data.timeline);
    for (const Sequence& s : data.sequences) scan(s.timeline);
    QStringList list(out.begin(), out.end());
    list.sort();
    return list;
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
    QHash<QString, QStringList> wanted; // file name -> old paths (camera names like C0001.MP4 repeat)
    for (const QString& p : missingMedia(*data)) wanted[fileNameAnyOs(p)] << p;
    if (wanted.isEmpty()) return 0;

    QHash<QString, QStringList> found; // file name -> files found
    QDirIterator it(searchDir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (wanted.contains(it.fileName())) found[it.fileName()] << path;
    }

    // How many folders at the end of both paths match (camA/C0001.MP4 belongs to .../camA/C0001.MP4)
    auto score = [](const QString& a, const QString& b) {
        static const QRegularExpression sep("[/\\\\]");
        const QStringList x = a.split(sep, Qt::SkipEmptyParts), y = b.split(sep, Qt::SkipEmptyParts);
        int n = 0;
        while (n < x.size() && n < y.size() && x[x.size() - 1 - n] == y[y.size() - 1 - n]) ++n;
        return n;
    };
    QHash<QString, QString> moved; // old path -> new path
    for (auto w = wanted.cbegin(); w != wanted.cend(); ++w) {
        QStringList candidates = found.value(w.key());
        // Assign pairs with the best match first, each file only once
        struct Pair { int score; QString old, now; };
        QVector<Pair> pairs;
        for (const QString& old : w.value())
            for (const QString& now : candidates) pairs << Pair{score(old, now), old, now};
        std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.score > b.score; });
        QSet<QString> used;
        for (const Pair& pr : pairs) {
            if (moved.contains(pr.old) || used.contains(pr.now)) continue;
            // Ambiguous (several old files of the same name, another file fits just as well): better leave offline
            // than to show wrong material
            const bool ambiguous = w.value().size() > 1 && std::any_of(pairs.begin(), pairs.end(), [&](const Pair& o) {
                return o.score == pr.score && !moved.contains(o.old) && !used.contains(o.now)
                       && ((o.old == pr.old) != (o.now == pr.now));
            });
            if (ambiguous) continue;
            moved.insert(pr.old, pr.now);
            used.insert(pr.now);
        }
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
