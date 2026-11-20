// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli `direct`: a recipe (short, highlights; talk, template and script come next) looks at the material and
// suggests moments with reasons (core/Direction.h) without changing anything; applying a session (all, or the ones
// switched on, edges moved, another order) builds the ops and runs them as ONE `edit` (a backup on a file, one undo
// step in the app; plan:true makes a normal plan instead). Sessions are kept like plans as d<N>.json in the plan
// folder for a day. The moments come from cli/DirectMoments.cpp, the picking from core/ShortPick and core/ReelPick.
#include "cli/AnalysisDetail.h"
#include "cli/DirectDetail.h"
#include "cli/Live.h"

#include "core/GameHud.h"
#include "core/I18n.h"
#include "core/LiveHost.h"
#include "core/ProjectFile.h"
#include "core/ReelPick.h"
#include "core/RenderJob.h"
#include "core/ShortPick.h"
#include "core/StreamPresets.h"
#include "engine/Extensions.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace Cli::detail {

namespace {

constexpr int kSessionHours = 24;

double secondsOf(double v) { return std::round(v * 100) / 100; }

QString uniqueName(const QStringList& taken, const QString& wanted)
{
    const auto has = [&](const QString& n) {
        return std::any_of(taken.begin(), taken.end(), [&](const QString& t) { return t.compare(n, Qt::CaseInsensitive) == 0; });
    };
    QString name = wanted;
    for (int i = 2; has(name); ++i) name = QString("%1 %2").arg(wanted).arg(i);
    return name;
}

QString freeFile(const QString& dir, const QString& base, const QString& suffix)
{
    QString path = QDir(dir).filePath(base + suffix);
    for (int i = 2; QFileInfo::exists(path); ++i) path = QDir(dir).filePath(QString("%1-%2%3").arg(base).arg(i).arg(suffix));
    return path;
}

Direction::Note note(const QString& key, const QJsonObject& values = {}) { return Direction::makeNote(key, values); }

// The game whose kill feed gives the moments: given (id or "none"), else from the first file's folder or name
QString gameOf(const QJsonObject& options, const QStringList& files)
{
    if (options.contains("game")) {
        const QString g = options["game"].toString().trimmed();
        if (g.compare("none", Qt::CaseInsensitive) == 0) return "none";
        if (const GameHud::Game* game = GameHud::find(g)) return game->id;
        QStringList ids{"none"};
        for (const GameHud::Game& h : GameHud::games()) ids << h.id;
        fail("BAD_ARGUMENT", "game must be " + ids.join(", ") + " (or leave it out: from the folder or file name)");
    }
    if (!files.isEmpty())
        if (const GameHud::Game* game = GameHud::guess(files.first())) return game->id;
    return "none";
}

// The recordings of a timeline (its media clips with a picture, else with sound), in their order on it
QStringList timelineFiles(const QString& project, const QString& timeline)
{
    Session s(project);
    if (timeline != QLatin1String("@current")) s.selectTimeline(timeline);
    QStringList files;
    const Timeline& tl = s.project.timeline();
    for (const QVector<Track>* tracks : {&tl.video, &tl.audio}) {
        QVector<const Clip*> clips;
        for (const Track& t : *tracks)
            for (const Clip& c : t.clips)
                if (c.kind == ClipKind::Media && !c.mediaPath.isEmpty()) clips << &c;
        std::stable_sort(clips.begin(), clips.end(), [](const Clip* a, const Clip* b) { return a->start < b->start; });
        for (const Clip* c : clips)
            if (!files.contains(c->mediaPath)) files << c->mediaPath;
        if (!files.isEmpty()) break;
    }
    return files;
}

// Picked moments as suggestions: the reason of the best candidate inside each (labels of all of them)
Direction::Suggestion suggestionOf(const ShortPick::Moment& m, const QVector<DirectMoment>& from,
                                   const QStringList& files)
{
    Direction::Suggestion s;
    s.file = files.value(m.file);
    s.from = m.from;
    s.to = m.to;
    s.score = m.score;
    const DirectMoment* best = nullptr;
    for (const DirectMoment& c : from) {
        if (c.file != m.file || c.to <= m.from || c.from >= m.to) continue;
        if (!best || c.score > best->score) best = &c;
        for (const QString& l : c.labels)
            if (!s.labels.contains(l)) s.labels << l;
    }
    if (best) {
        s.reason = best->reason;
        if (s.score <= 0) s.score = best->score;
    } else {
        s.reason = {"fallback_end", {}};
    }
    return s;
}

QVector<ShortPick::Moment> asMoments(const QVector<DirectMoment>& list)
{
    QVector<ShortPick::Moment> out;
    for (const DirectMoment& m : list) out << ShortPick::Moment{m.file, m.from, m.to, m.score};
    return out;
}

bool overlapsAny(const DirectMoment& c, const QVector<Direction::Suggestion>& list, const QStringList& files)
{
    return std::any_of(list.begin(), list.end(), [&](const Direction::Suggestion& s) {
        return s.file == files.value(c.file) && c.from < s.to && s.from < c.to;
    });
}

// The best moment first (hook), then the others by file and time
QVector<int> hookOrder(const QVector<Direction::Suggestion>& list, const QVector<int>& ns, bool hookBest,
                       const QStringList& files)
{
    QVector<const Direction::Suggestion*> chosen;
    for (const Direction::Suggestion& s : list)
        if (ns.contains(s.n)) chosen << &s;
    const auto fileIndex = [&files](const QString& f) { return int(files.indexOf(f)); };
    std::stable_sort(chosen.begin(), chosen.end(), [&](const Direction::Suggestion* a, const Direction::Suggestion* b) {
        const int fa = fileIndex(a->file), fb = fileIndex(b->file);
        return fa != fb ? fa < fb : a->from < b->from;
    });
    int best = -1;
    if (hookBest && chosen.size() > 1)
        for (int k = 0; k < chosen.size(); ++k)
            if (best < 0 || chosen[k]->score > chosen[best]->score) best = k;
    QVector<int> out;
    if (best >= 0) out << chosen[best]->n;
    for (int k = 0; k < chosen.size(); ++k)
        if (k != best) out << chosen[k]->n;
    return out;
}

// ---------- Recipes ----------

void suggestShort(Direction::Session& s, const CollectedMoments& found, const QStringList& files)
{
    const QJsonObject& o = s.options;
    ShortPick::Options pick;
    if (o.contains("min_s")) pick.minS = std::clamp(o["min_s"].toDouble(), 1.0, 600.0);
    if (o.contains("max_s")) pick.maxS = std::clamp(o["max_s"].toDouble(), 1.0, 600.0);
    if (pick.maxS < pick.minS) fail("BAD_ARGUMENT", "max_s must not be below min_s");
    const ShortPlatform* platform = shortPlatform(o.contains("for") ? o["for"].toString() : QString("shorts"));
    if (!platform) fail("BAD_ARGUMENT", "for must be shorts, reels or tiktok (the platform)");
    if (platform->maxSeconds < pick.maxS) pick.maxS = platform->maxSeconds;
    pick.minS = std::min(pick.minS, pick.maxS);
    const int variants = std::clamp(o.contains("variants") ? o["variants"].toInt() : 1, 1, 3);
    const bool hookBest = o["hook"].toString("best").trimmed().toLower() != QLatin1String("chronological");

    QVector<double> durations;
    for (const Direction::Material& m : s.material) durations << m.length;
    // The highlights first (as `short` always took them); without any, the sound events and picture changes
    QVector<DirectMoment> source = found.highlights;
    std::stable_sort(source.begin(), source.end(),
                     [](const DirectMoment& x, const DirectMoment& y) { return x.score > y.score; });
    if (source.isEmpty() && !found.extra.isEmpty()) {
        source = found.extra;
        s.notes << note("fallback");
    }
    const QVector<QVector<ShortPick::Moment>> picked = ShortPick::variants(asMoments(source), durations, variants, pick);
    if (source.isEmpty()) s.notes << note("no_moments");
    else if (picked.size() < variants) s.notes << note("fewer_variants", {{"n", int(picked.size())}, {"asked", variants}});
    int n = 0;
    for (int v = 0; v < picked.size(); ++v)
        for (const ShortPick::Moment& m : picked[v]) {
            Direction::Suggestion x = suggestionOf(m, source, files);
            x.n = ++n;
            x.variant = v + 1;
            x.on = v == 0;
            s.suggestions << x;
        }
    // The rest of what was found, switched off (to swap in on the page), at most 12
    QVector<DirectMoment> rest = found.highlights + found.extra;
    std::stable_sort(rest.begin(), rest.end(), [](const DirectMoment& x, const DirectMoment& y) { return x.score > y.score; });
    int added = 0;
    QVector<Direction::Suggestion> taken = s.suggestions;
    for (const DirectMoment& c : rest) {
        if (added >= 12) break;
        if (overlapsAny(c, taken, files)) continue;
        ShortPick::Moment m{c.file, c.from, c.to, c.score};
        if (m.to - m.from < pick.minMomentS) ShortPick::widen(m, std::min(pick.minMomentS, durations.value(c.file)) - (m.to - m.from), durations.value(c.file));
        Direction::Suggestion x = suggestionOf(m, {c}, files);
        x.n = ++n;
        x.on = false;
        s.suggestions << x;
        taken << x;
        ++added;
    }
    // What else is heard or seen in a moment shows in its labels (a loud moment that is laughter)
    for (Direction::Suggestion& x : s.suggestions)
        for (const DirectMoment& e : found.extra)
            if (files.value(e.file) == x.file && e.from < x.to && x.from < e.to)
                for (const QString& l : e.labels)
                    if (!x.labels.contains(l)) x.labels << l;
    QVector<int> first;
    for (const Direction::Suggestion& x : s.suggestions)
        if (x.variant == 1) first << x.n;
    s.order = hookOrder(s.suggestions, first, hookBest, files);
    s.target = pick.maxS;
    // Captions asked for (the Direct page's card) without whisper: said now, not only by a missing finish button
    const QString captions = o["captions"].toString().trimmed().toLower();
    if (!captions.isEmpty() && captions != QLatin1String("off")
        && (Extensions::whisperProgram().isEmpty() || Extensions::installedWhisperModels().isEmpty()))
        s.notes << note("no_whisper");
}

void suggestHighlights(Direction::Session& s, const CollectedMoments& found, const QStringList& files)
{
    const QJsonObject& o = s.options;
    ReelPick::Options pick;
    const QJsonValue length = o["length_s"];
    if (length.isString() && length.toString().trimmed().toLower() == QLatin1String("all")) pick.targetS = 0;
    else if (length.isDouble()) pick.targetS = std::clamp(length.toDouble(), 5.0, 3600.0);
    else if (!length.isUndefined() && !length.isNull()) fail("BAD_ARGUMENT", "length_s is seconds or 'all'");
    const QString order = o["order"].toString("best").trimmed().toLower();
    if (order != QLatin1String("best") && order != QLatin1String("time")) fail("BAD_ARGUMENT", "order must be best or time");
    pick.order = order == QLatin1String("time") ? ReelPick::Order::Time : ReelPick::Order::Best;
    const QString transitions = o["transitions"].toString("cut").trimmed().toLower();
    if (transitions != QLatin1String("cut") && transitions != QLatin1String("soft"))
        fail("BAD_ARGUMENT", "transitions must be cut or soft");
    QVector<double> durations;
    for (const Direction::Material& m : s.material) durations << m.length;
    // Everything that stands out counts here (holiday clips: laughter, cheering, movement, loud)
    QVector<DirectMoment> all = found.highlights + found.extra;
    std::stable_sort(all.begin(), all.end(), [](const DirectMoment& x, const DirectMoment& y) { return x.score > y.score; });
    QVector<ReelPick::Moment> picked = ReelPick::pick(asMoments(all), durations, pick);
    // Fewer moments than the target: ReelPick grew them (more build-up and reaction); the note says from how much
    if (pick.targetS > 0 && !picked.isEmpty()) {
        ReelPick::Options plain = pick;
        plain.growBeforeS = plain.growAfterS = 0;
        double found = 0, grown = 0;
        for (const ReelPick::Moment& m : ReelPick::pick(asMoments(all), durations, plain)) found += m.to - m.from;
        for (const ReelPick::Moment& m : picked) grown += m.to - m.from;
        if (grown > found + 0.5)
            s.notes << note("moments_grown", {{"found", std::round(found * 10) / 10},
                                              {"s", std::round(grown * 10) / 10},
                                              {"target", pick.targetS}});
    }
    if (picked.isEmpty()) {
        // Nothing stood out: the end of every recording (a clip tool saves the moment that just happened)
        ShortPick::Options ends; // as long as asked for ('all': the last 10 s of each)
        const double want = pick.targetS > 0 ? pick.targetS : 10.0 * durations.size();
        ends.minS = 0;
        ends.maxS = std::max(1.0, want);
        ends.fallbackS = want;
        picked = ShortPick::pick({}, durations, ends);
        s.notes << note("no_moments");
    }
    int n = 0;
    for (const ReelPick::Moment& m : picked) {
        Direction::Suggestion x = suggestionOf(m, all, files);
        x.n = ++n;
        s.suggestions << x;
        s.order << x.n;
    }
    int added = 0;
    QVector<Direction::Suggestion> taken = s.suggestions;
    for (const DirectMoment& c : all) {
        if (added >= 20) break;
        if (overlapsAny(c, taken, files)) continue;
        ShortPick::Moment m{c.file, c.from, c.to, c.score};
        if (m.to - m.from < pick.minMomentS) ShortPick::widen(m, std::min(pick.minMomentS, durations.value(c.file)) - (m.to - m.from), durations.value(c.file));
        Direction::Suggestion x = suggestionOf(m, {c}, files);
        x.n = ++n;
        x.on = false;
        s.suggestions << x;
        taken << x;
        ++added;
    }
    // Far less than wanted stood out: the uncovered end of every recording offered too (switched off, the user fills
    // up); after the other candidates, never in their way
    double pickedS = 0;
    for (const ReelPick::Moment& m : picked) pickedS += m.to - m.from;
    const bool few = !s.suggestions.isEmpty() && pick.targetS > 0 && pickedS < pick.targetS * 0.5
                     && s.suggestions.first().reason.key != QLatin1String("fallback_end");
    if (few) {
        ShortPick::Options ends;
        ends.minS = 0;
        ends.maxS = std::max(1.0, pick.targetS - pickedS);
        ends.fallbackS = ends.maxS;
        int offered = 0;
        for (ShortPick::Moment m : ShortPick::pick({}, durations, ends)) {
            // Only the tail of an end no suggestion (found, picked or offered) covers
            QVector<const Direction::Suggestion*> same;
            for (const Direction::Suggestion& x : s.suggestions)
                if (x.file == files.value(m.file)) same << &x;
            std::sort(same.begin(), same.end(), [](auto* p, auto* q) { return p->from < q->from; });
            for (const Direction::Suggestion* x : same)
                if (x->from < m.to && m.from < x->to) m.from = std::max(m.from, x->to);
            if (m.to - m.from < 1.0) continue;
            Direction::Suggestion x;
            x.file = files.value(m.file);
            x.from = m.from;
            x.to = m.to;
            x.reason = {"fallback_end", {}};
            x.labels << "end";
            x.n = ++n;
            x.on = false;
            s.suggestions << x;
            ++offered;
        }
        if (offered > 0) s.notes << note("few_moments", {{"s", std::round(pickedS * 10) / 10}, {"target", pick.targetS}});
    }
    s.target = pick.targetS;
}

// ---------- Stored sessions ----------

// All sessions in one folder (ids unique across projects: the `next` of an answer works without the project)
QString sessionDir()
{
    return QDir(ProjectFile::backupDir({})).filePath("direct-sessions");
}

void pruneSessions(const QString& dir)
{
    const QDateTime limit = QDateTime::currentDateTimeUtc().addSecs(-kSessionHours * 3600);
    for (const QFileInfo& fi : QDir(dir).entryInfoList({"d*.json"}, QDir::Files))
        if (fi.lastModified().toUTC() < limit) QFile::remove(fi.absoluteFilePath());
}

void store(Direction::Session& s)
{
    const QString dir = sessionDir();
    if (!QDir().mkpath(dir)) fail("SAVE_FAILED", "cannot write the session into " + dir);
    pruneSessions(dir);
    s.id = newPlanId(dir, "d");
    QFile f(QDir(dir).filePath(s.id + ".json"));
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(Direction::toJson(s)).toJson(QJsonDocument::Compact)) < 0)
        fail("SAVE_FAILED", "cannot write the session " + f.fileName());
}

