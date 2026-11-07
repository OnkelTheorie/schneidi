// SPDX-License-Identifier: AGPL-3.0-only
// Readable names of the effect parameters stored under a plugin index ("0", "1" of the effects that replaced
// frei0r): set by name in op 'effect', a keyframe and an fx node; the project file keeps the index; `info` and
// `effects` show the name.
#include "cli/Commands.h"
#include "core/CompGraph.h"
#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/TimelineOps.h"

#include "check.h"

#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

namespace {

QString g_project;

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        qWarning().noquote() << "unexpected error" << e.code << e.message;
        return {{"error", e.code + ": " + e.message}};
    }
}

QJsonObject edit(const QJsonArray& ops) { return run("edit", {{"project", g_project}, {"ops", ops}}); }
QJsonObject op(const char* json) { return QJsonDocument::fromJson(json).object(); }

ProjectData load()
{
    ProjectData d;
    ProjectFile::load(g_project, &d, nullptr);
    return d;
}

// First object in v (depth first) whose `key` is `value`
QJsonObject findObject(const QJsonValue& v, const QString& key, const QString& value)
{
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        if (o.value(key).toString() == value) return o;
        for (const QJsonValue& x : o)
            if (QJsonObject r = findObject(x, key, value); !r.isEmpty()) return r;
    } else if (v.isArray()) {
        for (const QJsonValue& x : v.toArray())
            if (QJsonObject r = findObject(x, key, value); !r.isEmpty()) return r;
    }
    return {};
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("effect_aliases");
    QTemporaryDir dir;
    const QString still = dir.filePath("gray.png");
    {
        QImage img(64, 36, QImage::Format_RGB32);
        img.fill(QColor(128, 128, 128));
        CHECK(img.save(still));
    }
    g_project = dir.filePath("alias.schneidi");
    run("new", {{"project", g_project}, {"width", 64}, {"height", 36}, {"fps", 25}, {"media", QJsonArray{still}}});
    edit({QJsonObject{{"op", "append"}, {"media", still}, {"duration", "2s"}}});
    const int clip = load().timeline.video[0].clips.first().id;

    // The registry: names from the plugin's parameter names, unique per effect, never a bare index
    for (const EffectDescriptor& d : EffectRegistry::all()) {
        QSet<QString> names;
        for (const EffectParam& p : d.params) {
            if (!CHECK(!names.contains(p.name()))) std::printf("       %s: name %s twice\n", qPrintable(d.id), qPrintable(p.name()));
            names.insert(p.name());
            if (d.id.startsWith("frei0r.") && !CHECK(!p.name().isEmpty() && !p.name()[0].isDigit()))
                std::printf("       %s: parameter %s has no readable name\n", qPrintable(d.id), qPrintable(p.key));
        }
    }
    CHECK_EQ(EffectRegistry::storedKey("frei0r.glow", "blur"), QString("0"));
    CHECK_EQ(EffectRegistry::storedKey("frei0r.colorhalftone", "cyan_angle"), QString("1"));
    CHECK_EQ(EffectRegistry::storedKey("frei0r.rgbnoise", "amount"), QString("0"));
    CHECK_EQ(EffectRegistry::storedKey("frei0r.nosync0r", "offset"), QString("0"));
    CHECK_EQ(EffectRegistry::storedKey("frei0r.cairogradient", "shape"), QString("0"));
    CHECK_EQ(EffectRegistry::storedKey("frei0r.cairogradient", "start_color"), QString("1"));
    CHECK_EQ(EffectRegistry::storedKey("frei0r.alpha0ps_alpha0ps", "operation"), QString("2"));
    CHECK_EQ(EffectRegistry::shownKey("frei0r.glow", "0"), QString("blur"));
    CHECK_EQ(EffectRegistry::shownKey("frei0r.glow", "strength"), QString("strength"));

    // op 'effect' by name: stored under the index; the index still works (old scripts)
    QJsonObject r = edit({QJsonObject{{"op", "effect"}, {"clip", clip}, {"effect", "frei0r.glow"},
                                      {"params", QJsonObject{{"blur", 0.4}, {"strength", 0.5}}}}});
    CHECK(!r.contains("error"));
    auto glow = [&](const QString& key) {
        const ProjectData d = load();
        const Clip* c = TimelineOps::findClip(d.timeline, clip);
        return c ? EffectRegistry::value(*c, "frei0r.glow", key) : QVariant();
    };
    CHECK_EQ(glow("0").toDouble(), 0.4);
    CHECK(!glow("blur").isValid() || glow("blur") == glow("0")); // not stored under the name
    {
        QFile f(g_project);
        CHECK(f.open(QIODevice::ReadOnly));
        const QByteArray text = f.readAll();
        CHECK(!text.contains("\"blur\""));
    }
    edit({QJsonObject{{"op", "effect"}, {"clip", clip}, {"effect", "frei0r.glow"}, {"params", QJsonObject{{"0", 0.3}}}}});
    CHECK_EQ(glow("0").toDouble(), 0.3);

    // info shows the name
    QJsonObject info = run("info", {{"project", g_project}});
    QJsonObject fx = findObject(info, "effect", "frei0r.glow");
    CHECK_EQ(fx["params"].toObject().value("blur").toDouble(), 0.3);
    CHECK(!fx["params"].toObject().contains("0"));

    // A keyframe by name animates the stored parameter and shows by name
    r = edit({QJsonObject{{"op", "keyframe"}, {"clips", QJsonArray{clip}}, {"param", "frei0r.glow.blur"}, {"at", 0}, {"value", 0.1}},
              QJsonObject{{"op", "keyframe"}, {"clips", QJsonArray{clip}}, {"param", "frei0r.glow.blur"}, {"at", 25}, {"value", 0.9}}});
    CHECK(!r.contains("error"));
    {
        const ProjectData d = load();
        const Clip* c = TimelineOps::findClip(d.timeline, clip);
        CHECK(c && c->keys.value(Keys::effectParam("frei0r.glow", "0")).size() == 2);
    }
    info = run("info", {{"project", g_project}});
    CHECK(QJsonDocument(info).toJson().contains("frei0r.glow.blur"));
    CHECK(!QJsonDocument(info).toJson().contains("frei0r.glow.0"));

    // effects: the name is the key, the stored index beside it
    const QJsonObject list = run("effects", {{"effect", "frei0r.colorhalftone"}});
    const QJsonObject dot = findObject(list, "key", "dot_radius");
    CHECK_EQ(dot.value("stored_as").toString(), QString("0"));
    CHECK(findObject(list, "key", "0").isEmpty());

    // An fx node of a compositing graph: e_<name> stored as e_<index>, shown as e_<name>
    QJsonObject add = op(R"({"op":"node","add":{"type":"fx","params":{"effect":"frei0r.rgbnoise","e_amount":0.6}}})");
    add["clip"] = clip;
    r = edit({add});
    CHECK(!r.contains("error"));
    {
        const ProjectData d = load();
        const Clip* c = TimelineOps::findClip(d.timeline, clip);
        const EffectInstance* e = c ? EffectRegistry::instance(*c, "comp") : nullptr;
        bool stored = false;
        if (e)
            for (auto it = e->params.begin(); it != e->params.end(); ++it)
                if (it.key().endsWith(".e_0") && std::abs(it.value().toDouble() - 0.6) < 1e-9) stored = true;
        CHECK(stored);
    }
    info = run("info", {{"project", g_project}});
    const QJsonObject node = findObject(info, "type", "fx");
    CHECK_EQ(node["params"].toObject().value("e_amount").toDouble(), 0.6);
    CHECK(!node["params"].toObject().contains("e_0"));
    return Check::result();
}
