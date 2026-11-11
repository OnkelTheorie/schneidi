// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli `brand`: the brand kit (core/BrandKit.h) - the global one (all projects) and a project's own. Shows
// them, sets fields, copies the kit into the titles that reference it (apply), places the logo or intro/outro. What
// changes a project runs in the app when it has the project open (one undo step), else on the file (backup first).
#include "cli/CommandsDetail.h"
#include "cli/Sandbox.h"

#include "core/BrandKit.h"
#include "core/Editor.h"
#include "core/Project.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QLockFile>

namespace Cli::detail {

namespace {

const char* const kCorners[] = {"center", "top-right", "top-left", "bottom-right", "bottom-left"};

QJsonObject kitJson(const BrandKit& k)
{
    QJsonObject o;
    QJsonArray missing;
    auto file = [&](const char* key, const QString& path) {
        if (path.isEmpty()) return;
        o[key] = path;
        if (!QFileInfo::exists(path)) missing << key;
    };
    file("logo", k.logo);
    o["logo_corner"] = kCorners[std::clamp(k.logoCorner, 0, 4)];
    o["logo_size"] = k.logoSize;
    QJsonArray colors;
    for (const QColor& c : k.colors) colors << c.name(c.alpha() == 255 ? QColor::HexRgb : QColor::HexArgb);
    o["colors"] = colors;
    o["fonts"] = QJsonArray::fromStringList(k.fonts);
    file("intro", k.intro);
    file("outro", k.outro);
    if (!missing.isEmpty()) o["missing_files"] = missing;
    return o;
}

// Titles and subtitle tracks that reference the kit (all timelines)
int references(const Project& p)
{
    int n = 0;
    for (const Sequence& s : p.sequences()) {
        for (const Track& t : s.timeline.video)
            for (const Clip& c : t.clips) n += c.isTitle() && c.title.hasBrandRef();
        for (const SubtitleTrack& st : s.timeline.subtitles) n += st.style.hasBrandRef();
    }
    return n;
}

QStringList listOf(const QString& s)
{
    QStringList out;
    for (const QString& x : s.split(',')) out << x.trimmed();
    out.removeAll(QString());
    return out;
}

} // namespace

BrandKit projectKitBase(BrandKit k)
{
    for (QString* file : {&k.logo, &k.intro, &k.outro})
        if (!file->isEmpty() && !Sandbox::projectFileAllowed(*file)) file->clear();
    return k;
}

// The fields given in `a` over `k`; an empty string clears a file/list
BrandKit brandWithFields(BrandKit k, const QJsonObject& a)
{
    auto path = [](const QString& s) { return s.trimmed().isEmpty() ? QString() : absolute(s.trimmed()); };
    if (a.contains("logo")) k.logo = path(a["logo"].toString());
    if (a.contains("intro")) k.intro = path(a["intro"].toString());
    if (a.contains("outro")) k.outro = path(a["outro"].toString());
    if (a.contains("logo_corner")) {
        const QString c = a["logo_corner"].toString().trimmed().toLower();
        int corner = -1;
        for (int i = 0; i < 5; ++i)
            if (c == kCorners[i] || c == QString::number(i)) corner = i;
        if (corner < 0) fail("BAD_ARGUMENT", "logo_corner: top-right, top-left, bottom-right, bottom-left or center");
        k.logoCorner = corner;
    }
    if (a.contains("logo_size")) {
        const double v = a["logo_size"].toDouble();
        if (!(v > 0 && v <= 1)) fail("BAD_ARGUMENT", "logo_size: the logo's width as share of the picture, 0.01..1");
        k.logoSize = v;
    }
    if (a.contains("colors")) {
        k.colors.clear();
        for (const QString& c : listOf(a["colors"].toString())) k.colors << colorOf(c, "colors: " + c);
        if (k.colors.size() > BrandKit::kMaxColors) fail("BAD_ARGUMENT", "colors: at most 4");
    }
    if (a.contains("fonts")) {
        k.fonts = listOf(a["fonts"].toString());
        if (k.fonts.size() > BrandKit::kMaxFonts) fail("BAD_ARGUMENT", "fonts: at most 2 (heading, body)");
    }
    return k.clamped();
}

namespace {

bool hasFields(const QJsonObject& a)
{
    for (const char* key : {"logo", "logo_corner", "logo_size", "colors", "fonts", "intro", "outro"})
        if (a.contains(key)) return true;
    return false;
}

} // namespace

QJsonObject cmdBrand(const QJsonObject& a, Context&)
{
    const QString action = a["action"].toString("show").trimmed().toLower();
    const QString project = a["project"].toString();
    static const QStringList actions{"show", "set", "apply", "unset", "logo", "intro_outro"};
    if (!actions.contains(action))
        fail("BAD_ARGUMENT", "action: show, set, apply, unset, logo or intro_outro");
    if (action != "show" && action != "set" && project.isEmpty())
        fail("BAD_ARGUMENT", QString("%1 needs a project (a .schneidi file or \"@app\")").arg(action));
    const bool global = a.contains("global") ? a["global"].toBool() : true;
    if (action == "set" && !global && project.isEmpty())
        fail("BAD_ARGUMENT", "global:false sets the project's own kit: give the project");
    if (action == "set" && !hasFields(a))
        fail("BAD_ARGUMENT", "set: give the fields to change (logo, logo_corner, logo_size, colors, fonts, intro, outro)");

    QJsonObject out;
    // Setting the global kit needs no project; with one, the project gets it applied below
    if (action == "set" && global) {
        const std::unique_ptr<QLockFile> lock = Brand::lockGlobal();
        if (!lock) fail("BUSY", "the global brand kit is being changed by another schneidi, try again");
        const BrandKit kit = brandWithFields(Brand::global(), a);
        QString err;
        if (!Brand::setGlobal(kit, &err)) fail("SAVE_FAILED", err);
    }
    if (project.isEmpty()) {
        out["global"] = kitJson(Brand::global());
        return out;
    }

    const bool writes = action != "show";
    Session s(project, writes ? Session::Access::Write : Session::Access::Read);
    Project& p = s.project;
    if (action == "set" && !global) {
        s.editor.setProjectBrand(brandWithFields(projectKitBase(p.effectiveBrand()), a)); // starts from what applies now
    } else if (action == "unset") {
        if (!p.brand()) s.notes << "the project has no own kit: nothing to unset";
        s.editor.setProjectBrand(std::nullopt);
    } else if (action == "apply" || (action == "set" && global)) {
        const bool changed = s.editor.applyBrand();
        out["applied"] = changed;
        if (action == "set" && p.brand()) s.notes << "the project has its own kit, the global one does not apply to it";
    } else if (action == "logo") {
        const BrandKit kit = p.effectiveBrand();
        if (kit.logo.isEmpty()) fail("NOT_FOUND", "the brand kit has no logo (set it with action:'set', logo:<file>)");
        s.media(kit.logo);
        if (a.contains("timeline")) s.selectTimeline(a["timeline"]);
        const int id = s.editor.addBrandLogo(p.currentSequence());
        if (!id) fail("EMPTY", "nothing to put the logo over: the timeline is empty");
        out["clip"] = id;
    } else if (action == "intro_outro") {
        const BrandKit kit = p.effectiveBrand();
        if (kit.intro.isEmpty() && kit.outro.isEmpty())
            fail("NOT_FOUND", "the brand kit has no intro or outro (set them with action:'set', intro/outro:<file>)");
        if (!kit.intro.isEmpty()) s.media(kit.intro);
        if (!kit.outro.isEmpty()) s.media(kit.outro);
        if (a.contains("timeline")) s.selectTimeline(a["timeline"]);
        if (!s.editor.addIntroOutro()) fail("LOCKED", "V1 is locked");
    }
    if (writes) {
        const QString backup = s.save();
        if (!backup.isEmpty()) out["backup"] = backup;
    }
    out["project"] = s.path;
    out["global"] = kitJson(Brand::global());
    out["project_kit"] = p.brand() ? QJsonValue(kitJson(*p.brand())) : QJsonValue(QJsonValue::Null);
    out["effective"] = kitJson(p.effectiveBrand());
    out["references"] = references(p);
    if (s.live) out["live"] = true;
    if (!s.notes.isEmpty()) out["notes"] = s.notes;
    return out;
}

} // namespace Cli::detail
