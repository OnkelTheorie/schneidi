// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli `direct`: a recipe (short, highlights; talk, template and script come next) looks at the material and
// suggests moments with reasons (core/Direction.h) without changing anything; applying a session (all, or the ones
// switched on, edges moved, another order) builds the ops and runs them as ONE `edit` (a backup on a file, one undo
// step in the app; plan:true makes a normal plan instead). Sessions are kept like plans as d<N>.json in the plan
// folder for a day. The moments come from cli/DirectMoments.cpp, the picking from core/ShortPick and core/ReelPick.
#include "cli/AnalysisDetail.h"
#include "cli/DirectDetail.h"
#include "cli/Live.h"
#include "cli/Sandbox.h"

#include "core/AppPaths.h"
#include "core/Vocabulary.h"
#include "core/GameHud.h"
#include "core/I18n.h"
#include "core/LiveHost.h"
#include "core/ProjectFile.h"
#include "core/ReelPick.h"
#include "core/RenderJob.h"
#include "core/ShortPick.h"
#include "core/StreamPresets.h"
#include "engine/AudioAnalysis.h"
#include "engine/Extensions.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QRandomGenerator>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

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

// The options a recipe knows (help of `direct` + what the app's Direct page sends); anything else is refused, a
// misspelt option was ignored silently
const QStringList& recipeOptionKeys(const QString& recipe)
{
    static const QHash<QString, QStringList> keys{
        {"short", {"for", "min_s", "max_s", "aspect", "variants", "hook", "reframe", "game", "audio_stream", "target_lufs",
                   "captions", "sfx", "language", "accents", "game_terms", "captions_check"}},
        {"highlights", {"length_s", "aspect", "music", "order", "transitions", "game", "audio_stream", "target_lufs"}},
        {"talk", {"min_pause_s", "pad_s", "fillers", "language", "tracks", "threshold", "target_lufs", "room_tone"}},
        {"template", {"template", "auto", "game", "target_lufs"}},
        {"script", {"script", "script_file", "takes", "language", "target_lufs"}}};
    static const QStringList none;
    const auto it = keys.find(recipe);
    return it == keys.end() ? none : *it;
}

// The options typed once: seconds as a number or text ("15", "15s"), counts as a whole number ("2" too), switches as
// true/false. Text read as a number was 0 before (a talk cut every 1-frame pause, a short became 1 s long).
void checkOptions(QJsonObject& o, const QString& recipe)
{
    for (auto it = o.begin(); it != o.end();) {
        if (it.key().startsWith('_')) it = o.erase(it); // internal values of an earlier session (re-suggested)
        else ++it;
    }
    const QStringList& known = recipeOptionKeys(recipe);
    for (const QString& k : o.keys())
        if (!known.contains(k))
            fail("BAD_ARGUMENT", QString("recipe %1 has no option '%2' (options: %3)").arg(recipe, k, known.join(", ")));
    const auto number = [](const QJsonValue& v, bool* ok) {
        if (v.isDouble()) return *ok = true, v.toDouble();
        QString t = v.toString().trimmed().toLower();
        if (t.endsWith('s')) t.chop(1);
        return v.isString() ? t.trimmed().toDouble(ok) : (*ok = false, 0.0);
    };
    for (const char* k : {"min_s", "max_s", "length_s", "min_pause_s", "pad_s"}) {
        const QJsonValue v = o.value(k);
        if (!o.contains(k) || v.isNull()) continue;
        if (QLatin1String(k) == QLatin1String("length_s") && v.toString().trimmed().toLower() == QLatin1String("all"))
            continue;
        bool ok = false;
        const double x = number(v, &ok);
        if (!ok || !std::isfinite(x) || x < 0)
            fail("BAD_ARGUMENT", QString("options.%1 is seconds: a number (15) or text with s (\"15s\")%2")
                                     .arg(k, QLatin1String(k) == QLatin1String("length_s") ? " or 'all'" : ""));
        o[k] = x;
    }
    for (const char* k : {"variants", "takes"}) {
        const QJsonValue v = o.value(k);
        if (!o.contains(k) || v.isNull()) continue;
        bool ok = false;
        const double x = v.isDouble() ? (ok = true, v.toDouble()) : v.toString().trimmed().toDouble(&ok);
        if (!ok || x != std::floor(x)) fail("BAD_ARGUMENT", QString("options.%1 is a whole number (e.g. 2)").arg(k));
        o[k] = int(x);
    }
    for (const char* k : {"fillers", "room_tone", "auto"}) {
        const QJsonValue v = o.value(k);
        if (!o.contains(k) || v.isNull() || v.isBool()) continue;
        const QString t = v.toString().trimmed().toLower();
        if (t == QLatin1String("true") || t == QLatin1String("on")) o[k] = true;
        else if (t == QLatin1String("false") || t == QLatin1String("off")) o[k] = false;
        else fail("BAD_ARGUMENT", QString("options.%1 is true or false").arg(k));
    }
}

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
    for (const DirectMoment& c : from)
        if (c.file == m.file)
            for (double h : c.hits)
                if (h >= m.from && h < m.to && !s.hits.contains(h)) s.hits << h;
    std::sort(s.hits.begin(), s.hits.end());
    if (best) {
        s.reason = best->reason;
        if (s.score <= 0) s.score = best->score;
    } else {
        s.reason = {"fallback_end", {}};
    }
    return s;
}

