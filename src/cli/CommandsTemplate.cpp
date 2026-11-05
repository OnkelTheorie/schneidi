// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli `templates`: start templates (core/StartTemplates, CommandsProject.cpp) and content templates
// (core/TemplateLibrary: a project with placeholder slots, core/TemplateFill). Lists both, shows a content template's
// slots, fills one into a new project (values checked against the slots' rules, brand kit applied, optionally
// rendered) or fills the slots of a project in place (live in the app: one undo step), saves/removes/renames own
// templates and shares them as ZIP (core/Zip).
#include "cli/CommandsDetail.h"

#include "cli/Live.h"
#include "cli/Sandbox.h"
#include "core/Beats.h"
#include "core/BrandKit.h"
#include "core/LiveHost.h"
#include "core/ProjectFile.h"
#include "core/TemplateFill.h"
#include "core/TemplateLibrary.h"
#include "core/TimelineOps.h"
#include "core/Zip.h"
#include "engine/Engine.h"
#include "engine/ReframeAnalysis.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRandomGenerator>
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
    f.rate = nearestFrameRate(t.fps > 0 ? t.fps : 25);
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
        editor.setProjectBrand(brandWithFields(p.effectiveBrand(), fields));
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

// Media of the template's own folder the filled project uses, copied next to it ("<name> media/"): the project must
// not depend on the template staying where it is (a built-in one lives in the cache). Unused ones leave the pool.
QJsonArray copyTemplateMedia(ProjectData& d, const ContentTemplate& t, const QString& out)
{
    const QString inside = QDir::cleanPath(QDir(t.dir).absolutePath()) + '/';
    QSet<QString> used;
    forEachTimeline(d, [&](Timeline& tl) {
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (const Track& tr : tl.tracks(k))
                for (const Clip& c : tr.clips)
                    if (c.kind == ClipKind::Media && c.mediaPath.startsWith(inside)) used.insert(c.mediaPath);
    });
    QVector<MediaInfo> kept;
    for (const MediaInfo& m : d.media)
        if (!m.path.startsWith(inside) || used.contains(m.path)) kept << m;
    d.media = kept;
    QJsonArray copied;
    if (used.isEmpty()) return copied;
    const QFileInfo of(out);
    const QDir target(of.absoluteDir().filePath(of.completeBaseName() + " media"));
    QHash<QString, QString> moved;
    for (const QString& src : used) {
        const QString dst = QDir::cleanPath(target.absoluteFilePath(src.mid(inside.size())));
        if (!(QFileInfo(dst).exists() && QFileInfo(dst).size() == QFileInfo(src).size())) {
            QDir().mkpath(QFileInfo(dst).absolutePath());
            QFile::remove(dst);
            if (!QFile::copy(src, dst)) fail("SAVE_FAILED", QString("cannot copy %1 to %2").arg(src, dst));
        }
        moved[src] = dst;
        copied << dst;
    }
    for (MediaInfo& m : d.media) m.path = moved.value(m.path, m.path);
    forEachTimeline(d, [&](Timeline& tl) {
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (Track& tr : tl.tracks(k))
                for (Clip& c : tr.clips)
                    if (c.kind == ClipKind::Media) c.mediaPath = moved.value(c.mediaPath, c.mediaPath);
    });
    return copied;
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

// ---------- Actions ----------

QJsonObject fillInPlace(const QJsonObject& a, Context& ctx)
{
    if (!a.contains("values")) fail("BAD_ARGUMENT", "give values {slot: value} to fill the project's slots");
    const bool dry = a["dry_run"].toBool();
    Session s(a["project"].toString(), dry ? Session::Access::Read : Session::Access::Write);
    const ProjectData d = s.project.data();
    const TemplateFill::Values values = fillValues(a["values"], d, d.format);
    QStringList notes;
    const TemplateFill::Context fc = fillContext(d.format, a["_beats"].toObject(), reframeMode(a) == "auto", &notes, &ctx);
    const QStringList unfilled = unfilledOf(d, values);
    if (dry) {
        QJsonObject o = checkJson(TemplateFill::check(d, values, fc), unfilled, a["strict"].toBool());
        o["project"] = s.path;
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
    QString err;
    const std::optional<ContentTemplate> t = TemplateLibrary::save(d, name, o, &err);
    if (!t) fail(err.contains("exists") ? "EXISTS" : "SAVE_FAILED", err);
    QJsonObject out = contentJson(*t);
    out["saved"] = true;
    if (!s.notes.isEmpty()) out["notes"] = s.notes;
    return out;
}

QJsonObject exportTemplate(const QJsonObject& a)
{
    const ContentTemplate t = *contentTemplate(a["export"].toString());
    const QString zip = a["zip"].toString();
    if (zip.isEmpty()) fail("BAD_ARGUMENT", "export needs zip (the file to write)");
    if (!zip.endsWith(".zip", Qt::CaseInsensitive)) fail("BAD_ARGUMENT", "zip must end in .zip");
    if (QFileInfo::exists(zip) && !a["overwrite"].toBool()) fail("EXISTS", "file exists (pass overwrite to replace it): " + zip);
    QVector<Zip::Source> files;
    const QDir root(t.dir);
    QDirIterator it(t.dir, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        files << Zip::Source{t.id + "/" + root.relativeFilePath(file), file};
    }
    std::sort(files.begin(), files.end(), [](const Zip::Source& x, const Zip::Source& y) { return x.name < y.name; });
    QDir().mkpath(QFileInfo(zip).absolutePath());
    const QString part = zip + ".part";
    QString err;
    if (!Zip::write(part, files, &err)) {
        QFile::remove(part);
        fail("SAVE_FAILED", err);
    }
    QFile::remove(zip);
    if (!QFile::rename(part, zip)) {
        QFile::remove(part);
        fail("SAVE_FAILED", "cannot write " + zip);
    }
    return {{"zip", zip}, {"template", QJsonObject{{"id", t.id}, {"name", t.name}}}, {"files", int(files.size())},
            {"size", QFileInfo(zip).size()}};
}

QJsonObject importTemplate(const QJsonObject& a)
{
    const QString zip = a["import"].toString();
    if (!QFileInfo::exists(zip)) fail("NOT_FOUND", "no file " + zip);
    QString err;
    const std::optional<QVector<Zip::Entry>> list = Zip::entries(zip, &err);
    if (!list) fail("BAD_ZIP", err);
    // The template's folder in the archive: its root, or the one folder everything is in
    QString prefix;
    bool atRoot = false;
    QSet<QString> tops;
    for (const Zip::Entry& e : *list) {
        const QString n = Zip::safeName(e.name);
        if (n.isEmpty()) continue;
        if (n == TemplateLibrary::ProjectFileName) atRoot = true;
        tops.insert(n.section('/', 0, 0));
    }
    if (!atRoot) {
        if (tops.size() == 1) prefix = *tops.begin();
        bool found = false;
        for (const Zip::Entry& e : *list)
            found |= !prefix.isEmpty() && Zip::safeName(e.name) == prefix + "/" + TemplateLibrary::ProjectFileName;
        if (!found) fail("BAD_TEMPLATE", QString("no %1 in %2 (a template is a folder with it)")
                                              .arg(TemplateLibrary::ProjectFileName, QFileInfo(zip).fileName()));
    }
    const QDir base(TemplateLibrary::dir());
    QDir().mkpath(base.absolutePath());
    const QString staging = base.filePath(QString(".import-%1").arg(QRandomGenerator::global()->generate(), 8, 16, QChar('0')));
    const auto cleanUp = [&] { QDir(staging).removeRecursively(); };
    if (!Zip::extract(zip, staging, prefix, &err)) {
        cleanUp();
        fail("BAD_ZIP", err);
    }
    std::optional<ContentTemplate> t = TemplateLibrary::open(staging, &err);
    if (!t) {
        cleanUp();
        fail("BAD_TEMPLATE", err);
    }
    ProjectData d;
    TemplateLibrary::load(*t, &d, &err);
    QString name = d.templateInfo && !d.templateInfo->name.trimmed().isEmpty() ? d.templateInfo->name.trimmed()
                   : !prefix.isEmpty()                                       ? prefix
                                                                             : QFileInfo(zip).completeBaseName();
    QString id = TemplateLibrary::idForName(name);
    if (QFileInfo::exists(base.filePath(id))) {
        if (a["replace"].toBool()) {
            if (!QDir(base.filePath(id)).removeRecursively()) {
                cleanUp();
                fail("SAVE_FAILED", "cannot replace " + base.filePath(id));
            }
        } else { // a name of its own: "Name 2"
            const QString shown = name;
            for (int n = 2; QFileInfo::exists(base.filePath(id)); ++n) {
                name = QString("%1 %2").arg(shown).arg(n);
                id = TemplateLibrary::idForName(name);
            }
        }
    }
    const QString final = base.filePath(id);
    if (!QDir().rename(staging, final)) {
        cleanUp();
        fail("SAVE_FAILED", "cannot create " + final);
    }
    // From its place: media paths into the new folder, the (unique) name in the project
    std::optional<ContentTemplate> placed = TemplateLibrary::open(final, &err);
    if (placed) {
        ProjectData pd;
        if (TemplateLibrary::load(*placed, &pd, &err)) {
            TemplateInfo info = pd.templateInfo.value_or(TemplateInfo{});
            info.name = name;
            pd.templateInfo = info;
            ProjectFile::save(pd, placed->project, &err);
        }
        placed = TemplateLibrary::open(final, &err);
    }
    if (!placed) fail("BAD_TEMPLATE", err);
    QJsonObject out = contentJson(*placed);
    out["imported"] = true;
    return out;
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
    try {
        if (command == "templates") {
            if (!args.contains("values")) return args;
            Session s(args.value("project").toString()); // a copy of what the app shows
            const ProjectData d = s.project.data();
            const QJsonObject beats = fillBeats(d, fillValues(args.value("values"), d, d.format), d.format, ctx);
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
    } catch (const Error&) {
        return args; // the command reports it where it runs
    }
    return out;
}

QJsonObject fillTemplate(const QJsonObject& a, Context& ctx)
{
    const ContentTemplate t = *contentTemplate(a["fill"].toString());
    ProjectData d = loadTemplate(t);
    const bool dry = a["dry_run"].toBool(), strict = a["strict"].toBool();
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
    const QString out = a["out_project"].toString();
    if (out.isEmpty() && !dry) fail("BAD_ARGUMENT", "fill needs out_project (the new project file)");
    if (!out.isEmpty()) checkedProjectPath(out, "out_project");
    const bool replaces = !out.isEmpty() && QFileInfo::exists(out);
    if (replaces && !a["overwrite"].toBool() && !dry)
        fail("EXISTS", "project already exists (pass overwrite to replace it): " + out);
    const LiveHost* h = Live::host();
    if (replaces && !dry && ((h && QFileInfo(h->liveProjectPath()) == QFileInfo(out)) || (!h && Live::connectionFor(out))))
        fail("IN_USE", "the project is open in the schneidi app; close it there first: " + out);
    if (a.contains("brand") && a["brand"].isString()) {
        const QString b = a["brand"].toString().trimmed().toLower();
        if (b != "global" && b != "none") fail("BAD_ARGUMENT", "brand: \"global\", \"none\" or kit fields {colors, fonts, logo, …}");
    }
    const TemplateFill::Context fc = fillContext(d.format, a["_beats"].toObject(), reframeMode(a) == "auto", &notes, &ctx);
    const QStringList unfilled = unfilledOf(d, values);
    if (dry) {
        QJsonObject o = checkJson(TemplateFill::check(d, values, fc), unfilled, strict);
        o["template"] = tpl;
        return o;
    }
    if (strict && !unfilled.isEmpty()) failUnfilled(unfilled);

    if (ctx.stage) ctx.stage("fill");
    const TemplateFill::Report report = TemplateFill::apply(d, values, fc);
    if (!report.ok()) fail("FILL_FAILED", problemsText(report.errors));
    const QJsonArray copied = a.contains("copy_media") && !a["copy_media"].toBool() ? QJsonArray()
                                                                                    : copyTemplateMedia(d, t, out);
    // The brand kit into the titles that reference it; the result is a project, not a template any more
    Project p;
    p.load(d);
    Selection selection;
    Editor editor(&p, &selection);
    const QJsonObject brand = applyBrand(editor, p, a.contains("brand") ? a["brand"] : QJsonValue("global"));
    ProjectData result = p.data();
    result.templateInfo.reset();
    QDir().mkpath(QFileInfo(out).absolutePath());
    std::unique_ptr<QLockFile> lock;
    QString backup;
    if (replaces) {
        lock = ProjectFile::lock(out);
        if (!lock) fail("PROJECT_LOCKED", "the project is being saved by someone else (schneidi app?), try again: " + out);
        backup = ProjectFile::backup(out);
    }
    QString error;
    if (!ProjectFile::save(result, out, &error)) fail("SAVE_FAILED", error);
    lock.reset();

    QJsonObject o = report.toJson();
    if (!unfilled.isEmpty())
        notes.prepend(QString("not filled, the template's placeholder content stays: %1").arg(unfilled.join(", ")));
    o["project"] = out;
    o["template"] = tpl;
    o["brand"] = brand;
    if (!copied.isEmpty()) o["copied_media"] = copied;
    if (!backup.isEmpty()) o["backup"] = backup;
    if (a.contains("render") && !(a["render"].isBool() && !a["render"].toBool())) o["render"] = renderFilled(a["render"], out, ctx);
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
    if (action == "fill") return fillTemplate(a, ctx);
    if (action == "project") return fillInPlace(a, ctx);
    if (action == "save") return saveTemplate(a);
    if (action == "export") return exportTemplate(a);
    if (action == "import") return importTemplate(a);
    if (action == "remove") {
        QString err;
        if (!TemplateLibrary::remove(a["remove"].toString().trimmed(), &err)) fail("NOT_FOUND", err);
        return {{"removed", a["remove"].toString().trimmed()}};
    }
    if (action == "rename") {
        QString err;
        if (a["name"].toString().trimmed().isEmpty()) fail("BAD_ARGUMENT", "rename needs name");
        if (!TemplateLibrary::rename(a["rename"].toString().trimmed(), a["name"].toString(), &err)) fail("NOT_FOUND", err);
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
