// SPDX-License-Identifier: AGPL-3.0-only
#include "core/TitleLookJson.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>

namespace TitleLookJson {

namespace {

QString colorName(const QColor& c) { return c.alpha() == 255 ? c.name() : c.name(QColor::HexArgb); }

// Reads one group's fields; the first bad value ends it (error text in English for CLI/MCP)
struct Reader {
    bool lenient;
    QString err;

    bool bad(const QString& what)
    {
        if (err.isEmpty()) err = what;
        return false;
    }
    void num(const QJsonObject& o, const char* key, double* v, double lo, double hi, const QString& group)
    {
        if (!o.contains(key)) return;
        const QJsonValue x = o.value(key);
        if (!x.isDouble() || !std::isfinite(x.toDouble())) {
            if (!lenient) bad(QString("%1.%2 must be a number").arg(group, key));
            return;
        }
        const double d = x.toDouble();
        if (!lenient && (d < lo || d > hi)) {
            bad(QString("%1.%2 must be between %3 and %4").arg(group, key).arg(lo).arg(hi));
            return;
        }
        *v = std::clamp(d, lo, hi);
    }
    void color(const QJsonObject& o, const char* key, QColor* v, const QString& group)
    {
        if (!o.contains(key)) return;
        const QColor c(o.value(key).toString());
        if (!o.value(key).isString() || !c.isValid()) {
            if (!lenient) bad(QString("%1.%2 must be a color like \"#ff8800\" or \"#80000000\"").arg(group, key));
            return;
        }
        *v = c;
    }
    void flag(const QJsonObject& o, const char* key, bool* v, const QString& group)
    {
        if (!o.contains(key)) return;
        if (!o.value(key).isBool()) {
            if (!lenient) bad(QString("%1.%2 must be true or false").arg(group, key));
            return;
        }
        *v = o.value(key).toBool();
    }
    // A group: an object, or (CLI) true/false for its switch. Returns the object to read ({} for a bool)
    bool group(const QJsonValue& v, const QString& name, bool* on, QJsonObject* out)
    {
        if (v.isBool()) {
            *on = v.toBool();
            *out = {};
            return true;
        }
        if (!v.isObject()) return lenient ? false : bad(name + " must be an object (or true/false)");
        *out = v.toObject();
        if (!lenient && !out->contains("on")) *on = true; // the CLI: giving its values switches it on
        flag(*out, "on", on, name);
        return true;
    }
};

} // namespace

QStringList keys() { return {"fill", "gradient", "texture", "edge3d", "shadow", "glow", "text3d"}; }

void write(QJsonObject& o, const TitleLook& l)
{
    const TitleLook d;
    if (l.fill != d.fill)
        for (const auto& f : kTitleFills)
            if (f.fill == l.fill) o["fill"] = f.id;
    if (l.stops != d.stops || l.gradAngle != d.gradAngle) {
        QJsonArray stops;
        for (const ColorStop& s : l.stops) stops << QJsonObject{{"at", s.at}, {"color", colorName(s.color)}};
        o["gradient"] = QJsonObject{{"stops", stops}, {"angle", l.gradAngle}};
    }
    if (l.texture != d.texture || l.texScale != d.texScale || l.texX != d.texX || l.texY != d.texY)
        o["texture"] = QJsonObject{{"path", l.texture}, {"scale", l.texScale}, {"x", l.texX}, {"y", l.texY}};
    if (l.edgeOn != d.edgeOn || l.edgeDepth != d.edgeDepth || l.edgeAngle != d.edgeAngle || l.edgeColor != d.edgeColor
        || l.edgeLight != d.edgeLight)
        o["edge3d"] = QJsonObject{{"on", l.edgeOn}, {"depth", l.edgeDepth}, {"angle", l.edgeAngle},
                                  {"color", colorName(l.edgeColor)}, {"light", l.edgeLight}};
    if (l.shadowOn != d.shadowOn || l.shadowX != d.shadowX || l.shadowY != d.shadowY || l.shadowSoft != d.shadowSoft
        || l.shadowColor != d.shadowColor || l.shadowOpacity != d.shadowOpacity)
        o["shadow"] = QJsonObject{{"on", l.shadowOn}, {"x", l.shadowX}, {"y", l.shadowY}, {"soft", l.shadowSoft},
                                  {"color", colorName(l.shadowColor)}, {"opacity", l.shadowOpacity}};
    if (l.glowOn != d.glowOn || l.glowRadius != d.glowRadius || l.glowColor != d.glowColor
        || l.glowStrength != d.glowStrength)
        o["glow"] = QJsonObject{{"on", l.glowOn}, {"radius", l.glowRadius}, {"color", colorName(l.glowColor)},
                                {"strength", l.glowStrength}};
    if (l.solidOn != d.solidOn || l.solidDepth != d.solidDepth || l.solidBevel != d.solidBevel
        || l.solidRotX != d.solidRotX || l.solidRotY != d.solidRotY || l.solidRotZ != d.solidRotZ
        || l.solidFov != d.solidFov || l.solidDistance != d.solidDistance || l.solidLightAngle != d.solidLightAngle
        || l.solidLightHeight != d.solidLightHeight || l.solidSide != d.solidSide || l.solidShine != d.solidShine
        || l.solidMetal != d.solidMetal)
        o["text3d"] = QJsonObject{{"on", l.solidOn},
                                  {"depth", l.solidDepth},
                                  {"bevel", l.solidBevel},
                                  {"rot_x", l.solidRotX},
                                  {"rot_y", l.solidRotY},
                                  {"rot_z", l.solidRotZ},
                                  {"fov", l.solidFov},
                                  {"distance", l.solidDistance},
                                  {"light_angle", l.solidLightAngle},
                                  {"light_height", l.solidLightHeight},
                                  {"side", colorName(l.solidSide)},
                                  {"shine", l.solidShine},
                                  {"metal", l.solidMetal}};
}

bool read(const QJsonObject& o, TitleLook& out, bool lenient, QString* err, bool* any)
{
    TitleLook l = out;
    Reader r{lenient, {}};
    bool found = false;
    bool fillGiven = false;
    if (o.contains("fill")) {
        found = true;
        const QString id = o.value("fill").toString();
        bool known = false;
        for (const auto& f : kTitleFills)
            if (id == QLatin1String(f.id)) {
                l.fill = f.fill;
                known = true;
            }
        if (!known && !lenient) r.bad("fill must be solid, gradient or texture");
        fillGiven = known;
    }
    if (o.contains("gradient")) {
        found = true;
        const QJsonValue v = o.value("gradient");
        QJsonArray list;
        if (v.isArray()) list = v.toArray();
        else if (v.isObject()) {
            list = v.toObject().value("stops").toArray();
            r.num(v.toObject(), "angle", &l.gradAngle, -360, 360, "gradient");
        } else if (!lenient) r.bad("gradient must be a list of colors or {stops, angle}");
        if (v.isArray() || v.toObject().contains("stops")) {
            if (!lenient && (list.size() < 2 || list.size() > 4)) r.bad("gradient needs 2 to 4 stops");
            QVector<ColorStop> stops;
            for (int i = 0; i < list.size() && i < 4; ++i) {
                ColorStop s;
                s.at = list.size() > 1 ? double(i) / (list.size() - 1) : 0.0;
                if (list[i].isObject()) {
                    r.num(list[i].toObject(), "at", &s.at, 0, 1, "gradient stop");
                    r.color(list[i].toObject(), "color", &s.color, "gradient stop");
                } else {
                    s.color = QColor(list[i].toString());
                    if (!list[i].isString() || !s.color.isValid()) {
                        if (!lenient) r.bad("gradient colors must look like \"#ff8800\"");
                        continue;
                    }
                }
                if (s.color.isValid()) stops << s;
            }
            l.stops = stops;
        }
        if (!lenient && !fillGiven) l.fill = TitleFill::Gradient;
    }
    if (o.contains("texture")) {
        found = true;
        const QJsonValue v = o.value("texture");
        if (v.isString()) l.texture = v.toString();
        else if (v.isObject()) {
            const QJsonObject t = v.toObject();
            if (t.contains("path")) l.texture = t.value("path").toString();
            r.num(t, "scale", &l.texScale, 0.01, 100, "texture");
            r.num(t, "x", &l.texX, -100000, 100000, "texture");
            r.num(t, "y", &l.texY, -100000, 100000, "texture");
        } else if (!lenient) r.bad("texture must be a file path or {path, scale, x, y}");
        if (!lenient && !fillGiven) l.fill = TitleFill::Texture;
    }
    QJsonObject g;
    if (o.contains("edge3d") && (found = true) && r.group(o.value("edge3d"), "edge3d", &l.edgeOn, &g)) {
        r.num(g, "depth", &l.edgeDepth, 0, 500, "edge3d");
        r.num(g, "angle", &l.edgeAngle, -360, 360, "edge3d");
        r.color(g, "color", &l.edgeColor, "edge3d");
        r.flag(g, "light", &l.edgeLight, "edge3d");
    }
    if (o.contains("shadow") && (found = true) && r.group(o.value("shadow"), "shadow", &l.shadowOn, &g)) {
        r.num(g, "x", &l.shadowX, -1000, 1000, "shadow");
        r.num(g, "y", &l.shadowY, -1000, 1000, "shadow");
        r.num(g, "soft", &l.shadowSoft, 0, 500, "shadow");
        r.color(g, "color", &l.shadowColor, "shadow");
        r.num(g, "opacity", &l.shadowOpacity, 0, 1, "shadow");
    }
    if (o.contains("glow") && (found = true) && r.group(o.value("glow"), "glow", &l.glowOn, &g)) {
        r.num(g, "radius", &l.glowRadius, 0, 500, "glow");
        r.color(g, "color", &l.glowColor, "glow");
        r.num(g, "strength", &l.glowStrength, 0, 1, "glow");
    }
    if (o.contains("text3d") && (found = true) && r.group(o.value("text3d"), "text3d", &l.solidOn, &g)) {
        r.num(g, "depth", &l.solidDepth, 0, 1000, "text3d");
        r.num(g, "bevel", &l.solidBevel, 0, 100, "text3d");
        r.num(g, "rot_x", &l.solidRotX, -360, 360, "text3d");
        r.num(g, "rot_y", &l.solidRotY, -360, 360, "text3d");
        r.num(g, "rot_z", &l.solidRotZ, -360, 360, "text3d");
        r.num(g, "fov", &l.solidFov, 1, 150, "text3d");
        r.num(g, "distance", &l.solidDistance, 0.1, 20, "text3d");
        r.num(g, "light_angle", &l.solidLightAngle, -360, 360, "text3d");
        r.num(g, "light_height", &l.solidLightHeight, -90, 90, "text3d");
        r.color(g, "side", &l.solidSide, "text3d");
        r.num(g, "shine", &l.solidShine, 0, 1, "text3d");
        r.num(g, "metal", &l.solidMetal, 0, 1, "text3d");
    }
    if (any) *any = found;
    if (!r.err.isEmpty() && !lenient) {
        if (err) *err = r.err;
        return false;
    }
    out = l;
    return true;
}

} // namespace TitleLookJson