// How far a recording's moments may reach: its length, or where its game picture ends (the desktop in its last seconds)
double usableLength(const Direction::Material& m)
{
    const double end = m.analysis["game_end_s"].toDouble(-1);
    return end > 0 && end < m.length ? end : m.length;
}

// bounded (recipe short): each moment within its lo/hi - from shortly before its first hit to a little after its last
// one or the player's death - and growing only inside them
QVector<ShortPick::Moment> asMoments(const QVector<DirectMoment>& list, bool bounded = false)
{
    QVector<ShortPick::Moment> out;
    for (const DirectMoment& m : list) {
        ShortPick::Moment x{m.file, m.from, m.to, m.score};
        if (bounded) {
            x.lo = m.lo;
            x.hi = m.hi;
            if (m.lo >= 0) x.from = std::max(x.from, m.lo);
            if (m.hi >= 0) x.to = std::min(x.to, m.hi);
            if (x.to <= x.from) x.to = m.hi >= 0 ? std::max(x.from, m.hi) : m.to;
        }
        out << x;
    }
    return out;
}

bool overlapsAny(const DirectMoment& c, const QVector<Direction::Suggestion>& list, const QStringList& files)
{
    return std::any_of(list.begin(), list.end(), [&](const Direction::Suggestion& s) {
        return s.file == files.value(c.file) && c.from < s.to && s.from < c.to;
    });
}

// The loudness a cut is leveled to when applied: options.target_lufs (default -14 LUFS, web and phone platforms; a
// script's takes are spoken word: -16, what `check` measures a talk against), NaN when it says false/'off' (the sound
// stays as recorded)
constexpr double kLevelCeilingDb = -1.2; // the master limiter's ceiling (under check's -1 dBTP with room for codecs)

