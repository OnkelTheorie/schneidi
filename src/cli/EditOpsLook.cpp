// schneidi-cli edit operations for the look of clips: Color page grade, Inspector transform/crop/opacity,
// keyframes of any animatable value and Paste Attributes. Same Editor/Keys functions as the app.
#include "cli/CommandsDetail.h"

#include "core/Accents.h"
#include "core/EffectRegistry.h"
#include "core/GradeWindows.h"
#include "core/Keyframes.h"
#include "core/TimelineOps.h"
#include "engine/ColorGrade.h"
#include "engine/Tracker.h"

#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <optional>

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

// A transform value without `at`: with keyframes it is the base of the curve, the whole curve moves so that the clip's
// first frame gets the value - zoom scaled (an accent's punch stays the same punch around the new zoom), the others
// shifted (a shake stays a shake around the new place); without keyframes the static value
void setBase(Clip& c, AnimParam p, double v)
{
    const bool animated = Keys::animated(c, p);
    const double old = animated ? Keys::valueAt(c, p, 0) : 0;
    Keys::setStaticValue(c, p, v); // also with keyframes: the value without them, what `info` shows as transform
    if (!animated) return;
    const bool scale = (p == AnimParam::ZoomX || p == AnimParam::ZoomY) && std::abs(old) > 1e-9;
    for (Keyframe& k : c.keys[p]) {
        k.value = scale ? k.value * v / old : k.value + v - old;
        clampToRange(p, &k.value);
    }
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

// Wheels and sliders of a grade op (lift, gamma, gain, offset, contrast, pivot, saturation, temperature, tint,
// exposure) for the parameters `list` in the order of ColorGrade::animParams(): the grade's own or a window's
QVector<QPair<AnimParam, double>> gradeValues(const QJsonObject& op, const QVector<AnimParam>& list)
{
    QVector<QPair<AnimParam, double>> values;
    wheel(op, "lift", &list[0], &values);
    wheel(op, "gamma", &list[4], &values);
    wheel(op, "gain", &list[8], &values);
    wheel(op, "offset", &list[12], &values);
    static const char* sliders[] = {"contrast", "pivot", "saturation", "temperature", "tint", "exposure"};
    for (int i = 0; i < 6; ++i)
        if (op.contains(sliders[i])) values.append({list[16 + i], paramValue(list[16 + i], op.value(sliders[i]), sliders[i])});
    return values;
}

void opColor(Session& s, const QJsonObject& op)
{
    const QVector<int> ids = clipsOn(s, op, TrackKind::Video, "color corrections");
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

    const QVector<QPair<AnimParam, double>> values = gradeValues(op, ColorGrade::animParams());
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

// ---------- windows (shape masks of the grade) ----------

void opWindow(Session& s, const QJsonObject& op)
{
    const QVector<int> ids = clipsOn(s, op, TrackKind::Video, "windows");
    Editor& ed = s.editor;
    const auto clip = [&](int id) { return TimelineOps::findClip(s.project.timeline(), id); };
    int n = 0;
    if (op.contains("window")) {
        n = op.value("window").toInt();
        if (n < 1 || n > GradeWindows::kMax) fail("BAD_ARGUMENT", "window is a number 1..4");
    }
    if (op.contains("add")) {
        const QString shape = op.value("add").toString();
        GradeWindows::Shape sh;
        if (!GradeWindows::shapeFromId(shape, &sh)) fail("BAD_ARGUMENT", "add is \"rectangle\" or \"ellipse\"");
        for (int k = 1; k <= GradeWindows::kMax && !n; ++k) // first number free on all clips
            if (std::none_of(ids.begin(), ids.end(), [&](int id) { return GradeWindows::exists(*clip(id), k); })) n = k;
        if (!n) fail("BAD_ARGUMENT", "no free window on these clips (at most 4 per clip)");
        ed.addGradeWindow(ids, n, shape);
        s.notes << QString("window %1 added").arg(n);
    }
    if (!n) fail("BAD_ARGUMENT", "window (1..4) is missing; add a new one with add:'ellipse' or add:'rectangle'");
    QVector<int> with;
    for (int id : ids)
        if (GradeWindows::exists(*clip(id), n)) with << id;
    if (with.isEmpty()) fail("NOT_FOUND", QString("no clip has window %1 (add one with add)").arg(n));
    if (op.value("remove").toBool()) {
        ed.removeGradeWindow(with, n);
        return;
    }
    if (op.contains("shape")) {
        GradeWindows::Shape sh;
        if (!GradeWindows::shapeFromId(op.value("shape").toString(), &sh))
            fail("BAD_ARGUMENT", "shape is \"rectangle\" or \"ellipse\"");
        ed.setGradeWindowOption(with, n, "shape", op.value("shape").toString());
    }
    if (op.contains("invert")) ed.setGradeWindowOption(with, n, "invert", op.value("invert").toBool());
    if (op.contains("fill")) {
        GradeWindows::Fill f;
        if (!GradeWindows::fillFromId(op.value("fill").toString(), &f))
            fail("BAD_ARGUMENT", "fill is \"none\", \"blur\", \"pixelate\" or \"color\"");
        ed.setGradeWindowOption(with, n, "fill", op.value("fill").toString());
    }
    if (op.contains("color")) ed.setGradeWindowOption(with, n, "color", colorOf(op.value("color"), "color"));
    QVector<QPair<AnimParam, double>> values;
    for (const QString& k : GradeWindows::geometryKeys())
        if (op.contains(k)) {
            const AnimParam p = GradeWindows::param(n, k);
            values.append({p, paramValue(p, op.value(k), k)});
        }
    values += gradeValues(op, ColorGrade::windowParams(n));
    if (!values.isEmpty())
        for (int id : with)
            ed.setGradeValues({id}, values, op.contains("at") ? timeOf(s, op, "at") : clip(id)->start, "Window");
    if (op.contains("keyframe"))
        for (int id : with) ed.setGradeKeyframe({id}, timeOf(s, op, "at"), op.value("keyframe").toBool(), n);
}

void opTrack(Session& s, const QJsonObject& op)
{
    const QVector<int> ids = clipsOn(s, op, TrackKind::Video, "tracking");
    const int n = op.value("window").toInt();
    if (n < 1 || n > GradeWindows::kMax) fail("BAD_ARGUMENT", "window is a number 1..4");
    for (int id : ids) {
        const Clip c = *TimelineOps::findClip(s.project.timeline(), id);
        if (!GradeWindows::exists(c, n)) fail("NOT_FOUND", QString("clip %1 has no window %2").arg(id).arg(n));
        const int from = op.contains("from") ? localFrame(c, timeOf(s, op, "from")) : 0;
        const int to = op.contains("to") ? localFrame(c, timeOf(s, op, "to")) : c.length() - 1;
        const Tracker::Result r = Tracker::trackWindow(s.format(), c, n, from, to);
        if (!r.error.isEmpty()) fail("BAD_ARGUMENT", r.error);
        s.editor.setGradeWindowPath(id, n, from, r.points);
        QString note = QString("clip %1 window %2: tracked %3 of %4 frames from %5")
                           .arg(id).arg(n).arg(r.points.size()).arg(std::abs(to - from))
                           .arg(tc(c.start + from, s.format()));
        if (r.lost)
            note += QString(", lost at %1 (the window stays there; look with `frames`, move it and track again from "
                            "there)").arg(tc(c.start + r.lostAt, s.format()));
        s.notes << note;
    }
}

// ---------- transform ----------

void opTransform(Session& s, const QJsonObject& op)
{
    const QVector<int> ids = clipsOn(s, op, TrackKind::Video, "transforms");
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
    // The Inspector's section switches: off keeps the values but does not apply them
    const QPair<const char*, bool ClipTransform::*> switches[] = {
        {"transform_on", &ClipTransform::transformOn}, {"crop_on", &ClipTransform::cropOn},
        {"opacity_on", &ClipTransform::compositeOn}};
    bool anySwitch = false;
    for (const auto& [name, member] : switches) {
        if (!op.contains(name)) continue;
        if (!op.value(name).isBool()) fail("BAD_ARGUMENT", QString("%1 must be true or false").arg(name));
        anySwitch = true;
    }
    // Fill around the fitted picture: 'blur' = a blurred copy of the same picture (titles fill the frame: skipped)
    std::optional<FitFill> fill;
    if (op.contains("fit")) {
        for (const FitFillInfo& f : kFitFills)
            if (op.value("fit").toString() == QLatin1String(f.id)) fill = f.fill;
        if (!fill) fail("BAD_ARGUMENT", "fit is 'blur' (a blurred copy of the picture around it) or 'none' (transparent)");
    }
    const std::optional<double> fillBlur = op.contains("fit_blur") ? std::optional(numberOf(op, "fit_blur", 0, 100))
                                                                   : std::nullopt;
    const std::optional<double> fillDim = op.contains("fit_dim") ? std::optional(numberOf(op, "fit_dim", 0, 100))
                                                                 : std::nullopt;
    const bool anyFill = fill || fillBlur || fillDim;
    if (anyFill) {
        const Timeline& tl = s.project.timeline();
        if (std::all_of(ids.begin(), ids.end(), [&](int id) { return TimelineOps::findClip(tl, id)->isTitle(); }))
            fail("BAD_ARGUMENT", "fit is for clips with a picture; titles fill the frame");
    }
    if (values.isEmpty() && !reset && !anySwitch && !anyFill)
        fail("BAD_ARGUMENT", "nothing to set (zoom, x, y, rotation, crop_*, opacity, fit, fit_blur, fit_dim, *_on or reset)");
    const bool hasAt = op.contains("at");
    const int at = hasAt ? timeOf(s, op, "at") : 0;
    s.editor.modifyClips(ids, "Transform", [&](Clip& c) {
        if (reset) {
            for (const auto& [name, p] : fields) Keys::clear(c, p);
            c.transform = ClipTransform{};
        }
        for (const auto& [p, v] : values) {
            if (hasAt) Keys::setValue(c, p, localFrame(c, at), v);
            else setBase(c, p, v);
        }
        for (const auto& [name, member] : switches)
            if (op.contains(name)) c.transform.*member = op.value(name).toBool();
        if (!c.isTitle()) {
            if (fill) c.transform.fill = *fill;
            if (fillBlur) c.transform.fillBlur = *fillBlur;
            if (fillDim) c.transform.fillDim = *fillDim;
        }
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
    QStringList names{"zoom"};
    for (const NamedParam& n : kNamed) names << n.name;
    fail("BAD_ARGUMENT", QString("unknown parameter '%1' (use %2 or <effect>.<key> like grade.liftY)").arg(name, names.join(", ")));
}

// "zoom" = zoom_x and zoom_y together (like the linked Zoom fields in the Inspector), otherwise one parameter
QVector<AnimParam> parseParams(const QString& name)
{
    if (name == QLatin1String("zoom")) return {AnimParam::ZoomX, AnimParam::ZoomY};
    return {parseParam(name)};
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
    // A key this op does not know was dropped without a word ("keyframes":[…], "time"): the op ran and nothing moved
    const auto checkKeys = [](const QJsonObject& o, const QStringList& known, const char* where) {
        for (auto it = o.begin(); it != o.end(); ++it)
            if (!known.contains(it.key()))
                fail("BAD_ARGUMENT", QString("keyframe: unknown key '%1' %2(keys: %3)").arg(it.key(), where, known.join(", ")));
    };
    checkKeys(op, {"op", "clips", "clip", "linked", "param", "at", "value", "ease", "remove", "move_to", "keys", "clear"}, "");
    if (!op.contains("param")) fail("BAD_ARGUMENT", "keyframe: missing 'param' (zoom, x, y, opacity, … see `help edit --op keyframe`)");
    const QVector<AnimParam> params = parseParams(op.value("param").toString());
    const AnimParam p = params.first(); // value checks: the parameters of "zoom" share their range
    const bool audioParam = p == AnimParam::Volume || p == AnimParam::Pan;
    QVector<int> ids = clipsOn(s, op, audioParam ? TrackKind::Audio : TrackKind::Video, "these keyframes");
    QString effectId;
    const EffectDescriptor* effect = nullptr;
    const EffectParam* ep = nullptr;
    if (EffectRegistry::paramFor(p, &effect, &ep)) effectId = effect->id;

    if (op.value("clear").toBool()) {
        s.editor.modifyClips(ids, "Remove keyframes", [&](Clip& c) {
            for (AnimParam q : params) Keys::clear(c, q);
        });
        return;
    }
    // Several keyframes at once: keys:[{at, value, ease?}, …]; one: at, value?, ease?, remove?
    QJsonArray keys = op.value("keys").toArray();
    if (keys.isEmpty()) {
        QJsonObject one{{"at", op.value("at")}};
        for (const char* k : {"value", "ease", "remove", "move_to"})
            if (op.contains(k)) one[k] = op.value(k);
        keys << one;
    }
    struct Key {
        int at;
        std::optional<double> value;
        std::optional<KeyEase> ease;
        bool remove;
        std::optional<int> moveTo; // timeline frame the keyframe at `at` moves to
    };
    QVector<Key> list;
    for (const QJsonValue& v : keys) {
        const QJsonObject k = v.toObject();
        checkKeys(k, {"at", "value", "ease", "remove", "move_to"}, "in keys ");
        Key key{timeOf(s, k, "at"), {}, {}, k.value("remove").toBool(), {}};
        if (k.contains("value")) key.value = paramValue(p, k.value("value"), "value");
        if (k.contains("ease")) key.ease = parseEase(k.value("ease"));
        if (k.contains("move_to")) {
            key.moveTo = timeOf(s, k, "move_to");
            for (int id : ids) {
                const Clip* c = TimelineOps::findClip(s.project.timeline(), id);
                if (c && std::none_of(params.begin(), params.end(),
                                      [&](AnimParam q) { return Keys::keyAt(*c, q, localFrame(*c, key.at)); }))
                    fail("NOT_FOUND", QString("clip %1 has no %2 keyframe at %3 to move")
                                          .arg(id).arg(op.value("param").toString(), tc(key.at, s.format())));
                if (c && (*key.moveTo < c->start || *key.moveTo >= c->end()))
                    fail("BAD_ARGUMENT", QString("move_to %1 is outside clip %2").arg(tc(*key.moveTo, s.format())).arg(id));
            }
        }
        list << key;
    }
    // Times are timeline frames: a time outside a clip was pulled to its edge, so keys meant from the clip start
    // ("0s", "2s" on a clip at 6 s) all landed on its first frame - no animation, one key. The clip's end counts as its
    // last frame. A clip none of the times is in is left alone (one op for clips at different places); none at all
    // is an error
    const Timeline& tl = s.project.timeline();
    const auto inside = [&](int id, int at) {
        const Clip* c = TimelineOps::findClip(tl, id);
        return c && at >= c->start && at <= c->end();
    };
    QVector<int> hit, missed;
    for (int id : ids) {
        if (std::all_of(list.begin(), list.end(), [&](const Key& k) { return inside(id, k.at); })) hit << id;
        else missed << id;
    }
    if (!missed.isEmpty()) {
        QStringList where;
        for (int id : missed)
            if (const Clip* c = TimelineOps::findClip(tl, id))
                where << QString("clip %1 is %2-%3").arg(id).arg(tc(c->start, s.format()), tc(c->end(), s.format()));
        const QString why = QString("keyframe times are timeline frames inside the clip (%1); from the clip start: "
                                    "at:{clip:<id>, offset:\"2s\"}")
                                .arg(where.join(", "));
        if (hit.isEmpty() || std::any_of(missed.begin(), missed.end(), [&](int id) {
                return std::any_of(list.begin(), list.end(), [&](const Key& k) { return inside(id, k.at); });
            }))
            fail("BAD_ARGUMENT", "a keyframe time is outside the clip: " + why);
        s.notes << "keyframe: left alone where no time falls inside: " + why;
        ids = hit;
    }
    s.editor.modifyClips(ids, "Keyframe", [&](Clip& c) {
        if (!effectId.isEmpty() && !EffectRegistry::has(c, effectId)) EffectRegistry::add(c, effectId);
        for (const Key& k : list) {
            const int t = localFrame(c, k.at);
            if (k.remove) {
                for (AnimParam q : params) Keys::removeKey(c, q, t);
                continue;
            }
            if (k.moveTo) { // keeps value, ease and handles; a keyframe already at the target is replaced
                Keys::move(c, {t}, localFrame(c, *k.moveTo) - t, params);
                continue;
            }
            // Without a value: keyframe with the value the clip has there now (like clicking the diamond)
            for (AnimParam q : params) Keys::setKey(c, q, t, k.value ? *k.value : Keys::valueAt(c, q, t));
            if (k.ease) Keys::setEase(c, {t}, *k.ease, params);
        }
    });
}

// ---------- accent ----------

// Zoom punch / flash / shake on hits as keyframes (core/Accents.h), all times of one op in one go
void opAccent(Session& s, const QJsonObject& op)
{
    const QString id = op.value("preset").toString("hit");
    const Accents::Preset* preset = Accents::find(id);
    if (!preset) {
        QStringList ids;
        for (const Accents::Preset& p : Accents::presets()) ids << p.id;
        fail("BAD_ARGUMENT", QString("no accent preset '%1' (%2; `effects` lists them)").arg(id, ids.join(", ")));
    }
    Accents::Amounts a = preset->amounts;
    if (op.contains("punch")) a.punch = numberOf(op, "punch", -0.5, 2);
    if (op.contains("flash")) a.flash = numberOf(op, "flash", -4, 4);
    if (op.contains("shake")) a.shake = numberOf(op, "shake", 0, 0.25);
    if (op.contains("strength")) a = a.scaled(numberOf(op, "strength", 0, 5));
    if (op.contains("length")) {
        const int frames = timeOf(s, op, "length");
        if (frames <= 0) fail("BAD_ARGUMENT", "'length' must be longer than 0");
        a.length = seconds(frames, s.format());
    }
    if (!a.any()) fail("BAD_ARGUMENT", "nothing to do: punch, flash and shake are all 0");

    QVector<int> frames;
    const QJsonValue at = op.value("at");
    const QJsonArray times = at.isArray() ? at.toArray() : at.isUndefined() ? QJsonArray{} : QJsonArray{at};
    for (const QJsonValue& v : times) frames << timeOf(s, QJsonObject{{"at", v}}, "at");
    if (op.value("markers").toBool()) frames += s.project.timeline().markers;
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    if (frames.isEmpty())
        fail("BAD_ARGUMENT", op.value("markers").toBool() ? "the timeline has no markers"
                                                           : "missing 'at' (a time or a list of times) or markers:true");

    const bool given = op.contains("clips") || op.contains("clip");
    const QVector<int> ids = given ? clipsOn(s, op, TrackKind::Video, "accents") : QVector<int>{};
    const QVector<int> landed = s.editor.addAccents(ids, frames, a, "Accent");
    if (landed.isEmpty())
        fail("NOT_FOUND", given ? "none of the times lies inside the given clips"
                                : "no video clip at those times (titles, disabled clips and hidden tracks are left out)");
    QString note = QString("accent '%1' at %2 of %3 times").arg(id).arg(landed.size()).arg(frames.size());
    QStringList missed;
    for (int f : frames)
        if (!landed.contains(f) && missed.size() < 8) missed << tc(f, s.format());
    if (!missed.isEmpty())
        note += QString("; no %1clip at %2").arg(given ? "given " : "video ", missed.join(", "));
    if (a.flash != 0) {
        const QVector<int> flashes = Accents::flashHits(frames, s.format().rate.fps());
        const auto lit = std::count_if(landed.begin(), landed.end(), [&](int f) { return flashes.contains(f); });
        if (lit < landed.size())
            note += QString("; the flash on %1 of them (at most one every %2 s, a burst would strobe)")
                        .arg(lit)
                        .arg(Accents::kFlashGap);
    }
    s.notes << note;
}

// ---------- copy_attributes ----------

void opCopyAttributes(Session& s, const QJsonObject& op)
{
    static const QPair<const char*, int> attrs[] = {
        {"zoom", Editor::AttrZoom},       {"position", Editor::AttrPosition}, {"rotation", Editor::AttrRotation},
        {"crop", Editor::AttrCrop},       {"opacity", Editor::AttrComposite}, {"color", Editor::AttrColor},
        {"speed", Editor::AttrSpeed},     {"volume", Editor::AttrVolume},     {"pan", Editor::AttrPan},
        {"fades", Editor::AttrFades},     {"effects", Editor::AttrEffects | Editor::AttrAudioEffects}};
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
    // Zoom X and Y with the same keyframes (linked zoom, op 'keyframe' param zoom, accents) show once as "zoom"
    const KeyTrack zx = c.keys.value(AnimParam::ZoomX), zy = c.keys.value(AnimParam::ZoomY);
    const bool zoom = !zx.isEmpty() && zx == zy;
    for (auto it = c.keys.begin(); it != c.keys.end(); ++it) {
        if (it.value().isEmpty() || (zoom && it.key() == AnimParam::ZoomY)) continue;
        QJsonArray list;
        for (const Keyframe& k : it.value()) {
            QJsonObject ko{{"at", c.start + k.frame - c.in}};
            ko["value"] = Keys::info(it.key()).color ? QJsonValue(Keys::toColor(k.value).name(QColor::HexArgb))
                                                     : QJsonValue(std::round(k.value * 10000) / 10000);
            if (k.ease != KeyEase::Linear) ko["ease"] = kEaseNames[int(k.ease)];
            list << ko;
        }
        o[zoom && it.key() == AnimParam::ZoomX ? QStringLiteral("zoom") : paramName(it.key())] = list;
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
        << OpDef{"window", "{op:'window', clips, window?, add?, remove?, shape?, x?, y?, width?, height?, rotation?, "
                           "softness?, invert?, fill?, amount?, color?, lift?, gamma?, gain?, offset?, contrast?, pivot?, "
                           "saturation?, temperature?, tint?, exposure?, at?, keyframe?}",
                 "Color page window (shape mask like DaVinci): grade or hide part of the picture",
                 "Up to 4 windows per clip, part of its grade (copied, reset and switched off with it). add:'ellipse' "
                 "or 'rectangle' makes a new one (number = first free one, see notes; default centered, 40 x 40 %). "
                 "x/y = center in % of the picture width/height from the top left (50/50 = middle), width/height in % "
                 "of the picture width/height, rotation in degrees (counterclockwise), softness = soft edge outside the "
                 "shape in % of the picture height (default 5), invert:true = everything outside. Inside the window "
                 "its own correction applies (lift … exposure like op 'color', neutral by default) and a fill: none, "
                 "blur (amount 0..100 like the blur effect), pixelate (amount 100 = blocks of 1/10 of the picture "
                 "height) or color (color \"#rrggbb\", covers it). Hide a face or a name: fill:'blur' or 'pixelate'. "
                 "The picture is the clip before transform/crop. Values that have keyframes get one at `at` (default "
                 "clip start); keyframe:true/false with `at` sets/removes a keyframe of every window value there. "
                 "Animate single values with {op:'keyframe', param:'grade.w1.x'}. remove:true deletes the window.",
                 opWindow}
        << OpDef{"track_window", "{op:'track_window', clips, window, from?, to?}",
                 "the window follows the object under it (tracker, position only)",
                 "Tracks what is inside window `window` from `from` to `to` (timeline frames inside the clip, default "
                 "clip start to clip end; to < from tracks backwards) and sets x/y keyframes of the window on every "
                 "frame (keyframes in that range are replaced). Place the window on the object at `from` first. "
                 "Stops where the picture no longer matches (see notes); move the window there and track again. Works "
                 "on the clip's picture before transform, speed changes included. Slow: ~10–40 frames per second.",
                 opTrack}
        << OpDef{"transform", "{op:'transform', clips, zoom?, zoom_x?, zoom_y?, x?, y?, rotation?, crop_left?, "
                              "crop_right?, crop_top?, crop_bottom?, opacity?, fit?, fit_blur?, fit_dim?, transform_on?, "
                              "crop_on?, opacity_on?, reset?, at?}",
                 "Inspector transform/crop/opacity of video clips; fit:'blur' = blurred background (9:16 from 16:9)",
                 "zoom 1 = the clip's picture fitted into the frame keeping its shape (a 1920x1080 clip in a "
                 "1080x1920 timeline is 1080x607.5 wide/high, centered, with transparent bars above and below); x/y "
                 "in pixels of the timeline from the center, y up; rotation in degrees, counterclockwise. crop_* = "
                 "timeline pixels on that fitted picture at zoom 1, cut off the picture's own edges (not the frame's), "
                 "the same unit at every zoom and timeline shape: crop px = source px * fitted width / source width "
                 "(1920x1080 clip: * 1 in a 1920x1080 timeline, * 0.5625 in 1080x1920, * 0.5 in 960x540). Crop "
                 "applies before zoom/position/rotation, so at zoom 2 a crop of 100 hides 200 timeline pixels; the "
                 "cut area is transparent (tracks below show). Cropping never moves what is left: it stays where it "
                 "was in the picture, and x/y/zoom/rotation act on the whole picture around ITS center (not the "
                 "center of the part left). To center a cropped part, move by its distance from the picture center "
                 "times zoom: x = -zoom * (crop_left - crop_right) / 2, y = zoom * (crop_top - crop_bottom) / 2. "
                 "Example, a 9:16 timeline (1080x1920) with a 1920x1080 clip (fitted 1080x607.5, crop px = source px * "
                 "0.5625) showing only the facecam at source x 1440..1920, y 0..360, centered and full width: "
                 "crop_left 810, crop_bottom 405, zoom 4 (270 px left * 4 = 1080), x -1620, y -810. opacity 0..100. "
                 "fit:'blur' fills the frame around the fitted picture (bars, room left by zoom < 1, moves, rotation) "
                 "with a blurred copy of the same picture, enlarged to cover the frame: a vertical video from 16:9 "
                 "material on ONE track (one clip per scene, decoded once, plays smoothly), instead of a blurred second "
                 "copy on the track below. The copy shows the clip's whole (cropped) picture and does not zoom or move "
                 "with it; zoom 1.2-1.5 crops the edges of the sharp picture and keeps a HUD readable. fit_blur 0..100 "
                 "(default 70), fit_dim 0..100 = percent darker than the clip (default 20); "
                 "fit:'none' = transparent again (default). Opacity and fades act on the whole clip; titles do not "
                 "take it. reset:true sets all back first. "
                 "Values that have keyframes: with `at` a keyframe there; without `at` the whole curve follows (zoom "
                 "scaled, the others shifted by the difference at the clip start), so accents keep their shape around "
                 "a new zoom or place. transform_on/crop_on/opacity_on:false switch that "
                 "Inspector section off (its values and keyframes stay but do not apply, e.g. to compare), true on "
                 "again; `info` shows them when off.",
                 opTransform}
        << OpDef{"keyframe", "{op:'keyframe', clips, param, at, value?, ease?, remove?, move_to?} or {op:'keyframe', "
                             "clips, param, keys:[{at, value?, ease?, remove?, move_to?}, …]} or {op:'keyframe', clips, "
                             "param, clear:true}",
                 "animate a value",
                 "param: zoom (zoom_x and zoom_y together; `info` shows them as zoom while they match), zoom_x, "
                 "zoom_y, x, y, rotation, crop_left, crop_right, crop_top, crop_bottom, opacity, "
                 "title_size, title_x, title_y, title_color (\"#rrggbb\"), volume (dB, audio clips), pan (-100..100, "
                 "audio clips) or an effect parameter \"<effect>.<key>\" (keys: `effects <id>`; grade values: "
                 "grade.liftY, grade.gainR, grade.saturation, …). at = timeline frame inside the clip (its end counts as "
                 "its last frame; outside is an error - with several clips, ones no time falls in are left alone), or "
                 "{clip, offset} / {clip, source} (relative to the clip, stays right when clips move: "
                 "at:{clip:7, offset:\"2s\"} = 2 s after clip 7 starts). Without value "
                 "the keyframe keeps the current value there. ease (from this keyframe on): linear, ease_in, ease_out, "
                 "ease_in_out. Between keyframes the value is interpolated; with no keyframes left the static value "
                 "applies. Times in `info` keyframes are timeline frames. Units as in op 'transform' (crop_*: timeline "
                 "pixels on the fitted picture). move_to = move the keyframe at `at` to this time inside the clip "
                 "(value, ease and curve handles stay; one already there is replaced). For a zoom punch, flash or "
                 "shake on a hit use op 'accent' (one op for many times).",
                 opKeyframe}
        << OpDef{"accent", "{op:'accent', at:[times]|markers:true, clips?, preset?, strength?, length?, punch?, flash?, "
                           "shake?}",
                 "zoom punch / flash / shake on hits (kills, beats, drops), written as keyframes",
                 "at = the hit times (one time or a list, e.g. bar ones from `beats`, or the hits of `highlights`: "
                 "its hit_s, not kill_s which comes a few frames later, as {media:file, source:'<hit_s>s'}); "
                 "markers:true adds the timeline markers. Without clips: every enabled video clip with a picture "
                 "under each time (no titles); clips: only these. preset (default hit; `effects` lists them): "
                 "punch (zoom +15 %, lands back in 0.25 s), flash (exposure +2 stops, 0.2 s), shake (position jumps "
                 "of up to 3 % of the frame, 0.25 s), hit (mild: punch 10 % + shake 1 %, 0.3 s, no flash: a kill), big "
                 "(punch 12 % + flash 1.5 + shake 2 %: the one big moment of a video, not every kill), drop "
                 "(punch 25 % + flash 2.5, 0.5 s: the music drop), pulse (zoom +5 %, 0.15 s: beats). strength scales "
                 "it (0.5 half, 2 double); length = how long it takes to fade (\"0.4s\"); punch (share: 0.15 = 15 % "
                 "bigger, negative = smaller), flash (stops, negative = darker) and shake (share of the frame's "
                 "shorter side) set one part directly, 0 leaves it out. The peak sits on the time, the rise starts "
                 "~30 ms before. Writes ordinary keyframes (zoom, grade.exposure, x, y) on top of what the clip has "
                 "there (a slow zoom keeps going) that add up for close hits; flashes keep 0.5 s apart (a burst of "
                 "kills would strobe), the hits in between get punch and shake only. Change or remove them with op "
                 "'keyframe'. Shake moves the picture: on a clip that just fills the frame the edge shows (zoom a "
                 "little, or fit:'blur'). Save the big accents for the big moments: the same one on every cut wears "
                 "off; pulse suits beats.",
                 opAccent}
        << OpDef{"copy_attributes", "{op:'copy_attributes', from, clips, attributes?}",
                 "Paste Attributes: copy values of clip `from` to the clips",
                 "attributes (default all): zoom (with fit), position, rotation, crop, opacity, effects (video and audio), color "
                 "(grade), speed, volume, pan, fades. Keyframes come along.",
                 opCopyAttributes};
}

} // namespace Cli::detail
