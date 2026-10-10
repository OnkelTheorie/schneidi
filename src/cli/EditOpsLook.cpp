// schneidi-cli edit operations for the look of clips: Color page grade, Inspector transform/crop/opacity,
// keyframes of any animatable value and Paste Attributes. Same Editor/Keys functions as the app.
#include "cli/CommandsDetail.h"

#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/TimelineOps.h"
#include "engine/ColorGrade.h"

#include <QFileInfo>

#include <cmath>

namespace Cli::detail {

namespace {

// Animatable values with names for people/AIs (the project file ids are camelCase); effect parameters use
// "<effect>.<key>" (e.g. "grade.liftY", "blur.strength")
struct NamedParam {
    const char* name;
    AnimParam param;
};
constexpr NamedParam kNamed[] = {
    {"zoom_x", AnimParam::ZoomX},           {"zoom_y", AnimParam::ZoomY},
    {"x", AnimParam::PosX},                 {"y", AnimParam::PosY},
    {"rotation", AnimParam::Rotation},      {"crop_left", AnimParam::CropLeft},
    {"crop_right", AnimParam::CropRight},   {"crop_top", AnimParam::CropTop},
    {"crop_bottom", AnimParam::CropBottom}, {"opacity", AnimParam::Opacity},
    {"title_size", AnimParam::TitleSize},   {"title_x", AnimParam::TitlePosX},
    {"title_y", AnimParam::TitlePosY},      {"title_color", AnimParam::TitleColor},
    {"volume", AnimParam::Volume},          {"pan", AnimParam::Pan},
};

constexpr const char* kEaseNames[] = {"linear", "ease_in", "ease_out", "ease_in_out", "bezier"};

// Clip frame (0 = first frame of the clip) of timeline frame `at`, kept inside the clip
int localFrame(const Clip& c, int at) { return std::clamp(at - c.start, 0, c.length() - 1); }

void clampToRange(AnimParam p, double* v)
{
    double lo = 0, hi = 0;
    Keys::range(p, &lo, &hi);
    if (lo < hi) *v = std::clamp(*v, lo, hi);
}

// Value of a JSON field for parameter p (colors as "#rrggbb")
double paramValue(AnimParam p, const QJsonValue& v, const QString& name)
{
    if (Keys::info(p).color) return Keys::fromColor(colorOf(v, name));
    if (!v.isDouble()) fail("BAD_ARGUMENT", QString("'%1' must be a number").arg(name));
    double d = v.toDouble();
    clampToRange(p, &d);
    return d;
}

QVector<int> withGrade(Session& s, const QVector<int>& ids)
{
    QVector<int> out;
    for (int id : ids)
        if (const Clip* c = TimelineOps::findClip(s.project.timeline(), id); c && EffectRegistry::has(*c, ColorGrade::EffectId))
            out << id;
    return out;
}

// ---------- color ----------

// lift/gamma/gain/offset: a number (master) or {y, r, g, b}
void wheel(const QJsonObject& op, const char* name, const AnimParam params[4], QVector<QPair<AnimParam, double>>* out)
{
    const QJsonValue v = op.value(name);
    if (v.isUndefined()) return;
    static const char* channels[] = {"y", "r", "g", "b"};
    const auto add = [&](int i, const QJsonValue& x) {
        out->append({params[i], paramValue(params[i], x, QString("%1.%2").arg(name, channels[i]))});
    };
    if (v.isDouble()) return add(0, v);
    if (!v.isObject()) fail("BAD_ARGUMENT", QString("'%1' must be a number (master) or {y, r, g, b}").arg(name));
    const QJsonObject o = v.toObject();
    for (auto it = o.begin(); it != o.end(); ++it) {
        const int i = int(std::find(std::begin(channels), std::end(channels), it.key().toLower()) - std::begin(channels));
        if (i >= 4) fail("BAD_ARGUMENT", QString("'%1' has y, r, g and b, not '%2'").arg(name, it.key()));
        add(i, it.value());
    }
}

void opColor(Session& s, const QJsonObject& op)
{
    const QVector<int> ids = onTracks(s, clipIds(s, op), TrackKind::Video, "color corrections");
    Editor& ed = s.editor;
    const QJsonValue reset = op.value("reset");
    if (reset.isBool() && reset.toBool()) {
        ed.resetGrade(ids, {}, 0, "Reset grade");
    } else if (reset.isArray()) {
        // Single values back to their default (keyframed ones at `at`)
        QVector<AnimParam> params;
        for (const QJsonValue& v : reset.toArray()) {
            const EffectDescriptor* d = EffectRegistry::find(ColorGrade::EffectId);
            const auto it = std::find_if(d->params.begin(), d->params.end(),
                                         [&](const EffectParam& p) { return p.key == v.toString(); });
            if (it == d->params.end() || it->anim == AnimParam::Count)
                fail("BAD_ARGUMENT", "reset names grade values like \"liftY\", \"gainR\", \"saturation\", got " + v.toString());
            params << it->anim;
        }
        for (int id : withGrade(s, ids)) {
            const Clip& c = *TimelineOps::findClip(s.project.timeline(), id);
            ed.resetGrade({id}, params, op.contains("at") ? timeOf(s, op, "at") : c.start, "Reset grade");
        }
    }

    using P = AnimParam;
    QVector<QPair<AnimParam, double>> values;
    static const P lift[4] = {P::GradeLiftY, P::GradeLiftR, P::GradeLiftG, P::GradeLiftB};
    static const P gamma[4] = {P::GradeGammaY, P::GradeGammaR, P::GradeGammaG, P::GradeGammaB};
    static const P gain[4] = {P::GradeGainY, P::GradeGainR, P::GradeGainG, P::GradeGainB};
    static const P offset[4] = {P::GradeOffsetY, P::GradeOffsetR, P::GradeOffsetG, P::GradeOffsetB};
    wheel(op, "lift", lift, &values);
    wheel(op, "gamma", gamma, &values);
    wheel(op, "gain", gain, &values);
    wheel(op, "offset", offset, &values);
    const QPair<const char*, P> sliders[] = {{"contrast", P::GradeContrast},     {"pivot", P::GradePivot},
                                             {"saturation", P::GradeSaturation}, {"temperature", P::GradeTemp},
                                             {"tint", P::GradeTint},             {"exposure", P::GradeExposure}};
    for (const auto& [name, p] : sliders)
        if (op.contains(name)) values.append({p, paramValue(p, op.value(name), name)});
    if (!values.isEmpty()) {
        // Keyframed values get a keyframe at `at` (default: each clip's start), like turning a wheel in the app
        for (int id : ids) {
            const Clip& c = *TimelineOps::findClip(s.project.timeline(), id);
            ed.setGradeValues({id}, values, op.contains("at") ? timeOf(s, op, "at") : c.start, "Color");
        }
    }
    if (op.contains("lut")) {
        const QString name = op.value("lut").toString();
        const QString path = name.isEmpty() ? QString() : findAsset(Asset::Lut, name);
        if (!path.isEmpty()) {
            QString error;
            if (!ColorGrade::parseLut(path, &error)) fail("BAD_ARGUMENT", "cannot read the LUT: " + error);
        }
        ed.setGradeLut(ids, path);
    }
    if (op.contains("keyframe")) {
        // A keyframe of every grade value at `at` (on) or none there (off), like a keyframe on a DaVinci node
        for (int id : ids) ed.setGradeKeyframe({id}, timeOf(s, op, "at"), op.value("keyframe").toBool());
    }
    if (op.contains("enabled")) ed.setGradeEnabled(ids, op.value("enabled").toBool());
}

// ---------- transform ----------

void opTransform(Session& s, const QJsonObject& op)
{
    const QVector<int> ids = onTracks(s, clipIds(s, op), TrackKind::Video, "transforms");
    using P = AnimParam;
    QVector<QPair<AnimParam, double>> values;
    if (op.contains("zoom")) {
        const double z = paramValue(P::ZoomX, op.value("zoom"), "zoom");
        values.append({P::ZoomX, z});
        values.append({P::ZoomY, z});
    }
    const QPair<const char*, P> fields[] = {{"zoom_x", P::ZoomX},         {"zoom_y", P::ZoomY},
                                            {"x", P::PosX},               {"y", P::PosY},
                                            {"rotation", P::Rotation},    {"crop_left", P::CropLeft},
                                            {"crop_right", P::CropRight}, {"crop_top", P::CropTop},
                                            {"crop_bottom", P::CropBottom}, {"opacity", P::Opacity}};
    for (const auto& [name, p] : fields)
        if (op.contains(name)) values.append({p, paramValue(p, op.value(name), name)});
    const bool reset = op.value("reset").toBool();
    if (values.isEmpty() && !reset) fail("BAD_ARGUMENT", "nothing to set (zoom, x, y, rotation, crop_*, opacity or reset)");
    const bool hasAt = op.contains("at");
    const int at = hasAt ? timeOf(s, op, "at") : 0;
    s.editor.modifyClips(ids, "Transform", [&](Clip& c) {
        if (reset) {
            for (const auto& [name, p] : fields) Keys::clear(c, p);
            c.transform = ClipTransform{};
        }
        for (const auto& [p, v] : values) Keys::setValue(c, p, hasAt ? localFrame(c, at) : 0, v);
    });
}

// ---------- keyframes ----------

AnimParam parseParam(const QString& name)
{
    for (const NamedParam& n : kNamed)
        if (name == QLatin1String(n.name)) return n.param;
    const int dot = name.indexOf('.');
    if (dot > 0) {
        const QString effectId = name.left(dot), key = name.mid(dot + 1);
        const EffectDescriptor* d = EffectRegistry::find(effectId);
        if (!d) fail("NOT_FOUND", QString("no effect '%1' (see `effects`)").arg(effectId));
        for (const EffectParam& p : d->params)
            if (p.key == key) {
                if (p.anim == AnimParam::Count) fail("BAD_ARGUMENT", QString("%1 cannot be animated").arg(name));
                return p.anim;
            }
        fail("BAD_ARGUMENT", QString("effect %1 has no parameter '%2'").arg(effectId, key));
    }
    QStringList names;
    for (const NamedParam& n : kNamed) names << n.name;
    fail("BAD_ARGUMENT", QString("unknown parameter '%1' (use %2 or <effect>.<key> like grade.liftY)").arg(name, names.join(", ")));
}

KeyEase parseEase(const QJsonValue& v)
{
    const QString s = v.toString();
    for (int i = 0; i < int(std::size(kEaseNames)) - 1; ++i) // bezier needs handles: not offered
        if (s == QLatin1String(kEaseNames[i])) return KeyEase(i);
    fail("BAD_ARGUMENT", "ease must be linear, ease_in, ease_out or ease_in_out");
}

void opKeyframe(Session& s, const QJsonObject& op)
{
    const AnimParam p = parseParam(op.value("param").toString());
    const bool audioParam = p == AnimParam::Volume || p == AnimParam::Pan;
    const QVector<int> ids = onTracks(s, clipIds(s, op), audioParam ? TrackKind::Audio : TrackKind::Video, "these keyframes");
    QString effectId;
    const EffectDescriptor* effect = nullptr;
    const EffectParam* ep = nullptr;
    if (EffectRegistry::paramFor(p, &effect, &ep)) effectId = effect->id;

    if (op.value("clear").toBool()) {
        s.editor.modifyClips(ids, "Remove keyframes", [&](Clip& c) { Keys::clear(c, p); });
        return;
    }
    // Several keyframes at once: keys:[{at, value, ease?}, …]; one: at, value?, ease?, remove?
    QJsonArray keys = op.value("keys").toArray();
    if (keys.isEmpty()) {
        QJsonObject one{{"at", op.value("at")}};
        for (const char* k : {"value", "ease", "remove"})
            if (op.contains(k)) one[k] = op.value(k);
        keys << one;
    }
    struct Key {
        int at;
        std::optional<double> value;
        std::optional<KeyEase> ease;
        bool remove;
    };
    QVector<Key> list;
    for (const QJsonValue& v : keys) {
        const QJsonObject k = v.toObject();
        Key key{timeOf(s, k, "at"), {}, {}, k.value("remove").toBool()};
        if (k.contains("value")) key.value = paramValue(p, k.value("value"), "value");
        if (k.contains("ease")) key.ease = parseEase(k.value("ease"));
        list << key;
    }
    s.editor.modifyClips(ids, "Keyframe", [&](Clip& c) {
        if (!effectId.isEmpty() && !EffectRegistry::has(c, effectId)) EffectRegistry::add(c, effectId);
        for (const Key& k : list) {
            const int t = localFrame(c, k.at);
            if (k.remove) {
                Keys::removeKey(c, p, t);
                continue;
            }
            // Without a value: keyframe with the value the clip has there now (like clicking the diamond)
            Keys::setKey(c, p, t, k.value ? *k.value : Keys::valueAt(c, p, t));
            if (k.ease) Keys::setEase(c, {t}, *k.ease, {p});
        }
    });
}

// ---------- copy_attributes ----------

void opCopyAttributes(Session& s, const QJsonObject& op)
{
    static const QPair<const char*, int> attrs[] = {
        {"zoom", Editor::AttrZoom},       {"position", Editor::AttrPosition}, {"rotation", Editor::AttrRotation},
        {"crop", Editor::AttrCrop},       {"opacity", Editor::AttrComposite}, {"effects", Editor::AttrEffects},
        {"color", Editor::AttrColor},     {"speed", Editor::AttrSpeed},       {"volume", Editor::AttrVolume},
        {"pan", Editor::AttrPan},         {"fades", Editor::AttrFades}};
    int mask = 0;
    const QJsonArray names = op.value("attributes").toArray();
    for (const QJsonValue& v : names) {
        const auto it = std::find_if(std::begin(attrs), std::end(attrs), [&](const auto& a) { return v.toString() == a.first; });
        if (it == std::end(attrs))
            fail("BAD_ARGUMENT", "attributes are zoom, position, rotation, crop, opacity, effects, color, speed, volume, pan, fades; got " + v.toString());
        mask |= it->second;
    }
    if (names.isEmpty()) mask = Editor::AttrVideoMask | Editor::AttrAudioMask | Editor::AttrFades;
    const QVector<int> from = clipIds(s, QJsonObject{{"clip", op.value("from")}, {"linked", op.value("linked")}});
    const QVector<int> to = clipIds(s, op);
    s.selection.set(QSet<int>(from.begin(), from.end()));
    s.editor.copySelection();
    s.selection.set(QSet<int>(to.begin(), to.end()));
    s.editor.pasteAttributes(mask & s.editor.pasteAttributesAvailable());
}

} // namespace

QString paramName(AnimParam p)
{
    for (const NamedParam& n : kNamed)
        if (n.param == p) return n.name;
    const EffectDescriptor* d = nullptr;
    const EffectParam* ep = nullptr;
    if (EffectRegistry::paramFor(p, &d, &ep)) return d->id + "." + ep->key;
    return Keys::info(p).id;
}

QJsonObject keyframesJson(const Clip& c)
{
    QJsonObject o;
    for (auto it = c.keys.begin(); it != c.keys.end(); ++it) {
        if (it.value().isEmpty()) continue;
        QJsonArray list;
        for (const Keyframe& k : it.value()) {
            QJsonObject ko{{"at", c.start + k.frame - c.in}};
            ko["value"] = Keys::info(it.key()).color ? QJsonValue(Keys::toColor(k.value).name(QColor::HexArgb))
                                                     : QJsonValue(std::round(k.value * 10000) / 10000);
            if (k.ease != KeyEase::Linear) ko["ease"] = kEaseNames[int(k.ease)];
            list << ko;
        }
        o[paramName(it.key())] = list;
    }
    return o;
}

void addLookOps(QVector<OpDef>& ops)
{
    ops << OpDef{"color", "{op:'color', clips, lift?, gamma?, gain?, offset?, contrast?, pivot?, saturation?, "
                          "temperature?, tint?, exposure?, lut?, enabled?, reset?, at?, keyframe?}",
                 "Color page grade (primary wheels like DaVinci, LUT)",
                 "Wheels lift/gamma/gain/offset: a number (master) or {y, r, g, b}. Neutral: lift 0, gamma 0, gain 1, "
                 "offset 25 (ranges: lift/gamma -1..1, gain 0..4, offset 0..100). contrast 0..2 (1), pivot 0..1 "
                 "(0.435), saturation 0..100 (50), temperature -4000..4000 (0, + = warmer), tint -100..100 (0, + = "
                 "magenta), exposure -4..4 stops (0). lut: name from `effects` or a .cube/.3dl/.csp/Hald image file, \"\" removes it. "
                 "reset:true removes the whole grade, reset:[\"liftY\", …] sets single values back. Values that "
                 "have keyframes get one at `at` (default: clip start); keyframe:true/false with `at` sets/removes a "
                 "keyframe of every value there. Single grade values animate with {op:'keyframe', param:'grade.gainY'}.",
                 opColor}
        << OpDef{"transform", "{op:'transform', clips, zoom?, zoom_x?, zoom_y?, x?, y?, rotation?, crop_left?, "
                              "crop_right?, crop_top?, crop_bottom?, opacity?, reset?, at?}",
                 "Inspector transform/crop/opacity of video clips",
                 "zoom 1 = 100 %; x/y in pixels of the timeline from the center, y up; rotation in degrees; crop in "
                 "pixels; opacity 0..100. reset:true sets all back first. Values that have keyframes get one at `at`.",
                 opTransform}
        << OpDef{"keyframe", "{op:'keyframe', clips, param, at, value?, ease?, remove?} or {op:'keyframe', clips, "
                             "param, keys:[{at, value, ease?}, …]} or {op:'keyframe', clips, param, clear:true}",
                 "animate a value",
                 "param: zoom_x, zoom_y, x, y, rotation, crop_left, crop_right, crop_top, crop_bottom, opacity, "
                 "title_size, title_x, title_y, title_color (\"#rrggbb\"), volume (dB, audio clips), pan (-100..100, "
                 "audio clips) or an effect parameter \"<effect>.<key>\" (keys: `effects <id>`; grade values: "
                 "grade.liftY, grade.gainR, grade.saturation, …). at = timeline frame inside the clip. Without value "
                 "the keyframe keeps the current value there. ease (from this keyframe on): linear, ease_in, ease_out, "
                 "ease_in_out. Between keyframes the value is interpolated; with no keyframes left the static value "
                 "applies. Times in `info` keyframes are timeline frames.",
                 opKeyframe}
        << OpDef{"copy_attributes", "{op:'copy_attributes', from, clips, attributes?}",
                 "Paste Attributes: copy values of clip `from` to the clips",
                 "attributes (default all): zoom, position, rotation, crop, opacity, effects, color (grade), speed, "
                 "volume, pan, fades. Keyframes come along.",
                 opCopyAttributes};
}

} // namespace Cli::detail