double levelTarget(const QJsonObject& options, const QString& recipe = {})
{
    const QJsonValue v = options["target_lufs"];
    if (v.isUndefined() || v.isNull()) return recipe == QLatin1String("script") ? -16 : -14;
    if (v.isDouble()) return std::clamp(v.toDouble(), -40.0, -5.0);
    if (v.isBool() ? !v.toBool() : v.toString().trimmed().compare("off", Qt::CaseInsensitive) == 0)
        return std::numeric_limits<double>::quiet_NaN();
    fail("BAD_ARGUMENT", "target_lufs is the loudness in LUFS (e.g. -14) or false (leave the sound as it is)");
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

// A hook starts on the action: a hard cut in its lead-in (the round restarts after a round-end or death screen, a
// clip tool's jump) moves its start to the picture after the cut. Only the lead-in before the first hit counts (a fast
// turn in the fight is no cut to start at). The picture's scene scores are cached per file (as `scenes` reads them).
constexpr double kHookCut = 0.3;       // scene score of a hard cut (as `scenes`' default threshold)
constexpr double kHookBeforeHit = 0.4; // the hook keeps at least this much before the first hit

void startHookOnAction(Direction::Suggestion& x, const QVector<DirectMoment>& found, const QStringList& files)
{
    double first = -1;
    for (const DirectMoment& m : found)
        if (files.value(m.file) == x.file && m.first >= x.from && m.first < x.to && (first < 0 || m.first < first))
            first = m.first;
    if (first < 0 || first - x.from < kHookBeforeHit + 0.2) return;
    std::vector<PictureScore> scores;
    try {
        scores = pictureScores(openMedia({{"file", x.file}}), 320, [](double) {});
    } catch (const Error& e) {
        if (e.code == QLatin1String("CANCELED")) throw;
        return; // unreadable picture: the hook stays as it is
    }
    double cut = -1;
    for (const PictureScore& p : scores)
        if (p.seconds > x.from + 0.05 && p.seconds <= first - kHookBeforeHit && p.score >= kHookCut) cut = p.seconds;
    if (cut < 0) return;
    x.from = std::round(cut * 1000) / 1000;
    if (!x.labels.contains("hook_trimmed")) x.labels << "hook_trimmed";
}

// A short's accents on its hits: punch (default: a mild zoom punch) or off
QString shortAccents(const QJsonObject& options)
{
    const QString a = options["accents"].toString("punch").trimmed().toLower();
    if (a != QLatin1String("punch") && a != QLatin1String("off")) fail("BAD_ARGUMENT", "accents must be punch or off");
    return a;
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
    for (const Direction::Material& m : s.material) durations << usableLength(m);
    // The highlights first (as `short` always took them); without any, the sound events and picture changes
    QVector<DirectMoment> source = found.highlights;
    std::stable_sort(source.begin(), source.end(),
                     [](const DirectMoment& x, const DirectMoment& y) { return x.score > y.score; });
    if (source.isEmpty() && !found.extra.isEmpty()) {
        source = found.extra;
        s.notes << note("fallback");
    }
    const QVector<QVector<ShortPick::Moment>> picked =
        ShortPick::variants(asMoments(source, true), durations, variants, pick);
    if (source.isEmpty()) s.notes << note("no_moments");
    else if (picked.size() < variants) s.notes << note("fewer_variants", {{"n", int(picked.size())}, {"asked", variants}});
    // Shorter than min_s although the recordings are longer: the moments did not reach it without dead footage
    double material = 0;
    for (double d : durations) material += d;
    if (!picked.isEmpty() && material > pick.minS + 0.5)
        if (const double got = ShortPick::total(picked.first()); got < pick.minS - 0.5)
            s.notes << note("short_shorter", {{"s", std::round(got * 10) / 10}, {"min", pick.minS}});
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
        ShortPick::Moment m = asMoments({c}, true).first();
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
    // Each variant's hook starts on the action (not on a round-end screen before it)
    for (int v = 1; v <= int(picked.size()); ++v) {
        QVector<int> ns;
        for (const Direction::Suggestion& x : s.suggestions)
            if (x.variant == v) ns << x.n;
        const QVector<int> order = hookOrder(s.suggestions, ns, hookBest, files);
        for (Direction::Suggestion& x : s.suggestions)
            if (!order.isEmpty() && x.n == order.first()) startHookOnAction(x, source, files);
        failIfCanceled();
    }
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
    for (const Direction::Material& m : s.material) durations << usableLength(m);
    // Everything that stands out counts here (holiday clips: laughter, cheering, movement, loud)
    // (not the end of a replay clip: a fallback of recipe short; this recipe offers the ends when few stood out)
    QVector<DirectMoment> all;
    for (const DirectMoment& m : found.highlights + found.extra)
        if (m.source != QLatin1String("end")) all << m;
    std::stable_sort(all.begin(), all.end(), [](const DirectMoment& x, const DirectMoment& y) { return x.score > y.score; });
    QVector<ReelPick::Moment> picked = ReelPick::pick(asMoments(all), durations, pick);
    // The same pick without growing: which candidates it took (the others stay offered, see below)
    ReelPick::Options plain = pick;
    plain.growBeforeS = plain.growAfterS = 0;
    const QVector<ReelPick::Moment> ungrown = pick.targetS > 0 ? ReelPick::pick(asMoments(all), durations, plain) : picked;
    // Fewer moments than the target: ReelPick grew them (more build-up and reaction); the note says from how much
    if (pick.targetS > 0 && !picked.isEmpty()) {
        double found = 0, grown = 0;
        for (const ReelPick::Moment& m : ungrown) found += m.to - m.from;
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
    // The other candidates, switched off: those the pick did not take (the ungrown pick decides - a moment that
    // grew into a candidate's surroundings must not hide it), cut to what the grown picks leave free; a candidate whose
    // own sound is inside a grown pick is in the video already
    int added = 0;
    QVector<Direction::Suggestion> taken;
    for (const ReelPick::Moment& m : ungrown) {
        Direction::Suggestion x;
        x.file = files.value(m.file);
        x.from = m.from;
        x.to = m.to;
        taken << x;
    }
    if (picked.size() != ungrown.size()) taken = s.suggestions; // the end fallback (nothing stood out)
    for (const DirectMoment& c : all) {
        if (added >= 20) break;
        if (overlapsAny(c, taken, files)) continue;
        ShortPick::Moment m{c.file, c.from, c.to, c.score};
        if (m.to - m.from < pick.minMomentS) ShortPick::widen(m, std::min(pick.minMomentS, durations.value(c.file)) - (m.to - m.from), durations.value(c.file));
        for (const Direction::Suggestion& g : s.suggestions) {
            if (!g.on || g.file != files.value(m.file) || g.to <= m.from || g.from >= m.to) continue;
            if (g.from <= m.from) m.from = std::max(m.from, g.to);
            else if (g.to >= m.to) m.to = std::min(m.to, g.from);
            else if (g.from - m.from >= m.to - g.to) m.to = g.from; // the pick inside it: the longer side
            else m.from = g.to;
        }
        if (m.to - m.from < 1.0 || c.to <= m.from || c.from >= m.to) continue;
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
    const QString file = newPlanId(dir, "d");
    // The number counts per data folder (a test run, another XDG_DATA_HOME starts at d1 again): a random tag makes the
    // id name this session only, so an id from another folder or another client's answer finds nothing instead of
    // someone else's session (videotest 21.11.: apply with "d1" took another agent's cut)
    s.id = QString("%1-%2").arg(file).arg(QRandomGenerator::global()->bounded(0x10000), 4, 16, QChar('0'));
    // Into the name newPlanId reserved (an empty file) in one step: a load in between never reads half a session
    QSaveFile f(QDir(dir).filePath(file + ".json"));
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(Direction::toJson(s)).toJson(QJsonDocument::Compact)) < 0 ||
        !f.commit())
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
    static const QRegularExpression valid("^(d[1-9][0-9]{0,8})(-[0-9a-f]{4})?$");
    const QRegularExpressionMatch m = valid.match(id);
    if (!m.hasMatch()) fail("BAD_ARGUMENT", "session: an id like 'd3-7f2a' (from the answer of direct) or the session itself");
    pruneSessions(sessionDir());
    QFile f(QDir(sessionDir()).filePath(m.captured(1) + ".json"));
    if (f.open(QIODevice::ReadOnly) && f.size() > 0) {
        s = Direction::fromJson(QJsonDocument::fromJson(f.readAll()).object(), &error);
        if (!s) fail("BAD_ARGUMENT", "session " + id + ": " + error);
        // Only the whole id opens it: the number alone (or another tag) belongs to a session of another folder/client
        if (s->id.toLower() != id && s->id.contains('-'))
            fail("SESSION_EXPIRED", QString("no session '%1' here (ids look like '%2-xxxx'; use the id from your own "
                                            "answer of direct, or suggest again)")
                                        .arg(id, m.captured(1)));
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

// The speech level of a voice stream in the moments used (dB, the power of the 0.1 s blocks the voice detection hears
// words in); nullopt = no words in them (a stream that only breathes or hums is never raised)
std::optional<double> speechLevelDb(const QString& file, int stream, const QVector<const Direction::Suggestion*>& used)
{
    double power = 0;
    int blocks = 0;
    for (const Direction::Suggestion* x : used) {
        failIfCanceled();
        std::vector<bool> speech;
        const auto levels = AudioAnalysis::fileBlockLevels(file, stream, x->from, x->to, 0.1, &speech);
        if (!levels) continue;
        for (size_t i = 0; i < levels->size() && i < speech.size(); ++i)
            if (speech[i] && (*levels)[i] > -90) {
                power += std::pow(10.0, (*levels)[i] / 10);
                ++blocks;
            }
    }
    if (blocks < 3) return std::nullopt; // (under 0.3 s of words: too little to tell)
    return 10 * std::log10(power / blocks);
}

// The voices placed brought to one level, per recording (its microphone and its voice chat, recorded together): each
// up to the loudest of them (at most kVoiceRaiseMaxDb; under 1 dB left as it is), only voices with words in the
// moments. Not across recordings: one shout in another clip would set every voice there. *said: what was raised
constexpr double kVoiceRaiseMaxDb = 12;
void levelVoices(QJsonArray& voiceRefs, QStringList* said)
{
    QHash<QString, double> loudest; // per recording
    for (const QJsonValue& v : voiceRefs)
        if (v["level_db"].isDouble())
            loudest[v["media"].toString()] = std::max(loudest.value(v["media"].toString(), -200.0), v["level_db"].toDouble());
    for (int i = 0; i < voiceRefs.size(); ++i) {
        QJsonObject v = voiceRefs[i].toObject();
        if (!v["level_db"].isDouble()) continue;
        const double gain = std::min(kVoiceRaiseMaxDb, loudest.value(v["media"].toString()) - v["level_db"].toDouble());
        if (gain < 1) continue;
        v["gain_db"] = std::round(gain * 10) / 10;
        voiceRefs[i] = v;
        *said << QString("%1 +%2 dB (%3)")
                     .arg(v["name"].toString().isEmpty() ? QString("stream %1").arg(v["stream"].toInt()) : v["name"].toString())
                     .arg(v["gain_db"].toDouble())
                     .arg(QFileInfo(v["media"].toString()).fileName());
    }
}

// Sound of the recordings: audio_stream 'auto' (default: the game streams of game material with several streams,
// plus each voice stream - microphone, voice chat - that is heard in a used moment of its recording), 'game' (only
// the game streams, as before 0.26), 'all', or numbers/names - as `short` always chose (per file: undefined = all).
// *voiced: per recording the voice streams placed and why; *gameRefs/*voiceRefs: the streams placed ({media, stream})
QVector<QJsonValue> soundStreams(const Direction::Session& s, const QString& game, QStringList* said,
                                 const QVector<const Direction::Suggestion*>& used, QStringList* dropped,
                                 QStringList* voiced, QJsonArray* gameRefs, QJsonArray* voiceRefs)
{
    const QString choice = s.options.contains("audio_stream") ? s.options["audio_stream"].toVariant().toString().trimmed()
                                                              : QString(s.recipe == QLatin1String("short") ? "auto" : "game");
    const bool autoVoice = choice.compare("auto", Qt::CaseInsensitive) == 0;
    QVector<QJsonValue> out(s.material.size());
    for (int i = 0; i < s.material.size(); ++i) {
        const QStringList& names = s.material[i].audioStreams;
        if (choice.compare("all", Qt::CaseInsensitive) == 0 || names.size() < 2) continue;
        if (!autoVoice && choice.compare("game", Qt::CaseInsensitive) != 0) {
            out[i] = choice;
            continue;
        }
        const bool mixAndChat = std::any_of(names.begin(), names.end(), StreamPresets::isMixTitle)
                             && std::any_of(names.begin(), names.end(), StreamPresets::isChatOrMusicTitle);
        if ((!s.options.contains("audio_stream") || autoVoice) && game == QLatin1String("none") && !mixAndChat) continue;
        const QVector<int> g = StreamPresets::gameStreams(names);
        if (g.isEmpty()) continue;
        QVector<const Direction::Suggestion*> mine;
        for (const Direction::Suggestion* x : used)
            if (x->file == s.material[i].file) mine << x;
        // The voice streams heard inside a used moment (Material::voice: where something is said or laughed)
        QVector<int> voices;
        QStringList heardIn;
        if (autoVoice)
            for (const QJsonValue& v : s.material[i].voice) {
                const int k = v["stream"].toInt(-1);
                if (k < 0 || k >= names.size() || g.contains(k)) continue;
                double heard = 0;
                for (const QJsonValue& span : v["s"].toArray())
                    for (const Direction::Suggestion* x : mine)
                        heard += std::max(0.0, std::min(span[1].toDouble(), x->to) - std::max(span[0].toDouble(), x->from));
                if (heard < 0.3) continue; // a breath, a click
                voices << k;
                heardIn << QString("%1 (%2 s heard)").arg(names[k]).arg(std::round(heard * 10) / 10);
            }
        QJsonArray list;
        QStringList kept, left;
        for (int k : g) list << k, kept << names[k];
        for (int k : voices) list << k;
        for (int k = 0; k < names.size(); ++k)
            if (!g.contains(k) && !voices.contains(k) && !StreamPresets::isMixTitle(names[k])) // the mix only repeats the others
                left << (names[k].isEmpty() ? QString("stream %1").arg(k) : names[k]);
        out[i] = list;
        // Only the recordings in the cut are named (not a switched-off one, not one without a moment)
        if (mine.isEmpty()) continue;
        for (int k : g) *gameRefs << QJsonObject{{"media", s.material[i].file}, {"stream", k}};
        for (int k : voices) {
            QJsonObject ref{{"media", s.material[i].file}, {"stream", k}, {"name", names[k]}};
            if (const std::optional<double> level = speechLevelDb(s.material[i].file, k, mine)) ref["level_db"] = *level;
            *voiceRefs << ref;
        }
        *said << QString("%1: %2").arg(QFileInfo(s.material[i].file).fileName(), kept.join(", "));
        if (!heardIn.isEmpty())
            *voiced << QString("%1: %2").arg(QFileInfo(s.material[i].file).fileName(), heardIn.join(", "));
        if (!left.isEmpty()) *dropped << QString("%1 (%2)").arg(left.join(", "), QFileInfo(s.material[i].file).fileName());
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
    if (a.contains("options") && !a["options"].isObject() && !a["options"].isNull())
        fail("BAD_ARGUMENT", "options is an object of the recipe's settings, e.g. {min_s:15, max_s:40}");
    s.options = a["options"].toObject();
    checkOptions(s.options, s.recipe);
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
    levelTarget(s.options); // checked now, not only when applying
    if (s.recipe == QLatin1String("short")) shortAccents(s.options);
    // A game neither given nor in the file names: looked for in the picture (its kill feed), kept in the session
    if (!s.options.contains("game") && gameOf(s.options, files) == QLatin1String("none"))
        if (const QString seen = gameInPicture(files, ctx); !seen.isEmpty()) {
            s.options["game"] = seen;
            s.notes << note("game_recognised", {{"game", seen}});
        }
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
                      QString* madeProject, bool* hadBackups);
} // namespace

QJsonObject directApply(const Direction::Session& s, const QJsonObject& apply, const QJsonObject& args, Context& ctx)
{
    // A project this apply made itself goes again when it fails before its edit is done (canceled, an op refused): no
    // half project, and no backups or plans of it left; once the edit is saved it stays whatever comes after
    QString made;
    bool hadBackups = false;
    try {
        return applyOnce(s, apply, args, ctx, &made, &hadBackups);
    } catch (...) {
        if (!made.isEmpty()) {
            QFile::remove(made);
            // Only a backup folder this apply made: a free project name can still have the backups of an older,
            // deleted project of that name (review 21.11.)
            if (!hadBackups) QDir(ProjectFile::backupDir(made)).removeRecursively();
        }
        throw;
    }
}

namespace {
QJsonObject applyOnce(const Direction::Session& s, const QJsonObject& apply, const QJsonObject& args, Context& ctx,
                      QString* madeProject, bool* hadBackups)
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
                for (const double h : b.hits) // (b's hits keep their punch)
                    if (!a->hits.contains(h)) a->hits << h;
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
    // The project: given, the session's, else a new one in the folder for projects
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
        QString freeDir, freeBase; // set when the name is our own free choice (it may be taken before `new` runs)
        if (project.isEmpty()) {
            const QFileInfo first(files.first());
            // In project_dir when given, else the folder for projects (Settings > Project; never the recordings'
            // folder, a clip tool's output an unattended run must not fill); the app's own page: next to the first one
            QString dir = args["project_dir"].toString().trimmed();
            // A work folder (`mcp --root`): the projects go into it, everything else would be OUTSIDE_ROOT
            dir = !dir.isEmpty()     ? absolute(dir)
                  : Live::host()     ? first.absolutePath()
                  : Sandbox::active() ? Sandbox::root()
                                     : AppPaths::projectFolder();
            if (!dry && !QDir().mkpath(dir)) fail("SAVE_FAILED", "cannot make the folder " + dir);
            freeDir = dir;
            freeBase = first.completeBaseName() + "-" + s.recipe;
            project = freeFile(freeDir, freeBase, "." + QString(ProjectFile::Extension));
        } else if (QFileInfo(project).suffix().isEmpty()) {
            project += "." + QString(ProjectFile::Extension);
        }
        if (dry) fail("BAD_ARGUMENT", "dry_run and plan need an existing project (give 'project')");
        const bool streams = std::any_of(s.material.begin(), s.material.end(), [](const Direction::Material& m) { return m.audioStreams.size() > 1; });
        // Only the recordings in the cut go into the new project's media (not a switched-off one, not one without a
        // moment)
        QStringList usedMedia;
        for (const Direction::Suggestion* x : list)
            if (!usedMedia.contains(x->file)) usedMedia << x->file;
        QJsonObject n{{"project", project}, {"media", QJsonArray::fromStringList(usedMedia)},
                      {"template", game != QLatin1String("none") || streams ? "game" : "neutral"}};
        if (s.material.first().fps > 0) n["fps"] = s.material.first().fps;
        failIfCanceled();
        // Another apply can take the free name between choosing and creating: `new` answers EXISTS, the next free
        // name is tried (a few times) instead of failing the whole apply
        for (int attempt = 0;; ++attempt) {
            n["project"] = project;
            *hadBackups = QFileInfo::exists(ProjectFile::backupDir(project));
            try {
                run("new", n);
                break;
            } catch (const Error& e) {
                if (freeDir.isEmpty() || e.code != QLatin1String("EXISTS") || attempt >= 8) throw;
                project = freeFile(freeDir, freeBase, "." + QString(ProjectFile::Extension));
            }
        }
        *madeProject = project;
        created = true;
        done << QString("new project %1 (template %2)").arg(project, n["template"].toString());
    }
    QStringList soundSaid, soundDropped, voiced;
    QJsonArray gameRefs, voiceRefs;
    const QVector<QJsonValue> soundOf = soundStreams(s, game, &soundSaid, list, &soundDropped, &voiced, &gameRefs,
                                                     &voiceRefs);
    if (!soundSaid.isEmpty())
        done << QString("sound: the game streams (%1)").arg(soundSaid.join("; "));
    if (!voiced.isEmpty())
        done << QString("sound: voices heard in the moments placed too, the game sound ducked 6 dB under them (%1)")
                    .arg(voiced.join("; "));
    // The quieter voices up to the loudest (a voice chat partner far under the microphone), before the duck
    QStringList raised;
    levelVoices(voiceRefs, &raised);
    if (!raised.isEmpty()) done << QString("sound: voices leveled to the loudest one: %1").arg(raised.join("; "));
    // Said on its own, not only inside `done`: voice chat and the microphone are not in the video
    if (!soundDropped.isEmpty()) {
        soundDropped.removeDuplicates();
        notes << QString("left out of the sound (voice chat, microphone, music not heard in the moments): %1 - to keep "
                         "them, suggest again with options.audio_stream 'all' or their names (\"System Output,Discord\"); "
                         "'game' = only the game sound")
                     .arg(soundDropped.join("; "));
    }

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
    // The voices placed: the game sound (the bed) a little under them while they speak - by the streams placed, not by
    // their names (an unplayable stream is not placed, an untitled one's name depends on the UI language)
    if (!voiceRefs.isEmpty() && (!gameRefs.isEmpty() || !raised.isEmpty()))
        ops << QJsonObject{{"op", "_voice_mix"}, {"game", gameRefs}, {"voice", voiceRefs}, {"amount_db", -6}};
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
        // A mild zoom punch on every hit in it (only the punch: no flash, no shake), on the copy (new_timeline
        // switched to it); accents:'off' leaves them out
        if (shortAccents(s.options) != QLatin1String("off")) {
            QJsonArray at;
            for (const Direction::Suggestion* x : list)
                for (double h : x->hits)
                    if (h >= x->from && h < x->to)
                        at << QJsonObject{{"media", x->file}, {"source", QString("%1s").arg(h, 0, 'f', 3)}};
            if (!at.isEmpty()) {
                ops << QJsonObject{{"op", "accent"}, {"at", at}, {"preset", "punch"}, {"strength", 0.5}};
                done << QString("accents: a mild zoom punch on %1 hit%2").arg(at.size()).arg(at.size() == 1 ? "" : "s");
            }
        }
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
    // A project file opens on the result (the app's own project: the page shows it), not on an older empty timeline
    const bool appProject = Live::isAppProject(project) || Live::host();
    if (!appProject) ops << QJsonObject{{"op", "timeline"}, {"timeline", result}, {"open", true}};
    // The result as loud as a platform wants it (like `short`): the master to target_lufs (default -14) behind its
    // limiter, measured (mixer target_lufs). Not in the app: measuring there would hold its window (the page's export
    // check does it); not for `short` (_no_check: it levels after its captions and sounds, before rendering)
    const double targetLufs = levelTarget(s.options, s.recipe);
    if (!appProject && !args["_no_check"].toBool() && !std::isnan(targetLufs))
        ops << QJsonObject{{"op", "mixer"}, {"track", "master"}, {"limiter", true}, {"limiter_db", kLevelCeilingDb},
                           {"target_lufs", targetLufs}};
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
    for (const QJsonValue& n : edited["notes"].toArray())
        if (const QJsonObject l = n.toObject()["loudness"].toObject(); !l.isEmpty())
            done << QString("sound: '%1' leveled to %2 LUFS (target %3, master limiter %4 dBTP)")
                        .arg(result)
                        .arg(std::round(l["after_lufs"].toDouble() * 10) / 10)
                        .arg(l["target_lufs"].toDouble())
                        .arg(kLevelCeilingDb);
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
        if (timeline.isEmpty())
            fail("BAD_ARGUMENT", "finish needs the short's timeline inside it: finish:{\"timeline\":\"<the apply answer's "
                                 "timeline>\", captions?, sfx?} (with project)");
        QJsonArray done, notes, noteKeys;
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
        QJsonValue terms = f["game_terms"];
        QJsonValue check = f["captions_check"];
        if (f["session"].isObject())
            if (const auto session = Direction::fromJson(f["session"].toObject())) {
                QStringList files;
                for (const Direction::Material& m : session->material) files << m.file;
                game = gameOf(session->options, files);
                if (!terms.isBool()) terms = session->options["game_terms"];
                if (!f.contains("captions_check")) check = session->options["captions_check"];
            }
        sf.gameTerms = Vocabulary::useGameTerms(terms);
        sf.captionsCheck = Vocabulary::captionsCheck(check);
        QJsonArray leftOut;
        sf.leftOut = &leftOut;
        if (!game.isEmpty() && game != QLatin1String("none")) sf.game = game;
        sf.language = f["language"].toString();
        if (f["prepare"].toBool()) {
            const QJsonArray ops = shortFinishOps(project, timeline, sf, ctx, &done, &notes, &noteKeys);
            QJsonObject o{{"project", project}, {"timeline", timeline}, {"ops", ops}, {"done", done}, {"notes", notes},
                          {"note_keys", noteKeys}, {"stamp", timelineStamp(project, timeline)}};
            if (!leftOut.isEmpty()) o["captions_left_out"] = leftOut;
            return o;
        }
        const bool captions = shortFinish(project, timeline, sf, ctx, &done, &notes);
        QJsonObject o{{"project", project}, {"timeline", timeline}, {"captions", captions}, {"done", done}};
        if (!notes.isEmpty()) o["notes"] = notes;
        if (!leftOut.isEmpty()) o["captions_left_out"] = leftOut;
        return o;
    }
    if (a.contains("session") || a.contains("apply")) {
        if (!a.contains("session")) fail("BAD_ARGUMENT", "apply needs the session (its id like 'd3-7f2a', or the session itself)");
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
    // Hidden too: transcripts measured before the edit ({file: {language, stream?, words:[[from_ms, to_ms, text], …]}}
    // or a list of those per audio stream) as the files' transcripts (what rough_cut keeps when it transcribes)
    ops << OpDef{"_transcripts", {}, {}, {},
                 [](Session& s, const QJsonObject& op) {
                     const QJsonObject files = op.value("files").toObject();
                     for (auto it = files.begin(); it != files.end(); ++it) {
                         const QString file = absolute(it.key());
                         const QJsonArray list = it.value().isArray() ? it.value().toArray() : QJsonArray{it.value()};
                         for (const QJsonValue& one : list) {
                             const int stream = one["stream"].toInt(0);
                             const MediaInfo* m = s.project.mediaInfo(file);
                             if (m && (it.value().isArray() ? m->transcript(stream) != nullptr : !m->transcripts.isEmpty()))
                                 continue; // the user's own stays
                             if (!m) continue; // a recording the cut does not use stays out of the project
                             MediaTranscript t;
                             t.audioStream = stream;
                             t.language = one["language"].toString();
                             for (const QJsonValue& w : one["words"].toArray())
                                 t.words << TranscriptWord{qint64(w[0].toDouble()), qint64(w[1].toDouble()), w[2].toString()};
                             s.project.setTranscript(file, t);
                         }
                     }
                 },
                 {"files"}};
    // Hidden too: the voices placed in a short brought to one level and the game sound ducked under them ({game:
    // [{media, stream}], voice:[{media, stream, gain_db?}], amount_db}). The clips are found by their recording and
    // stream; a duck that finds nothing or fails is left out with a note - it never fails the cut
    ops << OpDef{"_voice_mix", {}, {}, {},
                 [](Session& s, const QJsonObject& op) {
                     const auto placed = [](const Clip& c, const QJsonArray& refs) {
                         return std::any_of(refs.begin(), refs.end(), [&](const QJsonValue& r) {
                             return absolute(r["media"].toString()) == c.mediaPath && r["stream"].toInt() == c.audioStream;
                         });
                     };
                     const QJsonArray gameRefs = op.value("game").toArray(), voiceRefs = op.value("voice").toArray();
                     // The quieter voices up (clip volume), before the duck measures them
                     for (const QJsonValue& r : voiceRefs) {
                         if (!r["gain_db"].isDouble()) continue;
                         QJsonArray ids;
                         for (const Track& t : s.project.timeline().audio)
                             for (const Clip& c : t.clips)
                                 if (placed(c, QJsonArray{r})) ids << c.id;
                         if (!ids.isEmpty())
                             applyOp(s, QJsonObject{{"op", "volume"}, {"clips", ids}, {"db", r["gain_db"].toDouble()}});
                     }
                     if (gameRefs.isEmpty()) return;
                     const Timeline& tl = s.project.timeline();
                     QJsonArray game;
                     QStringList voice;
                     for (int i = 0; i < tl.audio.size(); ++i) {
                         bool hasGame = false, hasVoice = false;
                         for (const Clip& c : tl.audio[i].clips) {
                             if (placed(c, gameRefs)) {
                                 game << c.id;
                                 hasGame = true;
                             } else if (placed(c, voiceRefs)) {
                                 hasVoice = true;
                             }
                         }
                         if (hasVoice && !hasGame) voice << QString("A%1").arg(i + 1);
                     }
                     if (game.isEmpty() || voice.isEmpty()) {
                         s.notes << "the game sound is not ducked under the voices: the cut has no track of each";
                         return;
                     }
                     try {
                         applyOp(s, QJsonObject{{"op", "duck"}, {"clips", game}, {"by", voice.join(',')},
                                                {"amount_db", op.value("amount_db").toDouble(-6)}});
                     } catch (const Error& e) {
                         s.notes << "the game sound is not ducked under the voices: " + e.message;
                     }
                 },
                 {"game", "voice", "amount_db"}};
}

} // namespace Cli::detail
