// SPDX-License-Identifier: AGPL-3.0-only
// The short texts of the MCP tool list, see ToolText.h. Rules for a tool's text: what it is for and when to use it in
// one or two sentences, plus what an AI gets wrong without being told; parameters in a few words (or none when the
// name says it), fixed value lists as `choices`. No answer fields, error codes, defaults or examples: `help` shows the
// command's full texts. Keep the whole list under the budget of test_mcp_tool_size.
#include "cli/ToolText.h"

#include "cli/Commands.h"
#include "cli/CommandsDetail.h"

#include <QJsonArray>

#include <algorithm>
#include <initializer_list>

namespace Cli::ToolText {

namespace {

struct P {
    const char* name;
    const char* text = "";
    QStringList choices = {};
};

struct Def {
    const char* tool;
    const char* description;
    std::initializer_list<P> params;
    QStringList more;
};

const QHash<QString, Tool>& tools()
{
    static const QHash<QString, Tool> all = [] {
        const std::initializer_list<Def> defs = {
            {"help",
             "Details of a tool (every parameter, defaults, answer fields, errors) or of edit ops. Call it before using "
             "a tool or an op for the first time.",
             {{"command", "tool name"}, {"op", "with command 'edit': op name(s), comma list"}},
             {"all"}},
            {"guide",
             "How to edit well with schneidi: the workflow, and how-tos per task (topics). Read it first.",
             {{"topic", "e.g. highlights, talking, social, subtitles, audio, color, export"}},
             {}},
            {"new",
             "Create a project (.schneidi), optionally with media. Size and frame rate come from the first video unless "
             "given. template: neutral, game, podcast, vertical (9:16) or one from `templates` (default: the user's).",
             {{"project", "file to create", {}}, {"template"}, {"width"}, {"height"}, {"fps"}, {"media", "files to import"},
              {"overwrite"}},
             {}},
            {"templates",
             "Start templates (for `new`) and content templates (finished projects with slots for clips, texts, music, "
             "logo). No parameters: the list. info: a template's slots. fill + values: a new project (rows: one per "
             "table row). project + values: fill in place. auto: best shots for empty slots.",
             {{"template"}, {"info", "template whose slots to show"}, {"fill", "template to fill"},
              {"values", "slot id -> file | {file, from, to} | text"}, {"rows", "CSV/JSON table, a column per slot"},
              {"out_project"}, {"out_dir", "with rows"}, {"project"}, {"auto"}, {"render", "render parameters"},
              {"dry_run"}},
             {"continue_on_error", "brand", "reframe", "strict", "overwrite", "copy_media", "save", "name", "description",
              "tags", "preview", "replace", "remove", "rename", "export", "zip", "import", "files"}},
            {"info",
             "A project: format, media, its timelines and one timeline's tracks and clips (ids, positions, source "
             "ranges). Open in the app: also the playhead and selection.",
             {{"project"}, {"summary", "counts only, no clip list"}, {"timeline"}},
             {}},
            {"import",
             "Add media files to the media pool, or a timeline of another editor (FCP7 XML, .otio, .edl) as a new "
             "timeline.",
             {{"project"}, {"files"}, {"timeline_file", ".xml, .otio or .edl"}},
             {"format", "search", "name", "fps"}},
            {"export_timeline",
             "Write a timeline for another editor: FCP7 XML (Premiere, Resolve), .otio or .edl.",
             {{"project"}, {"out", ".xml, .otio or .edl"}, {"timeline"}, {"overwrite"}},
             {"format"}},
            {"edit",
             "Change a timeline: a list of ops, all or nothing, saved with a backup (open in the app: one undo step, "
             "not saved). The answer lists what changed with the new clip ids. plan:true shows the steps first "
             "(then plan_id + run).",
             {{"project"}, {"ops"}, {"timeline", "timeline to edit"}, {"dry_run"}, {"plan"},
              {"plan_id"}, {"run", "plan steps [1,2] or \"all\""}},
             {"discard", "full", "trust_custom_code"}},
            {"backups", "The saved states of a project, newest first (for `restore`).", {{"project"}}, {}},
            {"restore",
             "Undo: put a backup back as the project (the current state becomes a backup too).",
             {{"project"}, {"backup", "number from `backups` (1 = before the last change)"}},
             {}},
            {"app",
             "Control the schneidi app that has the project open: state (playhead, selection, page), seek, play, "
             "select clips, show a page, undo/redo, save, open files, proxies, render cache.",
             {{"action", "",
               {"state", "seek", "play", "stop", "select", "page", "undo", "redo", "save", "open", "proxies",
                "render_cache", "clean_proxies"}},
              {"at"},
              {"clips", "clip ids"},
              {"page", "", {"media", "direct", "edit", "comp", "color", "studio", "deliver"}},
              {"files"}},
             {"project", "generate", "delete", "mode", "pause", "path", "trust_custom_code"}},
            {"watch",
             "Watch folders: new media files there come into the media pool (a recorder's output). add a folder; "
             "without add/remove/list one pass imports what is new and complete ('new'); since = the revision of your "
             "last answer.",
             {{"project"}, {"add", "folder"}, {"bin"}, {"remove", "folder"}, {"list"}, {"since"}},
             {"subfolders", "types", "existing", "picture_aspect", "settle_s", "follow"}},
            {"brand",
             "The user's brand kit: logo, up to 4 colors, 2 fonts, intro/outro; global or per project. Titles take "
             "color \"brand1\"..\"brand4\" and font \"brand1\"/\"brand2\".",
             {{"project"},
              {"action", "", {"show", "set", "apply", "unset", "logo", "intro_outro"}},
              {"global", "set: the global kit (else the project's)"},
              {"logo", "file"},
              {"colors", "comma list"},
              {"fonts", "heading, body"},
              {"intro", "file"},
              {"outro", "file"}},
             {"logo_corner", "logo_size", "timeline"}},
            {"probe",
             "Media file details: length, frames, size, frame rate, audio streams and channels. gaps: stutter of "
             "game recordings; bars: black bars baked into the picture.",
             {{"file"}, {"files"}, {"project", "count frames in its rate"}, {"gaps"}, {"bars"}},
             {}},
            {"silence",
             "Pauses in the sound of a media file or a project's timeline: 'silence' and 'sound' ranges. File: 'sound' "
             "fits op keep; project: 'silence' fits op delete_range (or use op cut_pauses).",
             {{"file", "media file or project"}, {"project", "count frames in its rate"}, {"threshold_db"}, {"min"},
              {"pad"}},
             {"timeline", "tracks", "from", "to"}},
            {"overview",
             "Cheap first look at a file or project: per stretch of time loudness, silence, motion, cuts, brightness "
             "(or clips). Read it first, then the detailed analyses.",
             {{"file"}, {"files"}},
             {"window", "timeline", "audio_stream", "project"}},
            {"scenes",
             "Scene changes of a video: 'cuts' and 'scenes' in source frames. region (+ color) finds events in one "
             "part of the picture instead (kill feed lines, HUD changes).",
             {{"file"}, {"files"}, {"region", "x,y,w,h in pixels or 0..1"}, {"color", "with region: #rrggbb"},
              {"project", "count frames in its rate"}, {"threshold", "0..1, lower = more cuts"}, {"min"}},
             {"tolerance", "only", "min_area"}},
            {"peaks",
             "Loud moments of media files (shots, explosions, shouts, applause). events:true also says what is heard "
             "(speech, music, laughter, applause, cheering, shot, scream).",
             {{"file"}, {"files"}, {"threshold_db"}, {"min"}, {"top"}, {"group", "bursts of peaks closer than this"},
              {"events"}, {"project", "count frames in its rate"}},
             {"audio_stream", "event_kinds", "min_conf"}},
            {"beats",
             "A song's tempo and structure: bpm, beat grid, bar ones, phrases, energy, breaks and drops. Cut on beats, "
             "put strong moments on drops. Source: a file or a project's sound.",
             {{"file", "song, or a project"}, {"beats", "also every beat"}, {"apply", "markers on the timeline", {"markers"}}},
             {"project", "timeline", "tracks", "from", "to", "audio_stream", "min_bpm", "max_bpm", "markers"}},
            {"index",
             "Material index: every shot of each file with quality, speech, sounds, tags and descriptions (cached). "
             "query finds shots (\"calm wide shot of the shop\"); annotate writes descriptions.",
             {{"file"}, {"files"}, {"project"}, {"query"}, {"annotate", "[{file, shot, desc, tags}]"}},
             {"timeline", "top", "refresh", "thumbs", "sheet"}},
            {"highlights",
             "Candidates for a highlight video, best first: kills of a game's kill feed and loud moments, as source "
             "ranges. 'keep' per file fits op keep ('keep_clean' without stutter); hit_s = the hit, for accents.",
             {{"file"}, {"files"}, {"game", "default: from the file or folder name", {"cs2", "valorant", "none"}},
              {"top"}, {"pre", "lead-in"}, {"post", "lead-out"}, {"project", "count frames in its rate"}},
             {"region", "color", "tolerance", "min_area", "join", "threshold_db", "audio_stream"}},
            {"transcribe",
             "Speech to text (whisper/Parakeet, offline; install with `extensions`): segments with times, words:true "
             "every word. Source: a media file or a project; apply puts a subtitle or caption track on the timeline.",
             {{"source"}, {"language", "de, en, … (default: detect)"}, {"words"}, {"srt", "also write this SRT file"},
              {"apply"},
              {"captions", "with apply: word captions", {"karaoke", "word", "beat"}},
              {"length", "cue length", {"auto", "normal", "short", "very_short"}},
              {"quality", "", {"accurate", "fast", "very_fast"}}},
             {"project", "model", "music", "max_chars", "audio_stream", "from", "to", "timeline", "tracks", "name",
              "readable", "max_words", "beats", "bpm", "first_beat", "threads", "verbatim"}},
            {"loudness",
             "Loudness (EBU R128): integrated LUFS, range, true peak of a file, a project's mix or single clips. "
             "Web: -14 LUFS / -1 dBTP; broadcast -23.",
             {{"source"}, {"tracks"}, {"clips", "measure these clips one by one"}},
             {"timeline", "from", "to", "channels", "audio_stream"}},
            {"check",
             "Finds what typically goes wrong, before rendering (project) or handing over (rendered file): missing "
             "media, gaps, sound (clipping, loudness, music over voice), unreadable subtitles, text under phone UI, "
             "slow pace, late hook. Its 'fix' ops: apply them with `edit`.",
             {{"source", "project or rendered file"},
              {"for", "", {"shorts", "reels", "tiktok", "podcast", "youtube", "web", "broadcast"}},
              {"target_lufs"},
              {"timeline"}},
             {"from", "to", "channels", "sound", "max_still_s", "retention"}},
            {"scopes",
             "Video scopes of one frame (waveform, parade, vectorscope, histogram) as an image, with numbers for "
             "exposure and color cast.",
             {{"source"}, {"at"}},
             {"types", "out", "timeline", "project"}},
            {"extensions",
             "Optional downloads: whisper and models, x264, piper voices, faces, cutout. No parameters: the list with "
             "sizes. Ask the user before installing.",
             {{"install", "ids, e.g. whisper,small"}, {"remove"}},
             {"reinstall"}},
            {"effects",
             "Ids and parameters for edit ops: effects, LUTs, transitions, effect and accent presets, sounds; kind "
             "music/nodes/recipes: music bed moods, compositing nodes and recipes.",
             {{"effect", "one effect's parameters"}, {"category"}, {"kind", "", {"sfx", "music", "nodes", "recipes"}}},
             {"favorites", "export", "to", "import", "remove", "replace"}},
            {"sync",
             "Offsets of recordings of the same event by their shared sound (the first file is the reference). Op "
             "multicam syncs by itself; this checks first.",
             {{"files"}},
             {"stream", "max_offset_s", "project"}},
            {"frames",
             "Still images of a media file or a project timeline, or one contact sheet: look at material and results. "
             "crop = a detail (HUD, kill feed).",
             {{"source"}, {"sources", "several files on one sheet"}, {"at"}, {"every", "e.g. \"10s\""}, {"count"},
              {"sheet"}, {"crop", "x,y,w,h in pixels or 0..1"}},
             {"from", "to", "out", "columns", "tile_width", "width", "project", "timeline", "still", "clip", "node",
              "view", "guides"}},
        };
        QHash<QString, Tool> out;
        for (const Def& d : defs) {
            Tool t;
            t.description = QString::fromUtf8(d.description);
            for (const P& p : d.params) t.params.insert(QString::fromUtf8(p.name), {QString::fromUtf8(p.text), p.choices});
            t.more = d.more;
            out.insert(QString::fromUtf8(d.tool), t);
        }
        return out;
    }();
    return all;
}

} // namespace

const Tool* find(const QString& command)
{
    const auto it = tools().constFind(command);
    return it == tools().constEnd() ? nullptr : &*it;
}

QStringList choices(const QString& command, const QString& param)
{
    const Tool* t = find(command);
    return t ? t->params.value(param).choices : QStringList();
}

QString description(const Command& cmd)
{
    const Tool* t = find(cmd.name);
    return t ? t->description : cmd.summary;
}

QJsonObject schema(const Command& cmd)
{
    const Tool* t = find(cmd.name);
    QJsonObject s = inputSchema(cmd);
    if (!t) return s;
    QJsonObject props = s["properties"].toObject();
    QJsonObject out;
    for (const Param& p : cmd.params) {
        if (!t->params.contains(p.name)) continue;
        QJsonObject o = props[p.name].toObject();
        const ToolText::Param& brief = t->params[p.name];
        if (p.name == "ops" && cmd.name == "edit") o["description"] = editOpsBrief();
        else if (brief.text.isEmpty()) o.remove("description");
        else o["description"] = brief.text;
        out[p.name] = o;
    }
    s["properties"] = out;
    return s;
}

const QVector<std::pair<QString, QStringList>>& editOpGroups()
{
    static const QVector<std::pair<QString, QStringList>> groups{
        {"cut", {"append", "place", "keep", "insert", "copy", "fit_to_fill", "split", "trim", "move", "delete",
                 "delete_range", "clear", "enable", "link", "align", "cut_pauses", "jl_cut", "rough_cut", "best_takes",
                 "hide_jumps", "broll"}},
        {"timeline", {"timeline", "new_timeline", "rename_timeline", "duplicate_timeline", "remove_timeline", "format",
                      "add_track", "remove_track", "track", "compound", "decompose", "place_timeline", "marker", "marks",
                      "section", "sections", "note", "hook_variants"}},
        {"picture", {"effect", "transform", "reframe", "crop_bars", "picture_aspect", "hdr", "speed", "speed_ramp",
                     "fade", "transition", "keyframe", "accent", "copy_attributes", "blur_names"}},
        {"color", {"color", "still", "qualifier", "window", "track_window"}},
        {"sound", {"volume", "mixer", "duck", "normalize", "clean_voice", "audio_stream", "route", "track_volume",
                   "track_pan", "speak", "voiceover", "room_tone", "sfx", "music_bed", "fit_music", "snap_to_beat"}},
        {"text and graphics", {"title", "subtitle", "subtitles", "add_subtitle_track", "captions", "shape", "solid",
                               "clip_shape", "draw"}},
        {"compositing", {"comp", "comp_from", "node", "track_node", "pick_region", "cutout_node"}},
        {"multicam", {"multicam", "angle", "angle_cuts", "flatten", "multicam_nudge", "add_angle", "angle_name",
                      "source_marker"}},
        {"media pool", {"media", "bin", "remove_media"}},
        {"template slots", {"slot", "fill"}},
    };
    return groups;
}

QString editOpsBrief()
{
    QStringList grouped, seen;
    for (const auto& [group, ops] : editOpGroups()) {
        grouped << group + ": " + ops.join(", ");
        seen << ops;
    }
    // An op no group names yet (test_mcp_tool_size asks for one) is still listed
    QStringList other;
    for (const detail::OpDef& d : detail::editOps())
        if (!seen.contains(d.name)) other << d.name;
    if (!other.isEmpty()) grouped << "other: " + other.join(", ");
    return "[{op:'name', …keys}], applied in order. Ops - " + grouped.join("; ")
           + ". Times: frames, \"4.5s\", \"MM:SS\" or timecode; ranges [from, to). Tracks \"V1\", \"A2\". Clip ids "
             "from `info` or the last edit's answer. Keys of an op: help {command:'edit', op:'name'} (ask before "
             "using an op the first time); general rules: help {command:'edit'}.";
}

QStringList nearest(const QString& name, const QStringList& candidates, int max)
{
    const QString a = name.toLower();
    const auto distance = [](const QString& x, const QString& y) {
        QVector<int> row(y.size() + 1);
        for (int j = 0; j <= y.size(); ++j) row[j] = j;
        for (int i = 1; i <= x.size(); ++i) {
            int diag = row[0];
            row[0] = i;
            for (int j = 1; j <= y.size(); ++j) {
                const int up = row[j];
                row[j] = std::min({row[j] + 1, row[j - 1] + 1, diag + (x[i - 1] == y[j - 1] ? 0 : 1)});
                diag = up;
            }
        }
        return row[y.size()];
    };
    QVector<std::pair<int, QString>> close;
    for (const QString& c : candidates) {
        const QString b = c.toLower();
        if (b == a) continue;
        const int d = distance(a, b);
        // A typing error (about a third of the letters), or one name inside the other ("ramp" in "speed_ramp")
        const bool near = d <= std::max<qsizetype>(1, std::max(a.size(), b.size()) / 3);
        const bool inside = std::min(a.size(), b.size()) >= 4 && (a.contains(b) || b.contains(a));
        if (near || inside) close.push_back({inside && !near ? d + 100 : d, c});
    }
    std::stable_sort(close.begin(), close.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    QStringList out;
    for (const auto& [d, c] : close) {
        if (out.size() >= max) break;
        out << c;
    }
    return out;
}

} // namespace Cli::ToolText