// Files of a session object as the program spells them (Sandbox: a work folder), every suggestion in the material
void checkFiles(Direction::Session& s)
{
    QStringList files;
    for (Direction::Material& m : s.material) {
        m.file = absolute(m.file);
        files << m.file;
    }
    for (Direction::Suggestion& x : s.suggestions) {
        if (x.file.startsWith('@')) continue; // a range of the timeline itself (talk)
        x.file = absolute(x.file);
        if (!files.contains(x.file))
            fail("BAD_ARGUMENT", QString("session: suggestion %1 is of %2, which is not in its material").arg(x.n).arg(x.file));
    }
    if (files.isEmpty() && s.recipe != QLatin1String("talk")) fail("NO_MATERIAL", "session: no material");
}

Direction::Session load(const QJsonValue& v)
{
    std::optional<Direction::Session> s;
    QString error;
    if (v.isObject()) {
        s = Direction::fromJson(v.toObject(), &error);
        if (!s) fail("BAD_ARGUMENT", "session: " + error);
        const QDateTime made = QDateTime::fromString(s->made, Qt::ISODate);
        if (made.isValid() && made.addSecs(kSessionHours * 3600) < QDateTime::currentDateTimeUtc())
            fail("SESSION_EXPIRED", QString("session %1 is older than %2 h: suggest again").arg(s->id).arg(kSessionHours));
        checkFiles(*s);
        return *s;
    }
    const QString id = v.toString().trimmed().toLower();
    static const QRegularExpression valid("^d[1-9][0-9]{0,8}$");
    if (!valid.match(id).hasMatch()) fail("BAD_ARGUMENT", "session: an id like 'd3' (from the answer of direct) or the session itself");
    pruneSessions(sessionDir());
    QFile f(QDir(sessionDir()).filePath(id + ".json"));
    if (f.open(QIODevice::ReadOnly)) {
        s = Direction::fromJson(QJsonDocument::fromJson(f.readAll()).object(), &error);
        if (!s) fail("BAD_ARGUMENT", "session " + id + ": " + error);
        checkFiles(*s);
        return *s;
    }
    fail("SESSION_EXPIRED", QString("no session '%1' (sessions are kept %2 h; suggest again with direct recipe:…)")
                                .arg(id)
                                .arg(kSessionHours));
}

