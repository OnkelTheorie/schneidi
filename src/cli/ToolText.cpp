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
#include <cstdlib>
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
             "Every parameter of a tool (defaults, answer fields, errors), or an edit op's keys. Call it before using a "
             "tool or op the first time.",
             {{"command"}, {"op", "with command 'edit': op name(s)"}},
             {"all"}},
            {"guide",
             "How to edit well: the workflow and how-tos per task. Read it first.",
             {{"topic", "e.g. highlights, talking, social, subtitles, audio, color"}},
             {}},
            {"new",
             "Create a project (.schneidi), optionally with media; size and frame rate from the first video. template: "
             "neutral, game, podcast, vertical (9:16) or one of `templates`.",
             {{"project"}, {"template"}, {"width"}, {"height"}, {"media"}},
             {"fps", "overwrite"}},
            {"templates",
             "Start templates (for `new`) and content templates (projects with slots). No parameters: the list. info: "
             "a template's slots. fill + values: a new project (rows: one per table row).",
             {{"template"}, {"info"}, {"fill"}, {"values", "slot id -> file | {file, from, to} | text"},
              {"rows", "CSV/JSON table"}, {"out_project"}, {"out_dir"}, {"project", "fill this one in place"}},
             {"continue_on_error", "brand", "reframe", "strict", "overwrite", "copy_media", "save", "name", "description",
              "tags", "preview", "replace", "remove", "rename", "export", "zip", "import", "files", "auto", "render",
              "dry_run"}},
            {"info",
             "A project: format, media, timelines, and one timeline's tracks and clips (ids, positions). In the app: "
             "also playhead and selection.",
             {{"project"}, {"summary", "counts, no clips"}, {"timeline"}},
             {}},
            {"import",
             "Add media files to the media pool, or another editor's timeline (FCP7 XML, .otio, .edl).",
             {{"project"}, {"files"}, {"timeline_file"}},
             {"format", "search", "name", "fps"}},
            {"export_timeline",
             "Write a timeline for another editor: FCP7 XML (Premiere, Resolve), .otio or .edl.",
             {{"project"}, {"out"}, {"timeline"}},
             {"format", "overwrite"}},
            {"edit",
             "Change a timeline with a list of ops, all or nothing; saved with a backup (in the app: one undo step). "
             "The answer lists the changes with new clip ids.",
             {{"project"}, {"ops"}, {"timeline"}, {"dry_run"}, {"plan"}, {"plan_id"}, {"run", "plan steps or \"all\""}},
             {"discard", "full", "trust_custom_code"}},
            {"backups", "Saved states of a project, newest first.", {{"project"}}, {}},
            {"restore",
             "Undo: put a backup back (default: the state before the last change).",
             {{"project"}, {"backup"}},
             {}},
            {"app",
             "Control the schneidi app that has the project open.",
             {{"action", "",
               {"state", "seek", "play", "stop", "select", "page", "undo", "redo", "save", "open", "proxies",
                "render_cache", "clean_proxies"}},
              {"at"},
              {"clips"},
              {"page", "", {"media", "direct", "edit", "comp", "color", "studio", "deliver"}},
              {"files"}},
             {"project", "generate", "delete", "mode", "pause", "path", "trust_custom_code"}},
            {"watch",
             "Watch folders: their new media files come into the media pool. add a folder; without add/remove/list: one "
             "pass, new files in 'new'.",
             {{"project"}, {"add", "folder"}, {"list"}, {"since", "revision of your last answer"}},
             {"bin", "remove", "subfolders", "types", "existing", "picture_aspect", "settle_s", "follow"}},
            {"brand",
             "The user's brand kit (logo, 4 colors, 2 fonts, intro/outro). Titles take color \"brand1\"..\"brand4\", "
             "font \"brand1\"/\"brand2\".",
             {{"project"}, {"action", "", {"show", "set", "apply", "unset", "logo", "intro_outro"}}, {"logo"},
              {"colors", "comma list"}, {"fonts", "heading, body"}},
             {"global", "intro", "outro", "logo_corner", "logo_size", "timeline"}},
            {"render",
             "Render a timeline to a video, GIF, image sequence or audio file; out's extension must fit the format. "
             "Subtitles are burned in only with subtitles:'burn'. GIF: e.g. height 480, fps 15.",
             {{"project"}, {"out"}, {"preset", "e.g. youtube-1080p, tiktok"},
              {"format", "",
               {"h264", "mp4", "webm", "h265", "av1", "vp9", "s264", "prores", "prores422", "proreslt", "proresproxy", "prores4444",
                "dnxhr-sq", "dnxhr-hq", "dnxhr-hqx", "gif", "png", "tiff", "jpeg", "aac", "mp3", "opus", "wav", "aiff",
                "flac", "alac", "srt", "vtt"}},
              {"quality", "", {"high", "medium", "small", "web", "source", "0", "1", "2", "3", "4"}},
              {"max_mb", "file size limit"},
              {"height"},
              {"fps"},
              {"subtitles", "", {"none", "burn", "srt", "vtt"}},
              {"from"},
              {"to"},
              {"timeline"}},
             {"overwrite", "trust_custom_code", "size", "encoder", "speed", "audio_bitrate", "channels", "audio_codec",
              "embed_subtitles", "mute_tracks", "threads", "allow_missing", "review", "review_hits", "beats_track"}},
            {"short",
             "A finished vertical short of recordings in one call: moments, reframe, captions, check, render, contact "
             "sheet.",
             {{"files"}, {"project"}, {"out"}, {"for", "", {"shorts", "reels", "tiktok"}}, {"min_s"}, {"max_s"},
              {"captions", "", {"karaoke", "word", "off"}}, {"accents", "", {"punch", "off"}},
              {"game", "", {"cs2", "valorant", "none"}}},
             {"timeline", "aspect", "reframe", "hook", "sfx", "audio_stream", "sheet", "language", "encoder",
              "threads", "overwrite"}},
            {"direct",
             "Suggests what to cut with reasons, then applies the chosen as one edit. Suggest: recipe + files (or "
             "project + timeline); apply: session + apply.",
             {{"recipe", "", {"short", "highlights", "talk", "template", "script"}}, {"files"}, {"project"},
              {"timeline"}, {"options", "the recipe's settings (help)"}, {"session", "id from the suggest"},
              {"apply", "{on:'all'|[n…], adjust, order}"}},
             {"plan", "dry_run", "project_dir", "finish"}},
            {"probe",
             "Media file details: length, size, frame rate, audio streams. gaps: stutter of game recordings.",
             {{"file"}, {"files"}, {"gaps"}},
             {"project", "bars"}},
            {"silence",
             "Pauses in a media file or a project's sound: 'silence' and 'sound' ranges. File: 'sound' fits op keep; "
             "project: op cut_pauses does it.",
             {{"file", "media file or project"}, {"project", "frames in its rate"}, {"threshold_db"}, {"min"}},
             {"pad", "timeline", "tracks", "from", "to"}},
            {"overview",
             "Cheap first look at a file or project: per stretch loudness, silence, motion, cuts (or clips). Read it "
             "first.",
             {{"file"}, {"files"}},
             {"window", "timeline", "audio_stream", "project", "numbers"}},
            {"scenes",
             "Scene changes of a video in source frames. region + color: events in one part of the picture (kill feed, "
             "HUD).",
             {{"file"}, {"files"}, {"region", "x,y,w,h"}, {"color"}, {"project", "frames in its rate"}},
             {"threshold", "min", "tolerance", "only", "min_area"}},
            {"peaks",
             "Loud moments (shots, shouts, applause); events:true says what is heard (speech, music, laughter, "
             "cheering …).",
             {{"file"}, {"files"}, {"top"}, {"events"}, {"project", "frames in its rate"}},
             {"threshold_db", "min", "group", "audio_stream", "event_kinds", "min_conf"}},
            {"beats",
             "A song's bpm, beats, bar ones, phrases, breaks and drops: cut on beats, strong moments on drops.",
             {{"file", "song or project"}, {"beats", "list every beat"}},
             {"apply", "project", "timeline", "tracks", "from", "to", "audio_stream", "min_bpm", "max_bpm", "markers"}},
            {"index",
             "Material index: every shot with quality, speech, sounds, descriptions (cached). query finds shots; "
             "annotate describes them.",
             {{"file"}, {"files"}, {"project"}, {"query"}, {"annotate", "[{file, shot, desc}]"}},
             {"timeline", "top", "refresh", "thumbs", "sheet"}},
            {"highlights",
             "Highlight candidates, best first: kill feed kills (cs2, valorant) and loud moments as source ranges. "
             "'keep' fits op keep; hit_s = the hit (accents).",
             {{"file"}, {"files"}, {"game", "", {"cs2", "valorant", "none"}}, {"top"},
              {"project", "frames in its rate"}},
             {"pre", "post", "region", "color", "tolerance", "min_area", "join", "threshold_db", "audio_stream"}},
            {"transcribe",
             "Speech to text (offline; install with `extensions`): segments with times. A project with apply: subtitles "
             "or captions on the timeline.",
             {{"source"}, {"language", "default: detect"}, {"words", "every word's time"}, {"srt", "SRT file to write"},
              {"apply"}, {"captions", "", {"karaoke", "word", "beat"}}},
             {"project", "quality", "length", "model", "music", "max_chars", "audio_stream", "from", "to", "timeline",
              "tracks", "name", "readable", "max_words", "beats", "bpm", "first_beat", "threads", "verbatim"}},
            {"loudness",
             "Loudness (LUFS, true peak) of a file, a project's mix or clips. Web -14 LUFS, broadcast -23.",
             {{"source"}, {"clips", "measure each"}},
             {"tracks", "timeline", "from", "to", "channels", "audio_stream"}},
            {"check",
             "What goes wrong before rendering or handing over: media, gaps, sound, subtitles, phone UI, pace, hook. "
             "Apply its 'fix' ops with `edit`.",
             {{"source", "project or rendered file"},
              {"for", "", {"shorts", "reels", "tiktok", "podcast", "youtube", "web", "broadcast"}},
              {"timeline"}},
             {"target_lufs", "from", "to", "channels", "sound", "max_still_s", "retention"}},
            {"scopes",
             "Waveform, parade, vectorscope, histogram of one frame, with exposure and color cast numbers.",
             {{"source"}, {"at"}},
             {"types", "out", "timeline", "project"}},
            {"extensions",
             "Optional downloads: whisper, x264, piper voices, faces, cutout. Ask the user before installing.",
             {{"install", "ids"}, {"remove"}},
             {"reinstall"}},
            {"effects",
             "Ids and parameters for edit ops: effects, LUTs, transitions, presets, accents, sounds; kind music/nodes/"
             "recipes.",
             {{"effect", "one effect's parameters"}, {"kind", "", {"sfx", "music", "nodes", "recipes"}}},
             {"category", "favorites", "export", "to", "import", "remove", "replace"}},
            {"sync",
             "Offsets of recordings of one event by their sound (first file = reference).",
             {{"files"}},
             {"stream", "max_offset_s", "project"}},
            {"frames",
             "Still images or one contact sheet of media or a timeline: look at material and results. crop = a detail.",
             {{"source"}, {"sources"}, {"at"}, {"count"}, {"sheet"}, {"crop", "x,y,w,h"}},
             {"every", "from", "to", "out", "columns", "tile_width", "width", "project", "timeline", "still", "clip",
              "node", "view", "guides"}},
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
    for (const Cli::Param& p : cmd.params) {
        if (!t->params.contains(p.name)) continue;
        QJsonObject o = props[p.name].toObject();
        const ToolText::Param& brief = t->params[p.name];
        if (p.name == "ops" && cmd.name == "edit") o["description"] = editOpsBrief();
        else if (brief.text.isEmpty()) o.remove("description");
        else o["description"] = brief.text;
        // The types stay those of the full schema: a validating client must not refuse what the server accepts and
        // hands out itself (direct's session id 'd3-7f2a', frames from `info`); test_mcp_tool_size checks it
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
        {"text", {"title", "subtitle", "subtitles", "add_subtitle_track", "captions", "shape", "solid",
                               "clip_shape", "draw"}},
        {"comp", {"comp", "comp_from", "node", "track_node", "pick_region", "cutout_node"}},
        {"multicam", {"multicam", "angle", "angle_cuts", "flatten", "multicam_nudge", "add_angle", "angle_name",
                      "source_marker"}},
        {"pool", {"media", "bin", "remove_media"}},
        {"slots", {"slot", "fill"}},
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
        if (!seen.contains(d.name) && !d.usage.isEmpty()) other << d.name; // without usage: told by another op's
    if (!other.isEmpty()) grouped << "other: " + other.join(", ");
    return "[{op, …keys}] in order. " + grouped.join("; ")
           + ". Times: \"4.5s\", \"MM:SS\", timecode or frames; ranges [from, to). Tracks \"V1\", \"A2\"; clip ids "
             "from `info` or the last answer. Keys: help {command:'edit', op:'name'} first.";
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
        // a name inside the other one counts as half its extra letters ("add_title" -> "title" before "add_angle")
        if (near || inside) close.push_back({inside ? std::min(d, int(std::abs(a.size() - b.size()) / 2)) : d, c});
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
