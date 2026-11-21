// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli `templates`: start templates (core/StartTemplates, CommandsProject.cpp) and content templates
// (core/TemplateLibrary: a project with placeholder slots, core/TemplateFill). Lists both, shows a content template's
// slots, fills one into a new project (values checked against the slots' rules, brand kit applied, optionally
// rendered) or fills the slots of a project in place (live in the app: one undo step), saves/removes/renames own
// templates and shares them as ZIP (core/TemplateZip). fill + rows: one project (and video) per row of a table (core/DataRows).
#include "cli/CommandsDetail.h"

#include "cli/AnalysisDetail.h"
#include "cli/Live.h"
#include "cli/Sandbox.h"
#include "core/Beats.h"
#include "core/BrandKit.h"
#include "core/DataRows.h"
#include "core/LiveHost.h"
#include "core/ProjectFile.h"
#include "core/RenderJob.h"
#include "core/SlotPick.h"
#include "core/TemplateFill.h"
#include "core/TemplateLibrary.h"
#include "core/TemplateZip.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "engine/MediaIndex.h"
#include "engine/ReframeAnalysis.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSet>

#include <cmath>
#include <memory>

namespace Cli::detail {

namespace {

double secs(int frames, const ProjectFormat& f) { return std::round(seconds(frames, f) * 1000) / 1000; }

// Every timeline of a project (the open one from ProjectData::timeline) with its sequence
QVector<std::pair<const Sequence*, const Timeline*>> timelinesOf(const ProjectData& d)
{
    QVector<std::pair<const Sequence*, const Timeline*>> out;
    bool open = false;
    for (const Sequence& s : d.sequences) {
        const bool current = s.id == d.currentSequence;
        open |= current;
        out.push_back({&s, current ? &d.timeline : &s.timeline});
    }
    if (!open) out.prepend({nullptr, &d.timeline});
    return out;
}

void forEachTimeline(ProjectData& d, const std::function<void(Timeline&)>& fn)
{
    fn(d.timeline);
    for (Sequence& s : d.sequences)
        if (s.id != d.currentSequence) fn(s.timeline);
}

QString sequenceName(const ProjectData& d, int id)
{
    for (const Sequence& s : d.sequences)
        if (s.id == id) return s.name;
    return {};
}

QJsonObject contentJson(const ContentTemplate& t)
{
    QJsonObject o{{"kind", "content"}, {"id", t.id}, {"name", t.name}, {"built_in", t.builtIn}};
    if (!t.description.isEmpty()) o["description"] = t.description;
    if (!t.tags.isEmpty()) o["tags"] = QJsonArray::fromStringList(t.tags);
    o["aspect"] = t.aspect;
    o["width"] = t.width;
    o["height"] = t.height;
    o["fps"] = std::round(t.fps * 1000) / 1000;
    ProjectFormat f;
    f.rate = templateFrameRate(t.fps > 0 ? t.fps : 25); // 48 stays 48 (nearestFrameRate made it 50)
    o["length_s"] = secs(t.length, f);
    o["slots"] = QJsonArray::fromStringList(t.slotIds);
    if (!t.preview.isEmpty() && !Sandbox::active()) o["preview"] = t.preview;
    if (!Sandbox::active()) o["dir"] = t.dir;
    return o;
}

// Titles and subtitle tracks that take a value from the brand kit
QJsonArray brandRefs(const ProjectData& d)
{
    QJsonArray out;
    const auto refs = [](const TitleStyle& t) {
        QJsonObject r;
        const auto add = [&](const char* key, int n) {
            if (n) r[key] = QString("brand%1").arg(n);
        };
        add("color", t.colorRef);
        add("outline_color", t.outlineRef);
        add("box_color", t.boxRef);
        add("font", t.fontRef);
        return r;
    };
    for (const auto& [seq, tl] : timelinesOf(d)) {
        const QString name = seq ? seq->name : QString();
        for (int i = 0; i < tl->video.size(); ++i)
            for (const Clip& c : tl->video[i].clips)
                if (c.isTitle() && c.title.hasBrandRef())
                    out << QJsonObject{{"timeline", name}, {"clip", c.id}, {"track", trackName({TrackKind::Video, i})},
                                       {"text", c.title.text}, {"refs", refs(c.title)}};
        for (int i = 0; i < tl->subtitles.size(); ++i)
            if (tl->subtitles[i].style.hasBrandRef())
                out << QJsonObject{{"timeline", name}, {"subtitle_track", i + 1}, {"refs", refs(tl->subtitles[i].style)}};
    }
    return out;
}

// The slots of a project grouped by id: rules once, every instance with where it is and what it holds
QJsonArray slotsJson(const ProjectData& d)
{
    const ProjectFormat& f = d.format;
    QJsonArray out;
    QHash<QString, int> index;
    for (const TemplateFill::SlotRef& r : TemplateFill::listSlots(d)) {
        if (!index.contains(r.slot.id)) {
            index[r.slot.id] = int(out.size());
            QJsonObject o = slotJson(r.slot, f);
            o["instances"] = QJsonArray();
            out << o;
        }
        QJsonObject inst{{"timeline", sequenceName(d, r.sequence)}, {"track", trackName(r.track)}, {"clip", r.clip},
                         {"start_s", secs(r.start, f)}, {"length_s", secs(r.length, f)}};
        if (r.parts.size() > 1) {
            QJsonArray parts;
            for (int id : r.parts) parts << id;
            inst["clips"] = parts;
        }
        if (r.slot.kind == SlotKind::Text) inst["text"] = r.text;
        else if (!r.media.isEmpty()) inst["media"] = r.media;
        QJsonObject o = out[index[r.slot.id]].toObject();
        QJsonArray list = o["instances"].toArray();
        list << inst;
        o["instances"] = list;
        out[index[r.slot.id]] = o;
    }
    return out;
}

QJsonObject infoJson(const ProjectData& d, const std::optional<ContentTemplate>& t)
{
    QJsonObject o = t ? contentJson(*t) : QJsonObject{{"kind", "project"}};
    o["slots"] = slotsJson(d);
    const QJsonArray refs = brandRefs(d);
    if (!refs.isEmpty()) o["brand_refs"] = refs;
    if (d.brand) o["own_brand_kit"] = true; // the template brings its own kit (else the user's global one applies)
    return o;
}

QStringList unfilledOf(const ProjectData& d, const TemplateFill::Values& values)
{
    QStringList out;
    for (const QString& id : TemplateFill::slotIds(d))
        if (!values.contains(id)) out << id;
    return out;
}

QJsonObject problemJson(const TemplateFill::Problem& p)
{
    return {{"slot", p.slot}, {"code", p.code}, {"message", p.message}};
}

// The answer of a check (dry_run): the shape of Report::toJson without results
QJsonObject checkJson(const QVector<TemplateFill::Problem>& problems, const QStringList& unfilled, bool strict)
{
    QJsonArray errs;
    for (const TemplateFill::Problem& p : problems) errs << problemJson(p);
    if (strict)
        for (const QString& id : unfilled) errs << QJsonObject{{"slot", id}, {"code", "UNFILLED"}, {"message", "no value for the slot"}};
    return {{"dry_run", true}, {"ok", errs.isEmpty()}, {"results", QJsonArray()}, {"errors", errs},
            {"unfilled", QJsonArray::fromStringList(unfilled)}};
}

void failUnfilled(const QStringList& unfilled)
{
    fail("UNFILLED", QString("no value for the slot(s) %1 (strict)").arg(unfilled.join(", ")));
}

// The brand kit for a filled project: "global" (what applies: the project's own kit or the user's), "none", or kit
// fields (the project's own kit). Returns what was done.
QJsonObject applyBrand(Editor& editor, Project& p, const QJsonValue& brand)
{
    QJsonObject out;
    if (brand.isObject()) {
        QJsonObject fields = brand.toObject();
        for (const char* key : {"colors", "fonts"})
            if (fields[key].isArray()) {
                QStringList list;
                for (const QJsonValue& v : fields[key].toArray()) list << v.toString();
                fields[key] = list.join(',');
            }
        editor.setProjectBrand(brandWithFields(projectKitBase(p.effectiveBrand()), fields));
        out["kit"] = "own";
        out["applied"] = true;
    } else {
        const QString mode = brand.toString("global").trimmed().toLower();
        if (mode == "none") return {{"kit", "none"}, {"applied", false}};
        if (mode != "global") fail("BAD_ARGUMENT", "brand: \"global\", \"none\" or kit fields {colors, fonts, logo, …}");
        out["kit"] = p.brand() ? "project" : "global";
        out["applied"] = editor.applyBrand();
    }
    int refs = 0;
    for (const Sequence& s : p.sequences()) {
        for (const Track& t : s.timeline.video)
            for (const Clip& c : t.clips) refs += c.isTitle() && c.title.hasBrandRef();
        for (const SubtitleTrack& st : s.timeline.subtitles) refs += st.style.hasBrandRef();
    }
    out["references"] = refs;
    return out;
}

QString checkedProjectPath(const QString& path, const char* what)
{
    if (!path.endsWith("." + QString(ProjectFile::Extension)))
        fail("BAD_ARGUMENT", QString("%1 must end in .%2").arg(what, ProjectFile::Extension));
    return path;
}

// The render of a filled project: `render`'s parameters, out next to the project by default
QJsonObject renderFilled(const QJsonValue& v, const QString& project, Context& ctx)
{
    QJsonObject r = v.isObject() ? v.toObject() : QJsonObject();
    if (v.isString() && !QStringList{"true", "yes", "1", ""}.contains(v.toString().trimmed().toLower()))
        fail("BAD_ARGUMENT", "render: the parameters of `render` as an object ({out?, preset?, …})");
    r["project"] = project;
    if (!r.contains("out") && !r.contains("output")) {
        const QFileInfo fi(project);
        r["out"] = fi.absoluteDir().filePath(fi.completeBaseName() + ".mp4");
    }
    const Command* cmd = Cli::find("render");
    if (ctx.stage) ctx.stage("render");
    return cmd->run(Cli::normalize(*cmd, r), ctx);
}

QString problemsText(const QVector<TemplateFill::Problem>& problems)
{
    QStringList parts;
    for (const TemplateFill::Problem& p : problems)
        parts << QString("%1 %2: %3").arg(p.slot.isEmpty() ? QString("-") : p.slot, p.code, p.message);
    return parts.join("; ");
}

std::optional<ContentTemplate> contentTemplate(const QString& key)
{
    QString err;
    std::optional<ContentTemplate> t = TemplateLibrary::find(key, &err);
    if (!t) {
        QStringList ids;
        for (const ContentTemplate& c : TemplateLibrary::all()) ids << c.id;
        fail("NOT_FOUND", QString("%1 (content templates: %2)").arg(err, ids.isEmpty() ? QString("none") : ids.join(", ")));
    }
    return t;
}

ProjectData loadTemplate(const ContentTemplate& t)
{
    ProjectData d;
    QString err;
    if (!TemplateLibrary::load(t, &d, &err)) fail("BAD_TEMPLATE", err);
    return d;
}

QString reframeMode(const QJsonObject& a)
{
    const QString m = a.value("reframe").toString("cached").trimmed().toLower();
    if (m != "cached" && m != "auto") fail("BAD_ARGUMENT", "reframe: \"cached\" or \"auto\"");
    return m;
}

// ---------- auto: the clip slots filled with shots of the material index (core/SlotPick) ----------

// The material to choose from: `files` (a folder = its files), else the project's media with a picture
QStringList autoMaterial(const QJsonObject& a, const ProjectData* project)
{
    QStringList out;
    const QJsonValue v = a.value("files");
    QStringList given;
    if (v.isArray())
        for (const QJsonValue& x : v.toArray()) given << x.toString();
    else if (v.isString()) given = v.toString().split(',', Qt::SkipEmptyParts);
    for (const QString& g : std::as_const(given)) {
        const QString path = absolute(g.trimmed());
        if (QFileInfo(path).isDir()) {
            for (const QFileInfo& fi : QDir(path).entryInfoList(QDir::Files | QDir::Readable, QDir::Name))
                if (!fi.fileName().startsWith('.') && !fi.fileName().endsWith(".schneidi")) out << fi.absoluteFilePath();
        } else {
            out << path;
        }
    }
    if (out.isEmpty() && project)
        for (const MediaInfo& m : project->media)
            if (m.hasVideo) out << m.path;
    out.removeDuplicates();
    return out;
}

// `values` with a shot for every clip slot that has none yet: {file, from, to, auto:{shot, reason}}. The index is built
// for files without one (cancelable); the chosen slots are listed with autoPicks. `inPlace` = the project's own
// transcripts give the shots their speech.
QJsonObject withAutoValues(const QJsonObject& a, const ProjectData& d, bool inPlace, Context& ctx)
{
    // Inside the app nothing is indexed (the user would wait): schneidi-cli chooses before it sends the fill
    if (Live::host())
        fail("LIVE_NOT_PREPARED", "auto is chosen by schneidi-cli before the fill goes to the app (prepareTemplateFill): "
                                  "send it through schneidi-cli or MCP, or give values");
    QJsonObject out = a;
    out.remove("auto");
    out.remove("files");
    QJsonObject values = a.value("values").toObject();
    QStringList files = autoMaterial(a, inPlace ? &d : nullptr);
    // What the project already shows outside its slots (the brand kit's intro/outro/logo, stills, other clips) is no
    // material: it would be seen twice
    QSet<QString> shown;
    for (const auto& [seq, tl] : timelinesOf(d))
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (const Track& t : tl->tracks(k))
                for (const Clip& c : t.clips)
                    if (!c.slot.isSet() && !c.mediaPath.isEmpty()) shown.insert(QFileInfo(c.mediaPath).absoluteFilePath());
    const BrandKit kit = Brand::effective(d);
    for (const QString& f : {kit.intro, kit.logo, kit.outro})
        if (!f.isEmpty()) shown.insert(QFileInfo(absolute(f)).absoluteFilePath());
    QStringList skipped;
    files.erase(std::remove_if(files.begin(), files.end(), [&](const QString& f) {
                    const bool used = shown.contains(QFileInfo(f).absoluteFilePath());
                    if (used) skipped << QFileInfo(f).fileName();
                    return used;
                }),
                files.end());
    if (files.isEmpty())
        fail("NO_MATERIAL", inPlace ? "auto: the project has no video in its media; give files (or a folder) to choose from"
                                    : "auto: give files (or a folder) to choose the shots from");
    const double fps = d.format.rate.fps();
    // The clip slots without a value, each once; the longest of its instances tells how long a shot must be
    QVector<SlotPick::Slot> slotList;
    QHash<QString, int> seen;
    for (const TemplateFill::SlotRef& r : TemplateFill::listSlots(d)) {
        if (r.slot.kind != SlotKind::Clip || values.contains(r.slot.id)) continue;
        const int need = r.slot.minLen > 0 ? r.slot.minLen : (r.slot.length == SlotLength::Fixed ? r.slotLength : 0);
        if (const auto it = seen.constFind(r.slot.id); it != seen.constEnd()) {
            slotList[*it].minS = std::max(slotList[*it].minS, need / fps);
            continue;
        }
        seen.insert(r.slot.id, int(slotList.size()));
        SlotPick::Slot x;
        x.id = r.slot.id;
        x.label = r.slot.label;
        x.hint = r.slot.hint;
        x.minS = need / fps;
        slotList << x;
    }
    QStringList notes;
    if (!skipped.isEmpty())
        notes << QString("auto: left out of the material, the project shows them already: %1").arg(skipped.join(", "));
    QJsonArray picks;
    if (!slotList.isEmpty()) {
        QVector<SlotPick::Shot> shots;
        for (int i = 0; i < files.size(); ++i) {
            const QString& file = files[i];
            if (ctx.stage) ctx.stage("index " + QFileInfo(file).fileName());
            MediaIndex::Options o;
            const int n = int(files.size());
            o.progress = goOn([&ctx, i, n](double p) {
                if (ctx.progress) ctx.progress((i + p) / n);
            });
            QString error;
            std::optional<MediaIndex::Entry> e = MediaIndex::cached(file);
            if (!e) e = MediaIndex::build(file, o, &error);
            failIfCanceled();
            if (!e) {
                notes << QString("%1: left out (%2)").arg(QFileInfo(file).fileName(), error);
                continue;
            }
            if (inPlace)
                for (const MediaInfo& m : d.media)
                    if (m.path == e->file)
                        for (const MediaTranscript& t : m.transcripts) MediaIndex::addSpeech(*e, t.words);
            const QVector<IndexSearch::Shot> search = MediaIndex::searchable(*e);
            for (int k = 0; k < e->shots.size() && k < search.size(); ++k) {
                SlotPick::Shot x;
                x.shot = search[k];
                x.video = e->video;
                x.sharp = e->video ? e->shots[k].quality.sharp : -1;
                x.bright = e->video ? e->shots[k].quality.bright : -1;
                x.faces = e->shots[k].quality.faces;
                shots << x;
            }
        }
        for (const SlotPick::Pick& p : SlotPick::pick(slotList, shots)) {
            QJsonObject reason{{"key", "slot_match"}, {"text", p.why}};
            QJsonObject rv;
            if (!p.matched.isEmpty()) rv["matched"] = QJsonArray::fromStringList(p.matched);
            if (!p.quality.isEmpty()) rv["quality"] = QJsonArray::fromStringList(p.quality);
            if (p.shortShot) rv["short"] = true;
            if (p.inOrder) rv["in_order"] = true;
            reason["values"] = rv;
            if (p.shot < 0) {
                picks << QJsonObject{{"slot", p.slot}, {"shot", QJsonValue::Null}, {"reason", reason}};
                continue;
            }
            const SlotPick::Shot& sh = shots[p.shot];
            const QJsonObject pick{{"slot", p.slot}, {"file", sh.shot.file}, {"shot", sh.shot.n},
                                   {"from_s", std::round(sh.shot.from * 1000) / 1000},
                                   {"to_s", std::round(sh.shot.to * 1000) / 1000}, {"score", p.score},
                                   {"reason", reason}};
            picks << pick;
            // Whole frames inside the shot: its first frame rounded up (no frame of the shot before), its end down
            const int from = int(std::ceil(sh.shot.from * fps - 1e-6)), to = int(std::floor(sh.shot.to * fps + 1e-6));
            QJsonObject value{{"file", sh.shot.file}, {"from", from}, {"auto", pick}};
            if (to > from) value["to"] = to;
            values[p.slot] = value;
        }
    }
    out["values"] = values;
    out["_auto"] = QJsonObject{{"picks", picks}, {"notes", QJsonArray::fromStringList(notes)}};
    return out;
}

// What auto chose, for the answer: the picks and notes withAutoValues left in `_auto` (live: schneidi-cli put it into
// the arguments the app gets)
void addAutoPicks(const QJsonObject& a, QJsonObject& o, QStringList& notes)
{
    const QJsonArray picks = a.value("_auto").toObject().value("picks").toArray();
    for (const QJsonValue& n : a.value("_auto").toObject().value("notes").toArray()) notes << n.toString();
    if (!picks.isEmpty() || a.contains("_auto")) o["auto"] = picks;
}

// ---------- Actions ----------

QJsonObject fillInPlace(const QJsonObject& a0, Context& ctx)
{
    if (!a0.contains("values") && !a0["auto"].toBool())
        fail("BAD_ARGUMENT", "give values {slot: value} (or auto:true) to fill the project's slots");
    const bool dry = a0["dry_run"].toBool();
    Session s(a0["project"].toString(), dry ? Session::Access::Read : Session::Access::Write);
    const ProjectData d = s.project.data();
    const QJsonObject a = a0["auto"].toBool() ? withAutoValues(a0, d, true, ctx) : a0;
    const TemplateFill::Values values = fillValues(a["values"], d, d.format);
    QStringList notes;
    const TemplateFill::Context fc = fillContext(d.format, a["_beats"].toObject(), reframeMode(a) == "auto", &notes, &ctx);
    const QStringList unfilled = unfilledOf(d, values);
    if (dry) {
        QJsonObject o = checkJson(TemplateFill::check(d, values, fc), unfilled, a["strict"].toBool());
        o["project"] = s.path;
        addAutoPicks(a, o, notes);
        if (!notes.isEmpty()) o["notes"] = QJsonArray::fromStringList(notes);
        return o;
    }
    if (a["strict"].toBool() && !unfilled.isEmpty()) failUnfilled(unfilled);
    const TemplateFill::Report r = s.editor.fillSlots(values, fc);
    if (!r.ok()) fail("FILL_FAILED", problemsText(r.errors));
    QJsonObject o = r.toJson();
    if (a.contains("brand")) o["brand"] = applyBrand(s.editor, s.project, a["brand"]);
    const QString backup = s.save();
    o["project"] = s.path;
    if (!backup.isEmpty()) o["backup"] = backup;
    for (const QJsonValue& n : s.notes) notes << n.toString();
    addAutoPicks(a, o, notes);
    if (!notes.isEmpty()) o["notes"] = QJsonArray::fromStringList(notes);
    if (s.live) o["live"] = true;
    return o;
}

QJsonObject saveTemplate(const QJsonObject& a)
{
    const QString name = a["name"].toString().trimmed();
    if (name.isEmpty()) fail("BAD_ARGUMENT", "save needs a name");
    Session s(a["save"].toString());
    TemplateLibrary::SaveOptions o;
    o.description = a["description"].toString();
    for (const QString& t : a["tags"].toString().split(',', Qt::SkipEmptyParts)) o.tags << t.trimmed();
    o.copyMedia = a.contains("copy_media") ? a["copy_media"].toBool() : true;
    o.preview = a["preview"].toString();
    if (!o.preview.isEmpty() && !QFileInfo::exists(o.preview)) fail("NOT_FOUND", "no preview file " + o.preview);
    o.replace = a["replace"].toBool();
    const ProjectData d = s.project.data();
    if (TemplateFill::slotIds(d).isEmpty())
        s.notes << "the project has no slots: mark placeholders with the edit op 'slot' (or in the app) to make it fillable";
    QString err, code;
    const std::optional<ContentTemplate> t = TemplateLibrary::save(d, name, o, &err, &code);
    if (!t) fail(code.isEmpty() ? "SAVE_FAILED" : code, err);
    QJsonObject out = contentJson(*t);
    out["saved"] = true;
    if (!s.notes.isEmpty()) out["notes"] = s.notes;
    return out;
}

QJsonObject exportTemplate(const QJsonObject& a)
{
    const ContentTemplate t = *contentTemplate(a["export"].toString());
    const QString zip = a["zip"].toString();
    QString err, code;
    int files = 0;
    if (!TemplateLibrary::exportZip(t.dir, zip, a["overwrite"].toBool(), &err, &code, &files)) fail(code, err);
    return {{"zip", zip}, {"template", QJsonObject{{"id", t.id}, {"name", t.name}}}, {"files", files},
            {"size", QFileInfo(zip).size()}};
}

QJsonObject importTemplate(const QJsonObject& a)
{
    QString err, code;
    const std::optional<ContentTemplate> t = TemplateLibrary::importZip(
        a["import"].toString(), a["replace"].toBool() ? TemplateLibrary::ZipClash::Replace : TemplateLibrary::ZipClash::KeepBoth,
        &err, &code);
    if (!t) fail(code, err);
    QJsonObject out = contentJson(*t);
    out["imported"] = true;
    return out;
}

// ---------- Many videos from a table: fill + rows ----------

// The extension a render with these parameters writes (format, else the preset's, else H.264)
QString renderExtension(const QJsonObject& r)
{
    QString format = r["format"].toString().trimmed();
    if (format.isEmpty() && r.contains("preset"))
        if (const std::optional<RenderPreset> p = RenderPresets::find(r["preset"].toString())) format = p->settings.format;
    if (format.isEmpty()) format = RenderSettings().format;
    if (format == "srt" || format == "vtt") return format;
    return renderFormat(format).extension;
}

// What a column of the table fills
struct Columns {
    QHash<QString, QString> slotOf;           // column -> slot (the slot's own column, or "<slot>.from"/".to")
    QString nameColumn;                       // "out" or "name": the file name when no pattern is given
    QStringList extra, fileColumns, unfilled; // no slot / holding files / slots no column fills
};

Columns columnsOf(const QStringList& columns, const QHash<QString, SlotKind>& kinds, const QJsonObject& shared)
{
    Columns c;
    for (const QString& col : columns) {
        const QString slot = !kinds.contains(col) && (col.endsWith(".from") || col.endsWith(".to")) ? col.section('.', 0, -2) : col;
        if (kinds.contains(slot) && kinds[slot] != SlotKind::Text) c.fileColumns << col;
        if (kinds.contains(slot) && (slot == col || kinds[slot] != SlotKind::Text)) c.slotOf[col] = slot;
        else if ((col == "out" || col == "name") && c.nameColumn.isEmpty()) c.nameColumn = col;
        else c.extra << col;
    }
    QStringList ids = kinds.keys();
    std::sort(ids.begin(), ids.end());
    for (const QString& id : ids)
        if (!c.slotOf.values().contains(id) && !shared.contains(id)) c.unfilled << id;
    return c;
}

// A row as `values` of fillTemplate: files relative to the data file's folder, "<slot>.from"/".to" as source range
QJsonObject rowValues(const QJsonObject& row, const Columns& cols, const QHash<QString, SlotKind>& kinds,
                      const QDir& base, const QJsonObject& shared)
{
    QJsonObject values = shared;
    // The values are paths as a client gives them: fillTemplate maps them (--map) once. So a path of the client's
    // computer stays as it is, a relative one is taken against the table's folder (a path here) and turned back into
    // the client's (a map whose target is inside its source, /data=/data/client, would map it twice)
    const auto file = [&](const QString& f) {
        const QString given = f.trimmed();
        if (Sandbox::mapPath(given) != given || QFileInfo(given).isAbsolute()) return given;
        return Sandbox::mapBack(QDir::cleanPath(base.absoluteFilePath(given)));
    };
    for (auto it = row.begin(); it != row.end(); ++it) {
        const QString slot = cols.slotOf.value(it.key());
        if (slot.isEmpty() || slot != it.key()) continue;
        const QJsonValue v = it.value();
        if (kinds[slot] == SlotKind::Text) {
            if (v.isObject() || v.isString()) values[slot] = v;
            else values[slot] = DataRows::valueText(v); // 1234567, not 1.23457e+06
            continue;
        }
        QJsonObject o = v.isObject() ? v.toObject() : QJsonObject{{"file", v.toString()}};
        if (o["file"].isString() && !o["file"].toString().trimmed().isEmpty()) o["file"] = file(o["file"].toString());
        for (const char* part : {"from", "to"})
            if (row.contains(slot + "." + part)) o[part] = row[slot + "." + part];
        values[slot] = o.size() == 1 ? o["file"] : QJsonValue(o);
    }
    return values;
}

// The short form of a fill report for one row: what was adjusted per slot, warnings
void summarize(const QJsonObject& report, QJsonObject& entry)
{
    QJsonObject adjusted;
    QJsonArray warnings;
    for (const QJsonValue& r : report["results"].toArray()) {
        if (!r["adjusted"].toArray().isEmpty()) adjusted[r["slot"].toString()] = r["adjusted"];
        for (const QJsonValue& w : r["warnings"].toArray()) warnings << QString("%1: %2").arg(r["slot"].toString(), w.toString());
    }
    if (!adjusted.isEmpty()) entry["adjusted"] = adjusted;
    if (!warnings.isEmpty()) entry["warnings"] = warnings;
    if (!report["unfilled"].toArray().isEmpty()) entry["unfilled"] = report["unfilled"];
}

QJsonObject fillRows(const QJsonObject& a, Context& ctx)
{
    QElapsedTimer timer;
    timer.start();
    const ContentTemplate t = *contentTemplate(a["fill"].toString());
    const ProjectData d = loadTemplate(t);
    if (a.contains("out_project")) fail("BAD_ARGUMENT", "fill with rows writes one project per row: out_dir (and name) instead of out_project");
    // Called from other commands too (direct's template recipe, runSub), not only through the parameter table: the
    // path is resolved here as well (a work folder: relative paths start in it, nothing outside it)
    const QString outDir = a["out_dir"].toString().isEmpty() ? QString() : Sandbox::resolve(a["out_dir"].toString());
    if (outDir.isEmpty()) fail("BAD_ARGUMENT", "fill with rows needs out_dir (the folder for the projects and videos)");
    // The table: a file (paths in it relative to its folder) or the rows themselves
    QString err;
    std::optional<DataRows::Table> table;
    QDir base = QDir::current();
    const QJsonValue rows = a["rows"];
    if (rows.isString()) {
        const QString path = rows.toString();
        if (!QFileInfo(path).isFile()) fail("NOT_FOUND", "no file " + path);
        table = DataRows::readFile(path, &err);
        base = QFileInfo(path).absoluteDir();
    } else {
        table = DataRows::fromJson(rows, &err);
    }
    if (!table) fail("BAD_ROWS", err);
    if (table->rows.isEmpty()) fail("BAD_ROWS", "no data rows (the first line of a CSV names the columns: the slot ids)");
    const bool dry = a["dry_run"].toBool(), overwrite = a["overwrite"].toBool();
    const bool keepGoing = !a.contains("continue_on_error") || a["continue_on_error"].toBool();
    const QJsonObject shared = a["values"].isObject() ? a["values"].toObject() : QJsonObject();

    QHash<QString, SlotKind> kinds;
    for (const TemplateFill::SlotRef& r : TemplateFill::listSlots(d)) kinds.insert(r.slot.id, r.slot.kind);
    if (kinds.isEmpty()) fail("BAD_TEMPLATE", "the template has no slots to fill: " + t.name);
    const Columns cols = columnsOf(table->columns, kinds, shared);
    QStringList notes;
    if (!cols.extra.isEmpty())
        notes << QString("columns that are no slot, ignored: %1 (slots: %2)").arg(cols.extra.join(", "), QStringList(kinds.keys()).join(", "));
    if (!cols.unfilled.isEmpty())
        notes << QString("no column for the slot(s) %1: the template's content stays").arg(cols.unfilled.join(", "));

    // File names: the pattern, else the out/name column, else "<template>-001"
    QString pattern = a["name"].toString().trimmed();
    if (pattern.isEmpty()) pattern = cols.nameColumn.isEmpty() ? t.id + "-{n:03}" : "{" + cols.nameColumn + "}";
    if (const QString e = DataRows::patternError(pattern, table->columns); !e.isEmpty()) fail("BAD_ARGUMENT", e);
    QStringList names;
    for (int i = 0; i < table->rows.size(); ++i) {
        const std::optional<QString> n = DataRows::fileName(pattern, table->rows[i], i + 1, cols.fileColumns, 120, &err);
        if (!n) fail("BAD_ARGUMENT", err);
        names << *n;
    }
    names = DataRows::uniqueNames(names);

    const bool render = a.contains("render") && !(a["render"].isBool() && !a["render"].toBool());
    QJsonObject renderArgs = a["render"].isObject() ? a["render"].toObject() : QJsonObject();
    if (render && (renderArgs.contains("out") || renderArgs.contains("output")))
        fail("BAD_ARGUMENT", "fill with rows: each video goes to out_dir/<name> (no render.out)");
    if (render && a["render"].isString() && !QStringList{"true", "yes", "1", ""}.contains(a["render"].toString().trimmed().toLower()))
        fail("BAD_ARGUMENT", "render: the parameters of `render` as an object ({preset?, format?, …})");
    const QString extension = renderExtension(renderArgs);
    if (render && overwrite) renderArgs["overwrite"] = true;
    const QDir dir(outDir);
    if (!dry) QDir().mkpath(dir.absolutePath());

    // The template's own files once for the whole batch, not per row
    FillBatch batch;
    batch.mediaDir = dir.absoluteFilePath(t.id + " media");
    const int count = int(table->rows.size());
    QJsonArray results;
    int done = 0, failed = 0, skipped = 0;
    for (int i = 0; i < count; ++i) {
        const QString stage = QString("row %1/%2").arg(i + 1).arg(count);
        QJsonObject entry{{"row", i + 1}, {"name", names[i]}};
        if (failed && !keepGoing) {
            ++skipped;
            continue;
        }
        const double share = 1.0 / count, filled = render ? 0.15 : 1.0;
        if (ctx.stage) ctx.stage(stage);
        if (ctx.progress) ctx.progress(i * share);
        Context rc;
        rc.mcp = ctx.mcp;
        rc.partId = ctx.partId;
        if (ctx.progress) rc.progress = [&ctx, i, share, filled](double p) { ctx.progress((i + filled + (1 - filled) * p) * share); };
        if (ctx.stage) rc.stage = [&ctx, stage](const QString& s) { ctx.stage(stage + " " + s); };
        const QString project = dir.absoluteFilePath(names[i] + "." + ProjectFile::Extension);
        const QString video = dir.absoluteFilePath(names[i] + "." + extension);
        QJsonObject fa = a;
        for (const char* k : {"rows", "out_dir", "name", "render", "continue_on_error", "values"}) fa.remove(k);
        QJsonArray errors;
        try {
            fa["values"] = rowValues(table->rows[i], cols, kinds, base, shared);
            fa["out_project"] = project;
            // Checked first: a value that breaks a rule is reported per slot and nothing is written
            fa["dry_run"] = true;
            const QJsonObject check = fillTemplate(fa, rc);
            for (const QJsonValue& e : check["errors"].toArray()) errors << e;
            // (the check reports an existing project itself)
            if (!overwrite && render && QFileInfo::exists(video))
                errors << QJsonObject{{"code", "EXISTS"}, {"message", "video exists (overwrite:true replaces it): " + video}};
            if (!check["unfilled"].toArray().isEmpty()) entry["unfilled"] = check["unfilled"];
            entry["project"] = project;
            if (render) entry["video"] = video;
            if (errors.isEmpty() && !dry) {
                fa["dry_run"] = false;
                const QJsonObject r = fillTemplateInto(fa, rc, &batch);
                summarize(r, entry);
                if (r.contains("notes")) entry["notes"] = r["notes"];
                if (render) {
                    QJsonObject ra = renderArgs;
                    ra["out"] = video;
                    const QJsonObject out = renderFilled(ra, project, rc);
                    entry["video"] = out["out"].toString(video);
                    if (out.contains("render_s")) entry["render_s"] = out["render_s"];
                }
            }
        } catch (const Error& e) {
            errors << QJsonObject{{"code", e.code}, {"message", e.message}};
        }
        if (!errors.isEmpty()) {
            if (!dry && !QFileInfo::exists(project)) entry.remove("project");
            if (!dry && !QFileInfo::exists(video)) entry.remove("video");
        }
        entry["ok"] = errors.isEmpty();
        if (!errors.isEmpty()) entry["errors"] = errors;
        (errors.isEmpty() ? done : failed) += 1;
        results << entry;
    }
    if (ctx.progress) ctx.progress(1);
    QJsonObject o{{"template", QJsonObject{{"id", t.id}, {"name", t.name}}},
                  {"ok", failed == 0},
                  {"rows", results},
                  {"totals", QJsonObject{{"rows", count}, {"ok", done}, {"failed", failed}, {"skipped", skipped}}},
                  {"out_dir", dir.absolutePath()},
                  {"time_s", std::round(timer.elapsed() / 100.0) / 10}};
    if (dry) o["dry_run"] = true;
    if (table->separator == ';') notes << "CSV separated by ';'";
    if (!notes.isEmpty()) o["notes"] = QJsonArray::fromStringList(notes);
    return o;
}

} // namespace

// ---------- Shared with the edit ops (EditOpsTemplate.cpp) and `info` ----------

QJsonObject slotJson(const ClipSlot& s, const ProjectFormat& f)
{
    QJsonObject o{{"id", s.id}, {"kind", kSlotKindIds[int(s.kind)]}};
    if (!s.label.isEmpty()) o["label"] = s.label;
    if (!s.hint.isEmpty()) o["hint"] = s.hint;
    if (s.kind == SlotKind::Clip || s.kind == SlotKind::Music) {
        o["length"] = kSlotLengthIds[int(s.length)];
        if (s.minLen > 0) o["min_s"] = secs(s.minLen, f);
        if (s.maxLen > 0) o["max_s"] = secs(s.maxLen, f);
        o["fit"] = kSlotFitIds[int(s.fit)];
    }
    if (s.kind == SlotKind::Clip) o["reframe"] = s.reframe;
    if (s.kind == SlotKind::Text) {
        if (s.maxChars > 0) o["max_chars"] = s.maxChars;
        o["shrink"] = s.shrink;
    }
    if (s.kind == SlotKind::Music) {
        o["fit_music"] = s.fitMusic;
        o["snap_cuts"] = s.snapCuts;
    }
    return o;
}

QString fillProblemsText(const QVector<TemplateFill::Problem>& problems) { return problemsText(problems); }

TemplateFill::Values fillValues(const QJsonValue& v, const ProjectData& d, const ProjectFormat& f)
{
    if (!v.isObject()) fail("BAD_ARGUMENT", "values: an object {slot: \"file\" | {file, from?, to?} | \"text\"}");
    QHash<QString, SlotKind> kinds;
    for (const TemplateFill::SlotRef& r : TemplateFill::listSlots(d)) kinds.insert(r.slot.id, r.slot.kind);
    TemplateFill::Values out;
    const QJsonObject o = v.toObject();
    for (auto it = o.begin(); it != o.end(); ++it) {
        const QString id = it.key();
        const bool text = kinds.value(id, SlotKind::Clip) == SlotKind::Text;
        const QJsonValue x = it.value();
        if (x.isString()) {
            const QString str = x.toString();
            out[id] = text ? TemplateFill::Value::ofText(str)
                           : TemplateFill::Value::ofFile(str.trimmed().isEmpty() ? QString() : absolute(str.trimmed()));
        } else if (x.isObject()) {
            const QJsonObject vo = x.toObject();
            if (vo.contains("text")) {
                out[id] = TemplateFill::Value::ofText(vo["text"].toString());
                continue;
            }
            const QString file = vo["file"].toString().trimmed();
            if (file.isEmpty()) fail("BAD_ARGUMENT", QString("values.%1: {file, from?, to?} needs file").arg(id));
            const int from = vo.contains("from") ? parseTime(vo["from"], f, id + ".from") : -1;
            const int to = vo.contains("to") ? parseTime(vo["to"], f, id + ".to") : -1;
            if (from >= 0 && to >= 0 && to <= from) fail("BAD_ARGUMENT", QString("values.%1: to must be after from").arg(id));
            out[id] = TemplateFill::Value::ofFile(absolute(file), from, to);
        } else {
            fail("BAD_ARGUMENT", QString("values.%1: a file, {file, from?, to?} or a text").arg(id));
        }
    }
    return out;
}

TemplateFill::Context fillContext(const ProjectFormat& f, const QJsonObject& beats, bool measureReframe,
                                  QStringList* notes, Context* ctx)
{
    TemplateFill::Context c;
    // Each file probed once (check and apply both ask)
    auto probes = std::make_shared<QHash<QString, std::optional<MediaInfo>>>();
    c.probe = [f, probes](const QString& path) -> std::optional<MediaInfo> {
        if (const auto it = probes->constFind(path); it != probes->constEnd()) return *it;
        std::optional<MediaInfo> r;
        if (QFileInfo(path).isFile()) {
            const MediaInfo m = Engine::probe(f, path);
            if (m.length > 0 && (m.hasVideo || m.hasAudio)) r = m;
        }
        probes->insert(path, r);
        return r;
    };
    // Beats of a song (Music slots with fit_music): cached per file like op fit_music
    c.beats = [f, beats, probe = c.probe, ctx](const QString& path, int stream) -> std::optional<Beats::Result> {
        const std::optional<MediaInfo> m = probe(path);
        if (!m) return std::nullopt;
        try {
            return songBeats(f, *m, stream, beats, ctx);
        } catch (const Error&) {
            return std::nullopt; // no beat: trimmed with a fade-out (the report says so)
        }
    };
    // Auto reframe: the subject's path where the file was analysed before (or measured now with reframe:'auto')
    c.reframePath = [f, measureReframe, notes, probe = c.probe](const QString& file, int from, int length) {
        const std::optional<MediaInfo> m = probe(file);
        const int fileLength = m ? m->length : 0;
        Clip clip;
        clip.id = 1;
        clip.mediaPath = file;
        clip.in = from;
        clip.out = from + std::max(1, length) - 1;
        const std::optional<ReframeAnalysis::Need> n = ReframeAnalysis::need(clip, f.rate, fileLength);
        if (!n) return QVector<Reframe::Key>{};
        if (!measureReframe && !ReframeAnalysis::cached(*n)) {
            const QString note = QString("%1: reframed to the center (not analysed yet; reframe:'auto' follows the "
                                         "subject, slower)").arg(QFileInfo(file).fileName());
            if (notes && !notes->contains(note)) *notes << note;
            return QVector<Reframe::Key>{};
        }
        QStringList errors;
        const auto paths = ReframeAnalysis::paths({clip}, f, 0.5, [fileLength](const QString&) { return fileLength; }, &errors);
        if (notes)
            for (const QString& e : errors) *notes << "not measured, stays centered: " + e;
        return paths.value(1);
    };
    return c;
}

QJsonObject fillBeats(const ProjectData& d, const TemplateFill::Values& values, const ProjectFormat& f, Context& ctx)
{
    QJsonObject out;
    for (const TemplateFill::SlotRef& r : TemplateFill::listSlots(d)) {
        if (r.slot.kind != SlotKind::Music || !r.slot.fitMusic || !values.contains(r.slot.id)) continue;
        const QString file = values[r.slot.id].file;
        if (file.isEmpty() || !QFileInfo(file).isFile()) continue;
        const MediaInfo m = Engine::probe(f, file);
        if (!m.hasAudio || m.length <= 0) continue;
        const Timeline* tl = nullptr;
        for (const auto& [seq, t] : timelinesOf(d))
            if (!seq || seq->id == r.sequence) tl = t;
        const Clip* c = tl ? TimelineOps::findClip(*tl, r.clip) : nullptr;
        const int stream = c ? std::clamp(c->audioStream, 0, std::max(0, m.audioStreamCount() - 1)) : 0;
        const QString key = beatsKey(m.path, stream);
        if (out.contains(key)) continue;
        try {
            out[key] = songBeatsJson(songBeats(f, m, stream, {}, &ctx));
        } catch (const Error&) {
            // no beat: the fill trims it
        }
    }
    return out;
}

QJsonObject prepareTemplateFill(const QString& command, const QJsonObject& args, Context& ctx)
{
    QJsonObject out = args;
    bool autoDone = false; // the choice is made: a later failure (beats) must not send auto to the app
    try {
        if (command == "templates") {
            if (!args.contains("values") && !args.value("auto").toBool()) return args;
            Session s(args.value("project").toString()); // a copy of what the app shows
            const ProjectData d = s.project.data();
            if (args.value("auto").toBool()) {
                // The index is built here, not in the app; the app gets the values (and what was chosen, `_auto`)
                out = withAutoValues(args, d, true, ctx);
                autoDone = true;
            }
            const QJsonObject beats = fillBeats(d, fillValues(out.value("values"), d, d.format), d.format, ctx);
            if (!beats.isEmpty()) out["_beats"] = beats;
            return out;
        }
        const QJsonValue opsValue = args.value("ops");
        QJsonArray ops = opsValue.isObject() ? QJsonArray{opsValue} : opsValue.toArray();
        bool any = false;
        for (const QJsonValue& v : ops) any |= v.toObject().value("op").toString() == "fill";
        if (!any) return args;
        Session s(args.value("project").toString());
        const ProjectData d = s.project.data();
        for (QJsonValueRef v : ops) {
            QJsonObject op = v.toObject();
            if (op.value("op").toString() != "fill" || op.contains("_beats")) continue;
            const QJsonObject value{{op.value("slot").toString(), op.value("value")}};
            const QJsonObject beats = fillBeats(d, fillValues(value, d, d.format), d.format, ctx);
            if (beats.isEmpty()) continue;
            op["_beats"] = beats;
            v = op;
        }
        out["ops"] = ops;
    } catch (const Error& e) {
        if (e.code == "CANCELED") throw;
        return autoDone ? out : args; // the command reports it where it runs
    }
    return out;
}

QJsonObject fillTemplate(const QJsonObject& a, Context& ctx) { return fillTemplateInto(a, ctx, nullptr); }

QJsonObject fillTemplateInto(const QJsonObject& a0, Context& ctx, FillBatch* batch)
{
    const ContentTemplate t = *contentTemplate(a0["fill"].toString());
    ProjectData d = loadTemplate(t);
    const QJsonObject a = a0["auto"].toBool() ? withAutoValues(a0, d, false, ctx) : a0;
    const bool dry = a["dry_run"].toBool(), strict = a["strict"].toBool(), overwrite = a["overwrite"].toBool();
    TemplateFill::Values values = a.contains("values") ? fillValues(a["values"], d, d.format) : TemplateFill::Values{};
    const QJsonObject tpl{{"id", t.id}, {"name", t.name}};
    QStringList notes;
    // A logo slot without a value shows the brand kit's logo (the one `brand` gives, unless "none")
    if (!(a["brand"].isString() && a["brand"].toString().trimmed().toLower() == "none")) {
        QString logo = Brand::effective(d).logo;
        if (a["brand"].isObject() && a["brand"].toObject().contains("logo"))
            logo = a["brand"].toObject().value("logo").toString().trimmed();
        if (!logo.isEmpty() && QFileInfo(absolute(logo)).isFile())
            for (const TemplateFill::SlotRef& r : TemplateFill::listSlots(d))
                if (r.slot.kind == SlotKind::Logo && !values.contains(r.slot.id)) {
                    values[r.slot.id] = TemplateFill::Value::ofFile(absolute(logo));
                    notes << QString("%1: the brand kit's logo").arg(r.slot.id);
                }
    }
    // Resolved here too: direct's template recipe calls this without the parameter table (`mcp --root`: OUTSIDE_ROOT)
    const QString out = a["out_project"].toString().isEmpty() ? QString() : Sandbox::resolve(a["out_project"].toString());
    if (out.isEmpty() && !dry) fail("BAD_ARGUMENT", "fill needs out_project (the new project file)");
    if (!out.isEmpty()) checkedProjectPath(out, "out_project");
    // What would stop the write: dry_run reports it with the slots' problems, a real fill fails with it
    const bool replaces = !out.isEmpty() && QFileInfo::exists(out);
    QVector<TemplateFill::Problem> blocked;
    if (replaces && !overwrite)
        blocked << TemplateFill::Problem{{}, "EXISTS", "project already exists (pass overwrite to replace it): " + out};
    const LiveHost* h = Live::host();
    if (replaces && ((h && QFileInfo(h->liveProjectPath()) == QFileInfo(out)) || (!h && Live::connectionFor(out))))
        blocked << TemplateFill::Problem{{}, "IN_USE", "the project is open in the schneidi app; close it there first: " + out};
    if (!dry && !blocked.isEmpty()) fail(blocked.first().code, blocked.first().message);
    if (a.contains("brand") && a["brand"].isString()) {
        const QString b = a["brand"].toString().trimmed().toLower();
        if (b != "global" && b != "none") fail("BAD_ARGUMENT", "brand: \"global\", \"none\" or kit fields {colors, fonts, logo, …}");
    }
    const TemplateFill::Context fc = fillContext(d.format, a["_beats"].toObject(), reframeMode(a) == "auto", &notes, &ctx);
    const QStringList unfilled = unfilledOf(d, values);
    if (dry) {
        QJsonObject o = checkJson(blocked + TemplateFill::check(d, values, fc), unfilled, strict);
        o["template"] = tpl;
        addAutoPicks(a, o, notes);
        if (!notes.isEmpty()) o["notes"] = QJsonArray::fromStringList(notes);
        return o;
    }
    if (strict && !unfilled.isEmpty()) failUnfilled(unfilled);

    if (ctx.stage) ctx.stage("fill");
    const TemplateFill::Report report = TemplateFill::apply(d, values, fc);
    if (!report.ok()) fail("FILL_FAILED", problemsText(report.errors));
    // The brand kit into the titles that reference it; the result is a project, not a template any more
    Project p;
    p.load(d);
    Selection selection;
    Editor editor(&p, &selection);
    const QJsonObject brand = applyBrand(editor, p, a.contains("brand") ? a["brand"] : QJsonValue("global"));
    ProjectData result = p.data();
    result.templateInfo.reset();
    QDir().mkpath(QFileInfo(out).absolutePath());
    // Also for a new file: two fills to one name at once (two batches into one out_dir) take turns, and the later one
    // sees the file the first one made instead of replacing it silently
    std::unique_ptr<QLockFile> lock = ProjectFile::lock(out, ProjectFile::kCliLockWaitMs);
    if (!lock) fail("PROJECT_LOCKED", "the project is being saved by someone else (schneidi app?), try again: " + out);
    const bool exists = QFileInfo::exists(out);
    if (exists && !overwrite)
        fail("EXISTS", "project already exists (made meanwhile by another fill?); pass overwrite to replace it: " + out);
    // Only now the template's own files are copied (a fill that stops before leaves none behind): next to the
    // project ("<name> media/") or, for the rows of a batch, into their shared folder. Never over a user's file.
    TemplateLibrary::FileCopies copies;
    if (!(a.contains("copy_media") && !a["copy_media"].toBool())) {
        const QFileInfo of(out);
        const QString target = batch && !batch->mediaDir.isEmpty() ? batch->mediaDir
                                                                   : of.absoluteDir().filePath(of.completeBaseName() + " media");
        QString err;
        if (!TemplateLibrary::copyTemplateFiles(result, t.dir, target, &copies, &err, batch ? &batch->copies : nullptr))
            fail("SAVE_FAILED", err);
    }
    const auto dropCopies = [&] {
        for (const QString& f : std::as_const(copies.written)) QFile::remove(f);
        if (batch)
            for (const QString& f : std::as_const(copies.written)) batch->copies.remove(copies.moved.key(f));
    };
    // In a work folder only files there (a template file left out of the copies still points into the library)
    try {
        Sandbox::checkProject(result);
    } catch (const Error&) {
        dropCopies();
        throw;
    }
    const QString backup = exists ? ProjectFile::backup(out) : QString();
    QString error;
    if (!ProjectFile::save(result, out, &error)) {
        dropCopies();
        fail("SAVE_FAILED", error);
    }
    lock.reset();

    QJsonObject o = report.toJson();
    if (!unfilled.isEmpty())
        notes.prepend(QString("not filled, the template's placeholder content stays: %1").arg(unfilled.join(", ")));
    o["project"] = out;
    o["template"] = tpl;
    o["brand"] = brand;
    QJsonArray copied;
    for (const QString& dst : std::as_const(copies.moved)) copied << dst;
    if (!copied.isEmpty()) o["copied_media"] = copied;
    if (!backup.isEmpty()) o["backup"] = backup;
    // The project is saved: a failing render is reported next to it (a retry renders the project, it does not fill
    // again into an existing file)
    if (a.contains("render") && !(a["render"].isBool() && !a["render"].toBool())) {
        try {
            o["render"] = renderFilled(a["render"], out, ctx);
        } catch (const Error& e) {
            o["ok"] = false;
            o["render_error"] = QJsonObject{{"code", e.code}, {"message", e.message}};
            notes << QString("the project was saved, the render failed: render it with `render` {project: \"%1\"} "
                             "(filling again needs overwrite)").arg(out);
        }
    }
    addAutoPicks(a, o, notes);
    if (!notes.isEmpty()) o["notes"] = QJsonArray::fromStringList(notes);
    return o;
}

QJsonObject cmdTemplates(const QJsonObject& a, Context& ctx)
{
    QStringList given;
    for (const char* k : {"info", "fill", "save", "remove", "rename", "export", "import", "template"})
        if (a.contains(k)) given << k;
    if (a.contains("project") && !a.contains("fill")) given << "project";
    if (given.size() > 1)
        fail("BAD_ARGUMENT", QString("one action at a time (given: %1)").arg(given.join(", ")));
    const QString action = given.value(0);
    if (action == "fill") return a.contains("rows") ? fillRows(a, ctx) : fillTemplate(a, ctx);
    if (action == "project") return fillInPlace(a, ctx);
    if (action == "save") return saveTemplate(a);
    if (action == "export") return exportTemplate(a);
    if (action == "import") return importTemplate(a);
    if (action == "remove") {
        QString err, code;
        if (!TemplateLibrary::remove(a["remove"].toString().trimmed(), &err, &code)) fail(code, err);
        return {{"removed", a["remove"].toString().trimmed()}};
    }
    if (action == "rename") {
        QString err, code;
        if (a["name"].toString().trimmed().isEmpty()) fail("BAD_ARGUMENT", "rename needs name");
        if (!TemplateLibrary::rename(a["rename"].toString().trimmed(), a["name"].toString(), &err, &code)) fail(code, err);
        return contentJson(*contentTemplate(a["name"].toString()));
    }
    if (action == "info") {
        const QString key = a["info"].toString().trimmed();
        // A project (the app's, or a file that is no template folder): its slots as they are
        const bool project = Live::isAppProject(key)
                             || (isProjectFile(key) && QFileInfo(key).fileName() != TemplateLibrary::ProjectFileName);
        if (project) {
            Session s(key);
            QJsonObject o = infoJson(s.project.data(), std::nullopt);
            o["project"] = s.path;
            if (s.live) o["live"] = true;
            return o;
        }
        const ContentTemplate t = *contentTemplate(key);
        return infoJson(loadTemplate(t), t);
    }
    if (action == "template") {
        try {
            QJsonObject o = cmdStartTemplates(a, ctx);
            o["kind"] = "start";
            return o;
        } catch (const Error& e) {
            if (e.code != "NOT_FOUND") throw;
            QString err;
            const std::optional<ContentTemplate> t = TemplateLibrary::find(a["template"].toString(), &err);
            if (!t) throw;
            return infoJson(loadTemplate(*t), t);
        }
    }
    QJsonObject o = cmdStartTemplates({}, ctx);
    QJsonArray list;
    for (const QJsonValue& v : o["templates"].toArray()) {
        QJsonObject x = v.toObject();
        x["kind"] = "start";
        list << x;
    }
    for (const ContentTemplate& t : TemplateLibrary::all()) list << contentJson(t);
    o["templates"] = list;
    return o;
}

} // namespace Cli::detail