void requireFresh(const Direction::Session& s)
{
    for (const Direction::Material& m : s.material) {
        const QFileInfo fi(m.file);
        if (!fi.exists()) fail("SESSION_STALE", "the material changed since the session was made (gone: " + m.file + "): suggest again");
        if (m.size >= 0 && (fi.size() != m.size || fi.lastModified().toMSecsSinceEpoch() != m.modified))
            fail("SESSION_STALE", "the material changed since the session was made (" + m.file + "): suggest again");
    }
}

// ---------- Apply ----------

// "@app" and the file the app has open are the same project
bool sameProject(const QString& a, const QString& b)
{
    const auto key = [](const QString& p) {
        QString path = p;
        if (Live::isAppProject(p)) {
            if (LiveHost* h = Live::host()) path = h->liveProjectPath();
            else if (const Live::AppCopy* c = Live::appCopy()) path = c->path;
            else return QString(Live::AppProject);
        }
        return path.isEmpty() ? QString(Live::AppProject) : QFileInfo(path).absoluteFilePath();
    };
    return key(a) == key(b);
}

// Sound of the recordings: audio_stream 'game' (default: only the game streams of game material with several
// streams), 'all', or numbers/names - as `short` always chose (per file: undefined = all)
QVector<QJsonValue> soundStreams(const Direction::Session& s, const QString& game, QStringList* said)
{
    const QString choice = s.options.contains("audio_stream") ? s.options["audio_stream"].toVariant().toString().trimmed()
                                                              : QString("game");
    QVector<QJsonValue> out(s.material.size());
    for (int i = 0; i < s.material.size(); ++i) {
        const QStringList& names = s.material[i].audioStreams;
        if (choice.compare("all", Qt::CaseInsensitive) == 0 || names.size() < 2) continue;
        if (choice.compare("game", Qt::CaseInsensitive) != 0) {
            out[i] = choice;
            continue;
        }
        const bool mixAndChat = std::any_of(names.begin(), names.end(), StreamPresets::isMixTitle)
                             && std::any_of(names.begin(), names.end(), StreamPresets::isChatOrMusicTitle);
        if (!s.options.contains("audio_stream") && game == QLatin1String("none") && !mixAndChat) continue;
        const QVector<int> g = StreamPresets::gameStreams(names);
        if (g.isEmpty()) continue;
        QJsonArray list;
        QStringList kept;
        for (int k : g) list << k, kept << names[k];
        out[i] = list;
        *said << QString("%1: %2").arg(QFileInfo(s.material[i].file).fileName(), kept.join(", "));
    }
    return out;
}

} // namespace

Direction::Session directSuggest(const QJsonObject& a, Context& ctx, CollectedMoments* foundOut)
{
    Direction::Session s;
    s.recipe = a["recipe"].toString().trimmed().toLower();
    if (s.recipe.isEmpty()) fail("BAD_ARGUMENT", "give recipe (short, highlights, talk, template or script) to suggest, or session + apply");
    static const QStringList recipes{"short", "highlights", "talk", "template", "script"};
    if (!recipes.contains(s.recipe))
        fail("UNKNOWN_RECIPE", QString("no recipe '%1' (%2)").arg(s.recipe, recipes.join(", ")));
    s.options = a["options"].toObject();
    // `short`'s own settings given at the top level count as options too (one call from the command line)
    for (const char* k : {"game", "hook", "audio_stream"})
        if (a.contains(k) && !s.options.contains(k)) s.options[k] = a[k];
    s.project = a["project"].toString();
    if (!s.project.isEmpty() && !Live::isAppProject(s.project)) s.project = absolute(s.project);
    s.made = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    QStringList files;
    for (const QJsonValue& v : a["files"].toArray()) {
        const QString f = absolute(v.toString());
        if (!QFileInfo(f).isFile()) fail("NOT_FOUND", "no such file: " + f);
        if (isProjectFile(f)) fail("BAD_ARGUMENT", "files are recordings; a project goes into 'project'");
        if (!files.contains(f)) files << f;
    }
    // talk works on a timeline, template and script on files or the project's media
    if (s.recipe == QLatin1String("talk")) {
        Context sub;
        sub.cancel = ctx.cancel;
        sub.progress = ctx.progress;
        suggestTalk(s, a, sub);
        failIfCanceled();
        return s;
    }
    if (s.recipe == QLatin1String("template") || s.recipe == QLatin1String("script")) {
        Context sub;
        sub.cancel = ctx.cancel;
        sub.progress = ctx.progress;
        if (s.recipe == QLatin1String("template")) suggestTemplate(s, files, sub);
        else suggestScript(s, files, sub);
        failIfCanceled();
        if (s.suggestions.isEmpty() && s.notes.isEmpty()) s.notes << note("no_moments");
        return s;
    }
    if (files.isEmpty() && a.contains("timeline")) {
        // The Direct page: a timeline of the project open in the app (a background job reads its copy)
        if (s.project.isEmpty() && (Live::host() || Live::appCopy())) s.project = Live::AppProject;
        if (s.project.isEmpty()) fail("BAD_ARGUMENT", "timeline needs the project it is in");
        s.timeline = a["timeline"].toString().trimmed();
        files = timelineFiles(s.project, s.timeline);
    }
    if (files.isEmpty()) fail("NO_MATERIAL", "no material: give files (recordings) or a project's timeline");

    // Settings checked now, not only when applying
    const QString aspect = s.options["aspect"].toString().trimmed();
    const QStringList aspects = s.recipe == QLatin1String("short") ? QStringList{"9:16", "1:1", "4:5"}
                                                                   : QStringList{"16:9", "9:16", "1:1", "4:5"};
    if (!aspect.isEmpty() && !aspects.contains(aspect)) fail("BAD_ARGUMENT", "aspect must be " + aspects.join(", "));
    const QString reframe = s.options["reframe"].toString().trimmed().toLower();
    if (!reframe.isEmpty() && !QStringList{"auto", "center", "fit"}.contains(reframe))
        fail("BAD_ARGUMENT", "reframe must be auto, center or fit");
    const QString game = gameOf(s.options, files);
    Context sub;
    sub.cancel = ctx.cancel;
    sub.progress = ctx.progress;
    // Frames of the highlights count in the project's rate (a file's own rate without one, or for the app's project)
    const QString frameProject = Live::isAppProject(s.project) ? QString() : s.project;
    const CollectedMoments found = collectMoments(files, QFileInfo::exists(frameProject) ? frameProject : QString(), game,
                                                  s.recipe != QLatin1String("short"), sub);
    failIfCanceled();
    s.material = found.material;
    if (s.recipe == QLatin1String("short")) suggestShort(s, found, files);
    else suggestHighlights(s, found, files);
    if (foundOut) *foundOut = found;
    return s;
}

namespace {
QJsonObject applyOnce(const Direction::Session& s, const QJsonObject& apply, const QJsonObject& args, Context& ctx,
                      QString* madeProject);
} // namespace

QJsonObject directApply(const Direction::Session& s, const QJsonObject& apply, const QJsonObject& args, Context& ctx)
{
    // A project this apply made itself goes again when it fails before its edit is done (canceled, an op refused): no
    // half project, and no backups or plans of it left; once the edit is saved it stays whatever comes after
    QString made;
    try {
        return applyOnce(s, apply, args, ctx, &made);
    } catch (...) {
        if (!made.isEmpty()) {
            QFile::remove(made);
            QDir(ProjectFile::backupDir(made)).removeRecursively();
        }
        throw;
    }
}

namespace {
QJsonObject applyOnce(const Direction::Session& s, const QJsonObject& apply, const QJsonObject& args, Context& ctx,
                      QString* madeProject)
{
    const auto run = [&ctx](const char* command, const QJsonObject& a) {
        Context sub;
        sub.cancel = ctx.cancel;
        return execute(*find(command), a, sub);
    };
    requireFresh(s);
    Direction::Session chosen = s;
    QStringList files;
    for (const Direction::Material& m : s.material) files << m.file;
    // Which suggestions: on:'all' = the variant's (default: those switched on), on:[n…] = exactly these
    const int variant = apply.contains("variant") ? apply["variant"].toInt() : 0;
    if (apply.contains("variant")) {
        int most = 0;
        for (const Direction::Suggestion& x : s.suggestions) most = std::max(most, x.variant);
        if (!apply["variant"].isDouble() || variant < 1 || variant > most)
            fail("BAD_ARGUMENT", most == 0 ? QString("variant: session %1 has no variants (recipe short has them)").arg(s.id)
                                           : QString("variant: session %1 has variants 1 to %2").arg(s.id).arg(most));
    }
    const QJsonValue on = apply.contains("on") ? apply["on"] : QJsonValue("all");
    const bool all = on.isString() && on.toString().trimmed().toLower() == QLatin1String("all");
    QVector<int> ons;
    if (all) {
        for (const Direction::Suggestion& x : s.suggestions)
            if (variant > 0 ? x.variant == variant : x.on) ons << x.n;
    } else if (on.isArray()) {
        for (const QJsonValue& v : on.toArray()) {
            if (!v.isDouble() || !s.find(v.toInt())) fail("BAD_ARGUMENT", QString("on: session %1 has no suggestion %2").arg(s.id).arg(v.toInt()));
            ons << v.toInt();
        }
    } else {
        fail("BAD_ARGUMENT", "apply.on is 'all' or a list of suggestion numbers");
    }
    for (Direction::Suggestion& x : chosen.suggestions) x.on = ons.contains(x.n);
    for (const QJsonValue& v : apply["adjust"].toArray()) {
        Direction::Suggestion* x = nullptr;
        for (Direction::Suggestion& y : chosen.suggestions)
            if (y.n == v["n"].toInt()) x = &y;
        if (!x) fail("BAD_ARGUMENT", QString("adjust: no suggestion %1").arg(v["n"].toInt()));
        for (const char* k : {"from_s", "to_s"})
            if (v.toObject().contains(k) && !v[k].isDouble())
                fail("BAD_ARGUMENT", QString("adjust: %1 of suggestion %2 is seconds (a number)").arg(k).arg(x->n));
        // Inside the recording (or the timeline of talk); a length truly unknown keeps only from < to
        const double length = x->file == QLatin1String("@timeline") ? s.options["_timeline_length_s"].toDouble()
                                                                     : s.material.value(int(files.indexOf(x->file))).length;
        const double from = std::clamp(v["from_s"].toDouble(x->from), 0.0, length > 0 ? length : 1e9);
        const double to = std::clamp(v["to_s"].toDouble(x->to), 0.0, length > 0 ? length : 1e9);
        if (to - from < 0.1) fail("BAD_ARGUMENT", QString("adjust: suggestion %1 would be empty").arg(x->n));
        x->from = from;
        x->to = to;
    }
    const bool hookBest = s.options["hook"].toString("best").trimmed().toLower() != QLatin1String("chronological");
    if (apply.contains("order")) {
        chosen.order.clear();
        for (const QJsonValue& v : apply["order"].toArray()) {
            const Direction::Suggestion* x = v.isDouble() ? chosen.find(v.toInt()) : nullptr;
            if (!x || !x->on)
                fail("BAD_ARGUMENT", QString("order: %1 is no switched-on suggestion of session %2 (switched on: %3)")
                                         .arg(v.toVariant().toString(), s.id)
                                         .arg([&] {
                                             QStringList l;
                                             for (int n : ons) l << QString::number(n);
                                             return l.join(", ");
                                         }()));
            chosen.order << v.toInt();
        }
    } else if (variant > 1 || !all || !s.find(s.order.value(0)) || !ons.contains(s.order.value(0))) {
        // The session's order was made for its first variant: another choice gets its own (hook first)
        chosen.order = s.recipe == QLatin1String("short") ? hookOrder(chosen.suggestions, ons, hookBest, files) : s.order;
    }
    const QVector<int> order = Direction::resolvedOrder(chosen);
    if (order.isEmpty()) fail("BAD_ARGUMENT", "nothing switched on: apply.on names no suggestion");
    // Overlapping ranges of one recording (edges moved into each other) become one, at the place of the first
    for (int n : order) {
        Direction::Suggestion* a = nullptr;
        for (Direction::Suggestion& y : chosen.suggestions)
            if (y.n == n) a = &y;
        if (!a->on || a->kind == QLatin1String("slot") || a->kind == QLatin1String("pause") || a->kind == QLatin1String("filler"))
            continue;
        for (Direction::Suggestion& b : chosen.suggestions)
            if (&b != a && b.on && b.file == a->file && b.from < a->to && a->from < b.to) {
                a->from = std::min(a->from, b.from);
                a->to = std::max(a->to, b.to);
                b.on = false;
            }
    }
    QVector<const Direction::Suggestion*> list;
    for (int n : order)
        if (const Direction::Suggestion* x = chosen.find(n); x->on) list << x;
    chosen.order.clear();
    for (const Direction::Suggestion* x : list) chosen.order << x->n;
    // The platform's longest length, after the choice and the moved edges
    if (s.recipe == QLatin1String("short")) {
        double total = 0;
        for (const Direction::Suggestion* x : list) total += x->to - x->from;
        const ShortPlatform* platform = shortPlatform(s.options["for"].toString("shorts"));
        if (platform && total > platform->maxSeconds + 0.5)
            fail("BAD_ARGUMENT", QString("the short would be %1 s, longer than %2 allows (%3 s): switch some off or "
                                         "move their edges")
                                     .arg(secondsOf(total))
                                     .arg(QString::fromLatin1(platform->name))
                                     .arg(platform->maxSeconds));
    }

    if (s.recipe == QLatin1String("talk")) {
        QJsonObject o = applyTalk(chosen, list, args, ctx);
        madeProject->clear();
        return o;
    }
    if (s.recipe == QLatin1String("template")) return applyTemplate(chosen, list, args, ctx);
    const QString game = gameOf(s.options, files);
    QJsonArray done, notes;
    for (const Direction::Note& n : s.notes) notes << n.text;
    // The project: given, the session's, else a new one next to the first recording
    QString project = args["project"].toString();
    if (!project.isEmpty() && !Live::isAppProject(project)) project = absolute(project);
    // A session made for one project is applied to that one (its timeline material, its frame rate)
    if (!project.isEmpty() && !s.project.isEmpty() && !sameProject(project, s.project))
        fail("SESSION_PROJECT_MISMATCH", QString("session %1 was made for %2, not %3: apply it there or suggest again")
                                             .arg(s.id, s.project, project));
    if (project.isEmpty()) project = s.project;
    // Run in the app for the user (the Direct page, Live::runForUser): its project, one undo step around it all
    if (project.isEmpty() && Live::host()) project = Live::AppProject;
    if (!project.isEmpty() && !Live::isAppProject(project)) project = absolute(project);
    const bool dry = args["dry_run"].toBool() || args["plan"].toBool();
    bool created = args["_created"].toBool();
    const bool missing = !project.isEmpty() && !Live::isAppProject(project) && !QFileInfo::exists(project);
    if (project.isEmpty() || missing) {
        if (project.isEmpty()) {
            const QFileInfo first(files.first());
            // In project_dir when given, else next to the first recording
            QString dir = args["project_dir"].toString().trimmed();
            dir = dir.isEmpty() ? first.absolutePath() : absolute(dir);
            if (!dry && !QDir().mkpath(dir)) fail("SAVE_FAILED", "cannot make the folder " + dir);
            project = freeFile(dir, first.completeBaseName() + "-" + s.recipe, "." + QString(ProjectFile::Extension));
        } else if (QFileInfo(project).suffix().isEmpty()) {
            project += "." + QString(ProjectFile::Extension);
        }
        if (dry) fail("BAD_ARGUMENT", "dry_run and plan need an existing project (give 'project')");
        const bool streams = std::any_of(s.material.begin(), s.material.end(), [](const Direction::Material& m) { return m.audioStreams.size() > 1; });
        QJsonObject n{{"project", project}, {"media", QJsonArray::fromStringList(files)},
                      {"template", game != QLatin1String("none") || streams ? "game" : "neutral"}};
        if (s.material.first().fps > 0) n["fps"] = s.material.first().fps;
        failIfCanceled();
        run("new", n);
        *madeProject = project;
        created = true;
        done << QString("new project %1 (template %2)").arg(project, n["template"].toString());
    }
    QStringList soundSaid;
    const QVector<QJsonValue> soundOf = soundStreams(s, game, &soundSaid);
    if (!soundSaid.isEmpty())
        done << QString("sound: only the game streams (%1); audio_stream:'all' keeps voice chat, microphone and music too")
                    .arg(soundSaid.join("; "));

    const QJsonObject before = run("info", {{"project", project}, {"summary", true}});
    QStringList names;
    for (const QJsonValue& v : before["timelines"].toArray()) names << v["name"].toString();
    const bool isShort = s.recipe == QLatin1String("short");
    const bool isScript = s.recipe == QLatin1String("script");
    const QString cut = uniqueName(names, isShort ? T("Short-Schnitt") : isScript ? T("Rohschnitt") : T("Highlights"));
    names << cut;
    QJsonArray ops{created ? QJsonObject{{"op", "rename_timeline"}, {"name", cut}}
                           : QJsonObject{{"op", "new_timeline"}, {"name", cut}}};
    // A project of another shape: the cut in the recordings' shape (the reframe of the copy has room to work)
    const Direction::Material& shape = s.material.first();
    const QJsonObject format = before["format"].toObject();
    const int pw = format["width"].toInt(), ph = format["height"].toInt();
    if (shape.width > 0 && shape.height > 0 && pw > 0 && ph > 0
        && std::abs(double(shape.width) / shape.height - double(pw) / ph) > 0.01) {
        const int w = std::max(16, shape.width / 2 * 2), h = std::max(16, shape.height / 2 * 2);
        ops << QJsonObject{{"op", "format"}, {"timeline", cut}, {"width", w}, {"height", h}}
            << QJsonObject{{"op", "timeline"}, {"timeline", cut}};
    }
    const auto range = [](const Direction::Suggestion* m) {
        return QJsonArray{QString("%1s").arg(m->from, 0, 'f', 3), QString("%1s").arg(m->to, 0, 'f', 3)};
    };
    const auto keepOp = [&](const QString& file, const QJsonArray& ranges) {
        QJsonObject k{{"op", "keep"}, {"media", file}, {"ranges", ranges}};
        const QJsonValue& sound = soundOf.value(int(files.indexOf(file)));
        if (sound.isArray() || sound.isString()) k["audio_streams"] = sound;
        return k;
    };
    // One keep per run of the same recording; the short's hook (its first moment) a keep of its own
    QJsonArray moments;
    for (int k = 0; k < list.size();) {
        int j = k + 1;
        if (!(isShort && k == 0 && hookBest && list.size() > 1))
            while (j < list.size() && list[j]->file == list[k]->file) ++j;
        QJsonArray ranges;
        for (int i = k; i < j; ++i) {
            ranges << range(list[i]);
            moments << QJsonObject{{"file", list[i]->file}, {"from_s", secondsOf(list[i]->from)}, {"to_s", secondsOf(list[i]->to)},
                                   {"n", list[i]->n}, {"reason", Direction::reasonText(list[i]->reason)}};
        }
        ops << keepOp(list[k]->file, ranges);
        k = j;
    }
    // Story: the first moment is the hook, every other one a section of its own (each boundary the same time for both)
    double total = 0;
    QVector<double> cuts;
    {
        QString from = "0";
        for (int k = 0; k < list.size(); ++k) {
            total += std::max(0.04, list[k]->to - list[k]->from);
            if (k + 1 < list.size()) cuts << total;
            if (!isShort) continue;
            const QString to = QString("%1s").arg(total, 0, 'f', 3);
            QJsonObject sec{{"op", "section"}, {"kind", k == 0 ? "hook" : "free"}, {"from", from}, {"to", to}};
            if (k > 0)
                sec["name"] = QString("%1 %2 s").arg(QFileInfo(list[k]->file).completeBaseName()).arg(secondsOf(list[k]->from));
            ops << sec;
            from = to;
        }
    }
    QString result = cut;
    if (isShort) {
        // The vertical copy, every clip following its subject; game footage keeps its kill feed readable at the top
        const QString aspect = s.options["aspect"].toString("9:16").trimmed();
        if (!QStringList{"9:16", "1:1", "4:5"}.contains(aspect)) fail("BAD_ARGUMENT", "aspect must be 9:16, 1:1 or 4:5");
        const QString reframe = s.options["reframe"].toString("auto").trimmed().toLower();
        if (!QStringList{"auto", "center", "fit"}.contains(reframe)) fail("BAD_ARGUMENT", "reframe must be auto, center or fit");
        result = uniqueName(names, T("Short"));
        QJsonObject copy{{"op", "new_timeline"}, {"name", result}, {"from", cut}, {"aspect", aspect}, {"reframe", reframe},
                         {"open", false}};
        if (game != QLatin1String("none")) copy["game"] = game;
        else if (s.options["game"].toString().trimmed().compare("none", Qt::CaseInsensitive) == 0) copy["keep"] = "none";
        ops << copy;
    } else if (!isScript) {
        // Highlights: soft cuts, music (a file cut to the length, or the music bed), the cuts on its beat
        if (s.options["transitions"].toString() == QLatin1String("soft"))
            for (double at : cuts)
                ops << QJsonObject{{"op", "transition"}, {"at", QString("%1s").arg(at, 0, 'f', 3)}, {"length", "0.4s"}};
        const QJsonValue music = s.options["music"];
        if (music.isObject() && music["file"].isString()) {
            ops << QJsonObject{{"op", "fit_music"}, {"media", absolute(music["file"].toString())}, {"length", "timeline"}}
                << QJsonObject{{"op", "snap_to_beat"}, {"max_shift", "0.25s"}};
        } else if (music.isObject() && music["mood"].isString()) {
            if (!editOpKeys("music_bed").isEmpty())
                ops << QJsonObject{{"op", "music_bed"}, {"mood", music["mood"]}, {"length", "timeline"}}
                    << QJsonObject{{"op", "snap_to_beat"}, {"max_shift", "0.25s"}};
            else
                notes << Direction::noteText("music_missing");
        }
        const QString aspect = s.options["aspect"].toString().trimmed();
        if (!aspect.isEmpty() && aspect != QLatin1String("16:9")) {
            if (!QStringList{"9:16", "1:1", "4:5"}.contains(aspect)) fail("BAD_ARGUMENT", "aspect must be 16:9, 9:16, 1:1 or 4:5");
            result = uniqueName(names, T("Highlights") + " " + aspect);
            QJsonObject copy{{"op", "new_timeline"}, {"name", result}, {"from", cut}, {"aspect", aspect}, {"reframe", "auto"},
                             {"open", false}};
            if (game != QLatin1String("none")) copy["game"] = game;
            ops << copy;
        }
    }
    // The project remembers the session as it was applied (the Direct page shows it again; no undo step)
    // Transcripts whisper made while suggesting (script) go into the project with the cut
    if (s.options["_transcripts"].isObject() && !args["plan"].toBool())
        ops << QJsonObject{{"op", "_transcripts"}, {"files", s.options["_transcripts"]}};
    if (!args["_no_direction"].toBool() && !args["plan"].toBool()) { // a plan lists only what the user chose
        QJsonObject kept = Direction::toJson(chosen);
        ops << QJsonObject{{"op", "_direction"}, {"session", kept}};
    }
    QJsonObject e{{"project", project}, {"ops", ops}};
    if (args["plan"].toBool()) e["plan"] = true;
    if (args["dry_run"].toBool()) e["dry_run"] = true;
    const QJsonObject edited = run("edit", e);
    madeProject->clear(); // the edit is saved: the project stays
    if (args["plan"].toBool()) {
        QJsonObject o{{"project", project}, {"plan", edited["plan"]}, {"session", s.id}};
        return o;
    }
    done << QString("cut: %1 moment%2, %3 s (timeline '%4')")
                .arg(list.size())
                .arg(list.size() == 1 ? "" : "s")
                .arg(secondsOf(total))
                .arg(cut);
    QJsonObject o{{"project", project},
                  {"timeline", result},
                  {"cut_timeline", cut},
                  {"length_s", secondsOf(total)},
                  {"moments", moments},
                  {"done", done},
                  {"session", s.id},
                  {"edit_notes", edited["notes"]}};
    if (edited.contains("timelines")) {
        QJsonArray tl;
        for (const QJsonValue& v : edited["timelines"].toArray()) tl << v["name"];
        o["timelines"] = tl;
    }
    if (args["dry_run"].toBool()) o["dry_run"] = true;
    if (edited.contains("live")) o["live"] = true;
    // In the app (the page's Übernehmen) no check here: it runs on the UI thread, the page checks before exporting
    if (!dry && !args["_no_check"].toBool() && !Live::host()) {
        // What to look at before exporting (nothing fixed here: the export does that with the platform's preset)
        QJsonObject c{{"source", project}, {"timeline", result}};
        if (isShort) {
            const ShortPlatform* platform = shortPlatform(s.options["for"].toString("shorts"));
            if (platform) c["for"] = QString::fromLatin1(platform->alias);
        }
        const QJsonObject checked = run("check", c);
        QJsonObject check{{"issues", checked["issues"]}};
        int errors = 0, warnings = 0;
        for (const QJsonValue& v : checked["issues"].toArray()) {
            errors += v["severity"].toString() == QLatin1String("error");
            warnings += v["severity"].toString() == QLatin1String("warning");
        }
        check["errors"] = errors;
        check["warnings"] = warnings;
        o["check"] = check;
    }
    if (!notes.isEmpty()) o["notes"] = notes;
    o["next"] = isShort ? QString("look at it (`frames`), then `render` timeline '%1' with preset youtube-shorts / "
                                  "instagram-reels / tiktok (captions first: `transcribe` apply:true on it)").arg(result)
                        : QString("look at it (`frames`), then `render` timeline '%1'").arg(result);
    return o;
}
} // namespace

QJsonObject cmdDirect(const QJsonObject& a, Context& ctx)
{
    // After a short's apply: its captions and effect sounds, one more edit (the Direct page's second button). In the
    // app in two steps: prepare (a background job measures speech, peak and kill feed and answers the ops and the
    // timeline's stamp), then ops (the edit alone, refused when the timeline changed meanwhile)
    if (a.contains("finish")) {
        const QJsonObject f = a["finish"].toObject();
        QString project = a["project"].toString();
        if (project.isEmpty() && (Live::host() || Live::appCopy())) project = Live::AppProject;
        if (project.isEmpty()) fail("BAD_ARGUMENT", "finish needs the project of the short");
        const QString timeline = f["timeline"].toString();
        if (timeline.isEmpty()) fail("BAD_ARGUMENT", "finish needs the short's timeline (the answer's timeline)");
        QJsonArray done, notes;
        if (f.contains("ops")) {
            if (timelineStamp(project, timeline) != f["stamp"].toString())
                fail("FINISH_STALE", QString("the timeline '%1' changed since its speech and sound were measured: "
                                             "captions would land on the wrong times - nothing was added").arg(timeline));
            for (const QJsonValue& op : f["ops"].toArray())
                if (op["op"].toString() != QLatin1String("captions") && op["op"].toString() != QLatin1String("sfx"))
                    fail("BAD_ARGUMENT", "finish ops are captions and sfx only");
            const bool captions = shortFinishEdit(project, timeline, f["ops"].toArray(), ctx, &done);
            return {{"project", project}, {"timeline", timeline}, {"captions", captions}, {"done", done}};
        }
        ShortFinish sf;
        sf.captions = f["captions"].toString("karaoke").trimmed().toLower();
        sf.sfx = f["sfx"].toString("off").trimmed().toLower();
        if (!QStringList{"karaoke", "word", "off"}.contains(sf.captions)) fail("BAD_ARGUMENT", "captions must be karaoke, word or off");
        if (!QStringList{"off", "mild"}.contains(sf.sfx)) fail("BAD_ARGUMENT", "sfx must be off or mild");
        // The game as the short's session saw it (given, or from the recordings' folder/name): hits on its kills
        QString game = f["game"].toString();
        if (f["session"].isObject())
            if (const auto session = Direction::fromJson(f["session"].toObject())) {
                QStringList files;
                for (const Direction::Material& m : session->material) files << m.file;
                game = gameOf(session->options, files);
            }
        if (!game.isEmpty() && game != QLatin1String("none")) sf.game = game;
        sf.language = f["language"].toString();
        if (f["prepare"].toBool()) {
            const QJsonArray ops = shortFinishOps(project, timeline, sf, ctx, &done, &notes);
            return {{"project", project}, {"timeline", timeline}, {"ops", ops}, {"done", done}, {"notes", notes},
                    {"stamp", timelineStamp(project, timeline)}};
        }
        const bool captions = shortFinish(project, timeline, sf, ctx, &done, &notes);
        QJsonObject o{{"project", project}, {"timeline", timeline}, {"captions", captions}, {"done", done}};
        if (!notes.isEmpty()) o["notes"] = notes;
        return o;
    }
    if (a.contains("session") || a.contains("apply")) {
        if (!a.contains("session")) fail("BAD_ARGUMENT", "apply needs the session (its id like 'd3', or the session itself)");
        if (a.contains("recipe")) fail("BAD_ARGUMENT", "recipe makes a new session; session + apply uses one: not both");
        const Direction::Session s = load(a["session"]);
        const QJsonObject r = directApply(s, a["apply"].toObject(), a, ctx);
        // An AI's apply outside the app: the Direct page shows it like its own Übernehmen (not a plan or a dry run)
        if (!a["plan"].toBool() && !a["dry_run"].toBool() && !r.contains("plan")) Live::reportDirectApplied(a, Direction::toJson(s), r);
        return r;
    }
    Direction::Session s = directSuggest(a, ctx);
    store(s);
    const QJsonObject session = Direction::toJson(s);
    Live::reportDirection(a, session);
    return {{"session", session},
            {"next", QString("direct session:'%1' apply:{on:'all'} (or on:[n…], adjust:[{n, from_s, to_s}], order:[n…]%2)")
                         .arg(s.id, s.recipe == QLatin1String("short") ? QString(", variant:2") : QString())}};
}

void addDirectOps(QVector<OpDef>& ops)
{
    // Hidden (no usage): `direct` apply appends it; the session as it was applied, no undo step, the project changed
    ops << OpDef{"_direction", {}, {}, {},
                 [](Session& s, const QJsonObject& op) {
                     if (s.dryRun) return;
                     QJsonObject session = op.value("session").toObject();
                     if (!Direction::fromJson(session)) fail("BAD_ARGUMENT", "_direction: not a session");
                     s.project.setDirection(session);
                 },
                 {"session"}};
    // Hidden too: transcripts measured before the edit ({file: {language, words:[[from_ms, to_ms, text], …]}}) as the
    // files' transcripts (what rough_cut keeps when it transcribes)
    ops << OpDef{"_transcripts", {}, {}, {},
                 [](Session& s, const QJsonObject& op) {
                     const QJsonObject files = op.value("files").toObject();
                     for (auto it = files.begin(); it != files.end(); ++it) {
                         const QString file = absolute(it.key());
                         const MediaInfo* m = s.project.mediaInfo(file);
                         if (m && !m->transcripts.isEmpty()) continue; // the user's own stays
                         if (!m) s.media(file);
                         MediaTranscript t;
                         t.language = it.value().toObject()["language"].toString();
                         for (const QJsonValue& w : it.value().toObject()["words"].toArray())
                             t.words << TranscriptWord{qint64(w[0].toDouble()), qint64(w[1].toDouble()), w[2].toString()};
                         s.project.setTranscript(file, t);
                     }
                 },
                 {"files"}};
}

} // namespace Cli::detail
