#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "cli/Guide.h"
#include "cli/Live.h"
#include "cli/Sandbox.h"
#include "cli/Serve.h"

#include "core/Editor.h"
#include "core/StreamPresets.h"
#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/GradeWindows.h"
#include "core/LiveHost.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/RenderJob.h"
#include "core/Retime.h"
#include "core/Selection.h"
#include "core/Subtitles.h"
#include "core/Timecode.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"
#include "engine/Engine.h"
#include "engine/Exporter.h"
#include "engine/Profiles.h"
#include "engine/RenderQueue.h"
#include "engine/Snapshot.h"
#include "engine/Stabilizer.h"

#include <QBuffer>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <memory>

namespace Cli {

namespace detail {

void fail(const QString& code, const QString& message) { throw Error{code, message}; }

QString absolute(const QString& path)
{
    // "@app" stays (not a file in the working directory); in a work folder (server) relative paths start there and
    // nothing outside it is allowed
    return Sandbox::resolve(path);
}

// ---------- Times ----------

// Frames from 120 (frames), "120" (frames), "4.8s" (seconds) or "00:00:04:20" (timecode)
int parseTime(const QJsonValue& v, const ProjectFormat& fmt, const QString& what)
{
    constexpr double kMax = 2e9; // as Timecode::parse: beyond that the cast to int would be undefined
    if (v.isDouble()) {
        const double d = v.toDouble();
        if (d < 0 || d != std::floor(d) || d > kMax)
            fail("BAD_ARGUMENT", QString("%1: frames must be a whole number >= 0, got %2").arg(what).arg(d));
        return int(d);
    }
    const QString s = v.toString().trimmed();
    if (s.endsWith('s')) {
        bool ok = false;
        const double sec = s.chopped(1).toDouble(&ok);
        if (ok && sec >= 0 && sec * fmt.rate.fps() <= kMax) return int(std::lround(sec * fmt.rate.fps()));
    } else if (!s.isEmpty()) {
        bool ok = false;
        const int frames = s.toInt(&ok);
        if (ok && frames >= 0) return frames;
        const int tc = Timecode::parse(s, fmt.rate.timebase());
        if (tc >= 0 && s.contains(':')) return tc;
    }
    fail("BAD_ARGUMENT", QString("%1: '%2' is not a time (use frames, \"4.5s\" or \"HH:MM:SS:FF\")").arg(what, s));
}

QString tc(int frames, const ProjectFormat& fmt) { return Timecode::format(frames, fmt.rate.timebase()); }
double seconds(int frames, const ProjectFormat& fmt) { return std::round(frames / fmt.rate.fps() * 1000) / 1000; }

// [from, to) as frames, seconds and timecode
QJsonObject range(int from, int to, const ProjectFormat& fmt)
{
    return {{"from", from}, {"to", to}, {"from_s", seconds(from, fmt)}, {"to_s", seconds(to, fmt)},
            {"from_tc", tc(from, fmt)}, {"to_tc", tc(to, fmt)}};
}

ProjectFormat parseFormat(const QJsonObject& a, ProjectFormat f)
{
    if (a.contains("width")) f.width = a["width"].toInt();
    if (a.contains("height")) f.height = a["height"].toInt();
    if (f.width < 16 || f.height < 16 || f.width > 8192 || f.height > 8192 || f.width % 2 || f.height % 2)
        fail("BAD_ARGUMENT", "width/height must be even numbers between 16 and 8192");
    if (a.contains("fps")) {
        const double fps = a["fps"].toDouble();
        if (fps < 1 || fps > 120) fail("BAD_ARGUMENT", "fps must be between 1 and 120");
        f.rate = nearestFrameRate(fps);
    }
    return f;
}

QJsonObject formatJson(const ProjectFormat& f)
{
    return {{"width", f.width}, {"height", f.height}, {"fps", std::round(f.rate.fps() * 1000) / 1000},
            {"fps_label", QString("%1/%2").arg(f.rate.num).arg(f.rate.den)}};
}

// ---------- Projects ----------

// Where a session's project comes from: the app's project (writing inside the app), or a loaded copy
struct Session::Opened {
    QString path;
    std::unique_ptr<QLockFile> lock;
    Project* inApp = nullptr;
    std::unique_ptr<Project> own;
    bool live = false;
    QJsonObject app;
};

namespace {

Session::Opened openSession(const QString& file, Session::Access access)
{
    Session::Opened o;
    const bool anyApp = Live::isAppProject(file);
    o.path = anyApp ? file.trimmed() : absolute(file);
    const auto load = [&o](const QByteArray& json) {
        ProjectData d;
        QString error;
        if (!ProjectFile::fromJson(json, o.path, &d, &error)) fail("BAD_PROJECT", error);
        o.own = std::make_unique<Project>();
        o.own->load(d);
    };
    // Inside the app (a command the server runs): its project, or a copy for reading
    if (LiveHost* h = Live::host()) {
        const QString appPath = h->liveProjectPath();
        o.live = true;
        o.app = Live::appState(h);
        if (access == Session::Access::Write) o.inApp = h->liveProject();
        else {
            o.own = std::make_unique<Project>();
            o.own->load(h->liveProject()->data());
        }
        o.path = appPath;
        return o;
    }
    // schneidi-cli: the project is open in the app -> read what the app shows (writing commands went there)
    if (Live::Connection* c = Live::connectionFor(o.path)) {
        if (access == Session::Access::Write) fail("INTERNAL", "changing commands run inside the app");
        const Live::Snapshot snap = Live::snapshot(c);
        o.path = snap.path;
        load(snap.data);
        o.live = true;
        o.app = snap.app;
        return o;
    }
    if (anyApp) fail("NO_APP", "the schneidi app is not running");
    if (!QFileInfo::exists(o.path)) fail("NOT_FOUND", "project not found: " + o.path);
    if (access == Session::Access::Write) {
        o.lock = ProjectFile::lock(o.path);
        if (!o.lock) fail("PROJECT_LOCKED", "the project is being saved by someone else (schneidi app?), try again: " + o.path);
    }
    ProjectData d;
    QString error;
    if (!ProjectFile::load(o.path, &d, &error)) fail("BAD_PROJECT", error);
    o.own = std::make_unique<Project>();
    o.own->load(d);
    Sandbox::checkProject(*o.own);
    return o;
}

} // namespace

Session::Session(const QString& file, Access access) : Session(openSession(file, access)) {}

Session::Session(Opened&& o)
    : m_own(std::move(o.own)), path(o.path), lock(std::move(o.lock)), project(o.inApp ? *o.inApp : *m_own),
      live(o.live), app(o.app), m_inApp(o.inApp != nullptr)
{
    m_open = project.currentSequence();
    // The same import presets as the app: the project's own (from its start template) or the settings'
    editor.setStreamPresets(project.effectiveImportPresets());
}

Session::~Session()
{
    // In the app: show the user's timeline again if an op opened another one
    if (m_inApp && project.sequence(m_open) && project.currentSequence() != m_open) project.setCurrentSequence(m_open);
}

int Session::sequenceOf(const QJsonValue& v) const
{
    if (v.isDouble() && project.sequence(v.toInt())) return v.toInt();
    const QString name = v.toVariant().toString().trimmed();
    for (const Sequence& q : project.sequences())
        if (q.name.compare(name, Qt::CaseInsensitive) == 0 || QString::number(q.id) == name) return q.id;
    QStringList names;
    for (const Sequence& q : project.sequences()) names << QString("%1 \"%2\"").arg(q.id).arg(q.name);
    fail("NOT_FOUND", QString("no timeline '%1' (there are: %2)").arg(name, names.join(", ")));
}

void Session::failEmpty() const
{
    QStringList full;
    for (const Sequence& q : project.sequences())
        if (q.id != project.currentSequence() && !q.compound && TimelineOps::endFrame(q.timeline) > 0)
            full << QString("%1 \"%2\"").arg(q.id).arg(q.name);
    QString message = QString("the timeline %1 \"%2\" is empty")
                          .arg(project.currentSequence())
                          .arg(project.sequenceName(project.currentSequence()));
    if (!full.isEmpty()) message += " - timelines with clips: " + full.join(", ") + " (pass 'timeline': id or name)";
    fail("EMPTY", message);
}

void Session::selectTimeline(const QJsonValue& v, bool open)
{
    if (v.isUndefined() || v.isNull()) return;
    const int id = sequenceOf(v);
    project.setCurrentSequence(id);
    if (open) m_open = id;
}

QString Session::save()
{
    if (m_inApp) return {}; // the app has it (unsaved, like the user's own edits; undo works)
    if (!lock) fail("INTERNAL", "session was opened read-only");
    // The app opens the timeline the user had open (or the one an op asked for), not the last one edited
    if (project.sequence(m_open)) project.setCurrentSequence(m_open);
    Sandbox::checkProject(project); // e.g. media of an imported timeline
    const QString backup = ProjectFile::backup(path);
    QString error;
    if (!ProjectFile::save(project.data(), path, &error)) fail("SAVE_FAILED", error);
    return backup;
}

const MediaInfo& Session::media(const QString& file)
{
    const QString path = absolute(file);
    if (const MediaInfo* m = project.mediaInfo(path)) return *m;
    if (!QFileInfo::exists(path)) fail("NOT_FOUND", "media not found: " + path);
    const MediaInfo info = Engine::probe(format(), path);
    if (info.length <= 0 || (!info.hasVideo && !info.hasAudio)) fail("BAD_MEDIA", "cannot read media: " + path);
    project.addMedia(info);
    return *project.mediaInfo(path);
}

bool isProjectFile(const QString& path)
{
    return path.endsWith("." + QString(ProjectFile::Extension)) || Live::isAppProject(path);
}

QString trackName(TrackRef r) { return QString("%1%2").arg(r.kind == TrackKind::Video ? "V" : "A").arg(r.index + 1); }

// "V1", "A2" or a number (1 = first track); returns the index (0-based)
int parseTrack(const QJsonValue& v, TrackKind* kind)
{
    if (v.isUndefined() || v.isNull()) return 0;
    QString s = v.isDouble() ? QString::number(v.toInt()) : v.toString().trimmed().toUpper();
    TrackKind k = TrackKind::Video;
    if (s.startsWith('V') || s.startsWith('A')) {
        k = s.startsWith('A') ? TrackKind::Audio : TrackKind::Video;
        s = s.mid(1);
    }
    bool ok = false;
    const int n = s.toInt(&ok);
    if (!ok || n < 1 || n > 99) fail("BAD_ARGUMENT", "track must look like V1, A2 or 1, got " + v.toVariant().toString());
    if (kind) *kind = k;
    return n - 1;
}

// Title/subtitle style: only what differs from `def`
QJsonObject titleStyleJson(const TitleStyle& t, const TitleStyle& def)
{
    QJsonObject o;
    const auto num = [&](const char* k, double v, double d) {
        if (v != d) o[k] = v;
    };
    const auto color = [&](const char* k, const QColor& v, const QColor& d) {
        if (v != d) o[k] = v.alpha() == 255 ? v.name() : v.name(QColor::HexArgb);
    };
    const auto flag = [&](const char* k, bool v, bool d) {
        if (v != d) o[k] = v;
    };
    if (t.font != def.font) o["font"] = t.font;
    num("size", t.size, def.size);
    color("color", t.color, def.color);
    flag("bold", t.bold, def.bold);
    flag("italic", t.italic, def.italic);
    if (t.align != def.align) o["align"] = QStringList{"left", "center", "right"}.value(t.align);
    num("x", t.posX, def.posX);
    num("y", t.posY, def.posY);
    // Outline/box switched on: always with their color and size (the defaults are not obvious)
    flag("outline", t.outlineOn, def.outlineOn);
    color("outline_color", t.outlineColor, t.outlineOn ? QColor() : def.outlineColor);
    num("outline_width", t.outlineWidth, t.outlineOn ? -1 : def.outlineWidth);
    flag("box", t.boxOn, def.boxOn);
    color("box_color", t.boxColor, t.boxOn ? QColor() : def.boxColor);
    num("box_padding", t.boxPad, t.boxOn ? -1 : def.boxPad);
    return o;
}

QJsonObject clipJson(const Project& p, const Clip& c, TrackRef ref)
{
    const ProjectFormat& f = p.format();
    QJsonObject o{{"id", c.id}, {"track", trackName(ref)}, {"name", p.clipName(c)},
                  {"start", c.start}, {"end", c.end()}, {"start_tc", tc(c.start, f)}, {"end_tc", tc(c.end(), f)}};
    if (c.isTitle()) {
        o["kind"] = "title";
        o["text"] = c.title.text;
        o["style"] = titleStyleJson(c.title, TitleStyle{});
    } else if (c.isCompound()) {
        o["kind"] = "compound";
    } else if (c.isMulticam()) {
        // A segment of a multicam clip: shows one angle of it (1-based like the grid and the keys)
        o["kind"] = "multicam";
        o["multicam"] = c.sequenceId;
        o["angle"] = c.angle + 1;
        if (const Sequence* s = p.sequence(c.sequenceId); s && c.angle >= 0 && c.angle < s->angles.size())
            o["angle_name"] = s->angles[c.angle].name;
    } else {
        o["kind"] = "media";
        o["media"] = c.mediaPath;
        o["src_in"] = c.in;
        o["src_out"] = c.out + 1;
    }
    if (c.linkId) o["link"] = c.linkId;
    if (!c.enabled) o["enabled"] = false;
    if (ref.kind == TrackKind::Audio && c.volumeDb != 0) o["volume_db"] = c.volumeDb;
    if (ref.kind == TrackKind::Audio && c.pan != 0) o["pan"] = c.pan;
    if (ref.kind == TrackKind::Audio && c.audioStream != 0) o["audio_stream"] = c.audioStream;
    if (ref.kind == TrackKind::Video) {
        // Values also while their Inspector section is switched off (then they do not apply)
        const ClipTransform& t = c.transform;
        const ClipTransform def;
        QJsonObject to;
        const QPair<const char*, double> fields[] = {
            {"zoom_x", t.zoomX}, {"zoom_y", t.zoomY}, {"x", t.posX}, {"y", t.posY}, {"rotation", t.rotation},
            {"crop_left", t.cropLeft}, {"crop_right", t.cropRight}, {"crop_top", t.cropTop},
            {"crop_bottom", t.cropBottom}, {"opacity", t.opacity}};
        const double defaults[] = {def.zoomX, def.zoomY, def.posX, def.posY, def.rotation, def.cropLeft,
                                   def.cropRight, def.cropTop, def.cropBottom, def.opacity};
        for (int i = 0; i < int(std::size(fields)); ++i)
            if (fields[i].second != defaults[i]) to[fields[i].first] = fields[i].second;
        if (!t.transformOn) to["transform_on"] = false;
        if (!t.cropOn) to["crop_on"] = false;
        if (!t.compositeOn) to["opacity_on"] = false;
        // Blurred fill around the fitted picture: with its strength and darkening, so the effective look is visible
        const bool blur = t.fill == FitFill::Blur;
        if (blur) to["fit"] = "blur";
        if (blur || t.fillBlur != def.fillBlur) to["fit_blur"] = t.fillBlur;
        if (blur || t.fillDim != def.fillDim) to["fit_dim"] = t.fillDim;
        if (!to.isEmpty()) o["transform"] = to;
    }
    if (const QJsonObject keys = keyframesJson(c); !keys.isEmpty()) o["keyframes"] = keys;
    if (c.freeze) o["freeze"] = true;
    if (c.isRetimed() && !c.keepPitch) o["keep_pitch"] = false;
    if (c.fadeIn) o["fade_in"] = c.fadeIn;
    if (c.fadeOut) o["fade_out"] = c.fadeOut;
    if (c.transIn) o["transition_in"] = c.transIn;
    if (c.transOut) o["transition_out"] = c.transOut;
    if (const QJsonObject st = transitionStyleJson(c.transInStyle); c.transIn && !st.isEmpty()) o["transition_in_style"] = st;
    if (const QJsonObject st = transitionStyleJson(c.transOutStyle); c.transOut && !st.isEmpty()) o["transition_out_style"] = st;
    if (c.hasRamp()) {
        const MediaInfo* m = p.mediaInfo(c.mediaPath);
        const RetimeMap map(c, m ? m->length : 0);
        const QVector<double> points = map.pointMaterial();
        QJsonArray ramp;
        for (int i = 0; i < c.ramp.size() && i < points.size(); ++i) {
            QJsonObject r{{"at", c.start + int(std::lround(points[i])) - c.in}, {"speed", c.ramp[i].speed}};
            if (c.ramp[i].smooth) r["smooth"] = c.ramp[i].smooth;
            ramp << r;
        }
        o["speed_ramp"] = ramp;
    }
    if (c.speed != 1.0) o["speed"] = c.speed;
    if (c.reverse) o["reverse"] = true;
    if (!c.effects.isEmpty()) {
        QJsonArray fx;
        const auto json = [](const QVariant& v) {
            return v.typeId() == QMetaType::QColor ? QJsonValue(v.value<QColor>().name()) : QJsonValue::fromVariant(v);
        };
        for (const EffectInstance* fxi : EffectRegistry::renderOrder(c)) { // the order they render in
            const EffectInstance& e = *fxi;
            QJsonObject eo{{"effect", e.effectId}};
            QJsonObject params;
            QMap<int, QJsonObject> windows; // grade: its windows apart ("w1.x" -> windows[1].x)
            for (auto it = e.params.begin(); it != e.params.end(); ++it) {
                QString key;
                if (const int n = e.effectId == QLatin1String("grade") ? GradeWindows::windowOf(it.key(), &key) : 0)
                    windows[n][key] = json(it.value());
                else
                    params[it.key()] = json(it.value());
            }
            if (!params.isEmpty()) eo["params"] = params;
            if (!windows.isEmpty()) {
                QJsonArray list;
                for (auto it = windows.begin(); it != windows.end(); ++it) {
                    QJsonObject w = it.value();
                    w["window"] = it.key();
                    list << w;
                }
                eo["windows"] = list;
            }
            if (!e.enabled) eo["enabled"] = false;
            fx << eo;
        }
        o["effects"] = fx;
    }
    return o;
}

QJsonObject timelineJson(const Project& p)
{
    const Timeline& tl = p.timeline();
    const ProjectFormat& f = p.format();
    QJsonArray tracks;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (int i = 0; i < tl.tracks(k).size(); ++i) {
            const Track& t = tl.tracks(k)[i];
            QJsonArray clips;
            for (const Clip& c : t.clips) clips << clipJson(p, c, {k, i});
            QJsonObject o{{"track", trackName({k, i})}, {"clips", clips}};
            if (!t.name.isEmpty()) o["name"] = t.name;
            if (t.locked) o["locked"] = true;
            if (t.muted) o["muted"] = true;
            if (t.hidden) o["hidden"] = true;
            if (!t.color.isEmpty()) o["color"] = t.color;
            if (k == TrackKind::Audio) {
                if (t.volumeDb != 0) o["volume_db"] = t.volumeDb;
                if (t.pan != 0) o["pan"] = t.pan;
                if (t.solo) o["solo"] = true;
                if (t.dynamics != TrackDynamics{}) {
                    const TrackDynamics& d = t.dynamics;
                    o["compressor"] = QJsonObject{{"on", d.on},           {"threshold_db", d.thresholdDb},
                                                  {"ratio", d.ratio},     {"attack_ms", d.attackMs},
                                                  {"release_ms", d.releaseMs}, {"knee_db", d.kneeDb},
                                                  {"makeup_db", d.makeupDb}};
                }
                if (t.eq != TrackEq{}) {
                    static const char* kinds[] = {"high_pass", "low_shelf", "bell", "high_shelf", "low_pass"};
                    QJsonArray bands;
                    for (size_t b = 0; b < t.eq.bands.size(); ++b) {
                        const EqBand& band = t.eq.bands[b];
                        QJsonObject bo{{"band", int(b) + 1}, {"kind", kinds[int(band.kind)]}, {"on", band.on},
                                       {"freq", band.freq}, {"q", band.q}};
                        if (band.kind != EqBand::Kind::HighPass && band.kind != EqBand::Kind::LowPass)
                            bo["gain_db"] = band.gainDb;
                        bands << bo;
                    }
                    o["eq"] = QJsonObject{{"on", t.eq.on}, {"bands", bands}};
                }
            }
            tracks << o;
        }
    QJsonArray markers;
    for (int m : tl.markers) markers << m;
    const int end = TimelineOps::endFrame(tl);
    QJsonObject o{{"id", p.currentSequence()}, {"name", p.sequenceName(p.currentSequence())},
                  {"length", end}, {"length_s", seconds(end, f)}, {"length_tc", tc(end, f)}, {"tracks", tracks},
                  {"markers", markers}};
    QJsonArray subtitles;
    for (int i = 0; i < tl.subtitles.size(); ++i) {
        const SubtitleTrack& st = tl.subtitles[i];
        QJsonArray cues;
        for (const SubtitleCue& c : st.cues)
            cues << QJsonObject{{"id", c.id}, {"start", c.start}, {"end", c.end}, {"start_tc", tc(c.start, f)},
                                {"end_tc", tc(c.end, f)}, {"text", c.text}};
        QJsonObject so{{"track", QString("ST%1").arg(i + 1)}, {"cues", cues}};
        if (!st.name.isEmpty()) so["name"] = st.name;
        if (!st.enabled) so["enabled"] = false; // not shown/burnt in (one subtitle track at a time)
        if (st.locked) so["locked"] = true;
        if (const QJsonObject style = titleStyleJson(st.style, subtitleBaseStyle()); !style.isEmpty()) so["style"] = style;
        subtitles << so;
    }
    if (!subtitles.isEmpty()) o["subtitles"] = subtitles;
    // Always, so a change (also back to the default) can be checked: fader, limiter on/off and its ceiling
    o["master"] = QJsonObject{
        {"volume_db", tl.masterVolumeDb}, {"limiter", tl.masterLimiter}, {"limiter_db", tl.masterLimiterDb}};
    if (tl.markIn >= 0) o["mark_in"] = tl.markIn;
    if (tl.markOut >= 0) o["mark_out"] = tl.markOut + 1;
    return o;
}

QString findAsset(Asset kind, const QString& name)
{
    QVector<EffectFolders::LutEntry> list;
    if (kind == Asset::Lut) list = EffectFolders::builtinLuts() + EffectFolders::userLuts();
    if (kind == Asset::Luma) list = EffectFolders::builtinTransitions() + EffectFolders::userTransitions();
    if (kind == Asset::Preset) list = EffectFolders::userPresets();
    for (const EffectFolders::LutEntry& e : list) {
        const QString id = EffectFolders::isBuiltin(e.path) ? EffectFolders::builtinId(e.path) : QString();
        const QString grouped = e.group.isEmpty() ? e.name : e.group + "/" + e.name;
        if (name == id || name.compare(e.name, Qt::CaseInsensitive) == 0 || name.compare(grouped, Qt::CaseInsensitive) == 0
            || name == QFileInfo(e.path).fileName())
            return e.path;
    }
    if (const QString file = absolute(name); QFileInfo::exists(file)) return file;
    static const char* what[] = {"LUT", "luma transition", "effect preset"};
    fail("NOT_FOUND", QString("no %1 '%2' (`effects` lists them, or give a file)").arg(what[int(kind)], name));
}

QJsonArray timelinesJson(const Project& p, const QHash<int, QVector<Multicam::Angle>>& sync)
{
    QJsonArray list;
    for (const Sequence& q : p.sequences()) {
        const int end = TimelineOps::endFrame(q.timeline);
        QJsonObject o{{"id", q.id}, {"name", q.name}, {"length", end}, {"length_tc", tc(end, p.format())}};
        if (q.compound) o["compound"] = true;
        if (q.multicam) {
            o["multicam"] = true;
            const auto it = sync.find(q.id);
            o["angles"] = multicamAnglesJson(q, it != sync.end() ? &it.value() : nullptr);
        }
        list << o;
    }
    return list;
}

QString binPath(const Project& p, int bin)
{
    QStringList parts;
    for (int guard = 0; bin != 0 && guard < 64; ++guard) {
        const auto it = std::find_if(p.bins().begin(), p.bins().end(), [bin](const MediaBin& b) { return b.id == bin; });
        if (it == p.bins().end()) break;
        parts.prepend(it->name);
        bin = it->parent;
    }
    return parts.join('/');
}

QJsonObject importPresetsJson(const StreamPresets& p)
{
    QJsonArray rules;
    for (const StreamRule& r : p.rules) {
        QJsonObject o{{"title", r.title},
                      {"action", r.action == StreamRule::Action::Mute   ? "mute"
                                 : r.action == StreamRule::Action::Skip ? "skip"
                                                                        : "place"}};
        if (r.volumeDb != 0) o["volume_db"] = r.volumeDb;
        if (!r.trackName.isEmpty()) o["track"] = r.trackName;
        rules << o;
    }
    return {{"mute_mix", p.muteMix}, {"rules", rules}};
}

QJsonObject projectJson(const Project& p, const QString& path)
{
    QJsonArray media;
    for (const MediaInfo& m : p.media()) {
        QJsonObject o{{"path", m.path}, {"frames", m.length}, {"seconds", seconds(m.length, p.format())},
                      {"video", m.hasVideo}, {"audio", m.hasAudio}};
        if (m.isImage) o["image"] = true;
        if (!QFileInfo::exists(m.path)) o["offline"] = true;
        if (m.audioStreamCount() > 1) o["audio_streams"] = m.audioStreamCount();
        if (m.bin) o["bin"] = binPath(p, m.bin);
        if (!m.clipColor.isEmpty()) o["color"] = m.clipColor;
        if (!m.flags.isEmpty()) o["flags"] = QJsonArray::fromStringList(m.flags);
        if (m.markIn >= 0) o["mark_in"] = m.markIn;
        if (m.markOut >= 0) o["mark_out"] = m.markOut + 1;
        if (!m.markers.isEmpty()) { // source markers (sync:'marker' of the multicam op)
            QJsonArray markers;
            for (int f : m.markers) markers << f;
            o["markers"] = markers;
        }
        media << o;
    }
    QJsonObject format = formatJson(p.format());
    if (p.fixedResolution()) format["fixed_resolution"] = true; // from the start template: the first clip keeps it
    if (p.fixedFrameRate()) format["fixed_frame_rate"] = true;
    QJsonObject o{{"project", path}, {"format", format}, {"media", media}, {"timelines", timelinesJson(p)},
                  {"timeline", timelineJson(p)}};
    if (p.importPresets()) o["import_presets"] = importPresetsJson(*p.importPresets());
    return o;
}

} // namespace detail

using namespace detail;

namespace {
QJsonObject cmdHelp(const QJsonObject& a, Context&);
QJsonObject cmdGuide(const QJsonObject& a, Context&);
} // namespace

const QVector<Command>& commands()
{
    using T = ParamType;
    static const QVector<Command> list{
        {"help", "List all commands with their parameters (JSON).",
         {{"command", T::String, "only this command", false, true},
          {"op", T::String, "with command 'edit': the details of these ops, e.g. \"speed_ramp\" or \"title,track\" "
                            "(\"all\" = every op; without: only what each op is for)"}}, cmdHelp},
        {"guide", "How to edit well with schneidi: the workflow (look at the material, small steps, check the render) "
                  "and how-tos per task (highlight reel, talking head, short/9:16, ad, subtitles, color, sound, "
                  "several cameras, export, checking a render, automation with watch folders). Read it first if you "
                  "have not edited with schneidi before; without a topic: the main guide and the list of topics.",
         {{"topic", T::String, "one task guide, e.g. \"highlights\" or \"ad\" (see 'topics')", false, true}},
         cmdGuide},
        {"new", "Create a new empty project (.schneidi), optionally with media already in the media pool. It starts "
                "from a start template: the user's default (set in the app; \"neutral\" unless they chose another) or "
                "the one you name. Built-in: neutral (2 video + 2 audio tracks, nothing muted), game (recordings with "
                "several audio streams, e.g. OBS: the Mix stream comes in muted when single streams exist; master "
                "limiter), podcast (audio tracks named Voice 1, Voice 2, Music; master limiter), vertical (1080x1920 "
                "for shorts/reels/TikTok, frame rate from the material). `templates` lists them with the user's own.",
         {{"project", T::Path, "project file to create (.schneidi)", true, true},
          {"template", T::String, "start template: id or name from `templates` (e.g. \"podcast\"), or a template "
                                  "file (.json) or another project (.schneidi) whose format and tracks to take "
                                  "(default: the user's default template)"},
          {"width", T::Integer, "timeline width (default: the template's, 1920)"},
          {"height", T::Integer, "timeline height (default: the template's, 1080)"},
          {"fps", T::Number, "frame rate (default: the template's, 25; 23.976, 24, 25, 29.97, 30, 50, 59.94, 60)"},
          {"media", T::Paths, "media files to import"},
          {"overwrite", T::Boolean, "replace an existing project"}},
         cmdNew},
        {"templates", "List the start templates for `new`: built-in ones and the user's own (made in the app: File › "
                      "Save as Start Template), which one is the default, and what each sets: format (fixed_resolution "
                      "/ fixed_frame_rate: the first clip does not change it), tracks with names and states, master, "
                      "import_presets (own import presets for audio streams; absent = the user's settings).",
         {{"template", T::String, "only this one (id, name or file)", false, true}},
         cmdTemplates},
        {"info", "Show a project: format, media, its timelines and one timeline's tracks and clips (ids, positions, "
                 "source ranges). Each track shows its header and mixer state when not default: name, color, muted, "
                 "solo, hidden, locked; audio tracks volume_db, pan, compressor, eq. 'master': volume_db, limiter "
                 "(on/off), limiter_db (ceiling). To check such settings after an edit, summary:true is enough. 'watch': "
                 "watched folders and the revision of the newest file they brought in (see `watch`).",
         {{"project", T::Path, "project file", true, true},
          {"summary", T::Boolean, "short form: length, counts of clips/tracks/markers, no clip list (track states and "
                                  "master stay)"},
          {"timeline", T::String, "timeline to show (id or name; default: the open one)"}},
         cmdInfo},
        {"import", "Add media files to a project's media pool, or a timeline from another editor: an FCP7 XML "
                   "(Premiere: File › Export › Final Cut Pro XML; DaVinci: File › Export › Timeline › FCP 7 XML), "
                   "OpenTimelineIO (.otio) or EDL becomes a new timeline, its media imported. Carried over: tracks, "
                   "clips, gaps, constant speed, volume, opacity, dissolves, markers; not: color, effects, titles.",
         {{"project", T::Path, "project file", true, true},
          {"files", T::Paths, "media files", false, true},
          {"timeline_file", T::Path, "timeline of another program: .xml (FCP7), .otio or .edl"},
          {"format", T::String, "timeline_file format if the extension does not tell: xml, otio, edl"},
          {"search", T::Paths, "folders to look for media of the timeline that are not where the file says "
                               "(another computer); default: the folder of the timeline file"},
          {"name", T::String, "name of the new timeline (default: the one in the file)"},
          {"fps", T::Number, "EDL only (it has no frame rate): its frame rate (default: the project's)"}},
         cmdImport},
        {"export_timeline", "Write a timeline for another editor: FCP7 XML (Premiere Pro: File › Import; DaVinci "
                            "Resolve: File › Import › Timeline), OpenTimelineIO (.otio, DaVinci Resolve 18.5+) or EDL "
                            "(any editor; one video track). "
                            "'warnings' list what cannot be carried over (titles, effects, color, compound clips).",
         {{"project", T::Path, "project file", true, true},
          {"out", T::Path, "file to write: .xml, .otio or .edl", true},
          {"format", T::String, "xml (FCP7), otio or edl (default: from the extension of 'out')"},
          {"timeline", T::String, "which timeline (id or name; default: the open one)"},
          {"overwrite", T::Boolean, "replace an existing file"}},
         cmdExportTimeline},
        {"edit", "Apply edit operations to a project and save it (a backup of the previous state is kept). If the "
                 "project is open in the schneidi app, the app's project changes instead (one undo step, not saved). "
                 "Answer: 'changes' (added clips and subtitles as `info` shows them, with their new ids; changed "
                 "ones with id, track, start, end and the values that changed, keyframes as a count per parameter; "
                 "removed ids) and the timeline as a summary.",
         {{"project", T::Path, "project file", true, true},
          {"ops", T::Json, editHelp(), true},
          {"timeline", T::String, "timeline to edit (id or name from `info`; default: the open one)"},
          {"dry_run", T::Boolean, "only show what would change, do not save"},
          {"full", T::Boolean, "answer with the whole timeline (every clip, like `info`) instead of the summary"}},
         cmdEdit},
        {"backups", "List the backup copies of a project (every save keeps the previous state, newest first).",
         {{"project", T::Path, "project file or \"@app\"", true, true}}, cmdBackups},
        {"restore", "Undo: put a backup copy back as the project (the current state becomes a backup itself, so "
                    "restore can be undone too). Open in the app: replaces the timelines there as one undo step, "
                    "nothing is saved.",
         {{"project", T::Path, "project file or \"@app\"", true, true},
          {"backup", T::String, "number from `backups` (default 1 = state before the last change) or a backup file"}},
         cmdRestore},
        {"app", "The schneidi app (live, when it has the project open): see what the user sees (playhead, selection, "
                "open timeline) and control it. `info`, `edit` and the other commands already work on the open "
                "project there (an edit is one undo step in the app; the user saves).",
         {{"action", T::String, "state, seek {at}, play {at?}, stop, select {clips}, undo, redo, save {path?}, proxies "
                                "{files?, generate?} (preview proxies; default all video files), clean_proxies "
                                "{delete?} (proxy files neither this project nor a recently opened one needs, with the "
                                "reason: missing/changed original, unused, other size, leftover; a dry run unless "
                                "delete is true)", true, true},
          {"project", T::Path, "project file open in the app (default \"@app\" = the project of the running app)"},
          {"at", T::Time, "seek/play: position in the open timeline"},
          {"clips", T::Times, "select: clip ids (empty = nothing selected)"},
          {"files", T::Paths, "proxies: media files (default: all video in the project)"},
          {"generate", T::Boolean, "proxies: start generating missing ones (default true; false = only show the state)"},
          {"delete", T::Boolean, "clean_proxies: really delete the listed files (default false = only list them)"},
          {"path", T::Path, "save: file for a project that was never saved (a new .schneidi file, never overwrites)"}},
         cmdApp},
        {"watch", "Watch folders: new media files in a folder come into the project's media pool on their own (a screen "
                  "recorder's or game clip tool's output, renders of another program, a pipeline that drops "
                  "recordings). Nothing is watched unless you add a folder. Without add/remove/list: one pass - "
                  "imports the files that are new and complete (unchanged for settle_s, readable; still recording "
                  "= 'pending', look again later) and answers them in 'new' (with their length and bin; 'note' when "
                  "the first recording's frame rate differs from the still empty project's). The app watches the "
                  "folders itself while it has the project open (one undo step per batch); 'revision' "
                  "counts every file a watch folder brought in, so 'since' with the revision of your last answer "
                  "shows what arrived since (also what the app took in). Command line: --follow keeps running and "
                  "prints one JSON line per new file (react to it: `highlights`, `edit`, `render`).",
         {{"project", T::Path, "project file or \"@app\"", true, true},
          {"add", T::Path, "folder to watch from now on (again: change its settings)"},
          {"bin", T::String, "with add: media pool bin for its files, e.g. \"Recordings\" or \"Clips/Raw\" (created if "
                             "missing; default: Master)"},
          {"subfolders", T::Boolean, "with add: also files in subfolders"},
          {"types", T::String, "with add: which media, comma list of video, audio, image (default all)"},
          {"existing", T::Boolean, "with add: also import the files that are in the folder already (default: only "
                                   "new ones)"},
          {"remove", T::Path, "stop watching this folder (imported files stay)"},
          {"list", T::Boolean, "only list the watched folders and the revision, import nothing"},
          {"since", T::Integer, "'new' = every file watch folders brought in after this revision (default: what this "
                                "pass imported)"},
          {"settle_s", T::Number, "seconds a file must stay unchanged to count as complete (default 3; more for slow "
                                  "network folders)"},
          {"follow", T::Boolean, "command line only: keep running, one JSON line per new file (stop with Ctrl+C)"}},
         cmdWatch},
        {"render", "Render the project timeline to a video, animated GIF, image sequence or audio file.",
         {{"project", T::Path, "project file", true, true},
          {"out", T::Path, "output file (extension added if missing)", true},
          {"preset", T::String, presetHelp()},
          {"format", T::String,
           "h264 (default), h265, av1, vp9 (WebM); prores (422 HQ), prores422, proreslt, proresproxy, prores4444, "
           "dnxhr-sq, dnxhr-hq, dnxhr-hqx (MOV); gif (animated, no sound; e.g. height 480, fps 15); png, tiff, jpeg = "
           "image sequence (out 'x.png' writes x/x_00001.png …); audio only: aac, mp3, opus, wav, aiff, flac, alac; "
           "srt = only the subtitle file"},
          {"quality", T::Integer, "0 = high (default), 1 = medium, 2 = small, 3 = web: high with a bit rate cap like YouTube recommends (H.264/H.265 only; the YouTube and phone presets use it). ProRes, DNxHR, GIF and images have a fixed quality"},
          {"height", T::Integer, "shorter image edge, e.g. 720 (default: timeline resolution)"},
          {"size", T::String, "fixed output size, e.g. 1080x1920 (portrait; the picture is fitted in)"},
          {"fps", T::Number, "output frame rate (default: the timeline's)"},
          {"encoder", T::String, "H.264/H.265 video encoder: auto (default: a hardware encoder of the graphics card when "
                                 "one works here, else software), software, nvenc (NVIDIA), vaapi (AMD/Intel on Linux), "
                                 "qsv (Intel Quick Sync). A hardware encoder that does not work falls back to software; "
                                 "the answer's 'encoder' is the codec actually used"},
          {"audio_bitrate", T::Integer, "kbit/s for AAC/MP3/Opus (default 320)"},
          {"subtitles", T::String, "none (default), burn (into the picture) or srt (file next to the video)"},
          {"from", T::Time, "start of the range (default: timeline start)"},
          {"to", T::Time, "end of the range, exclusive (default: timeline end)"},
          {"timeline", T::String, "timeline to render (id or name; default: the open one)"},
          {"overwrite", T::Boolean, "replace an existing output file"}},
         cmdRender},
        {"probe", "Media file details: duration, frames, resolution, frame rate, audio streams. With gaps: the stutter "
                  "of a recording with variable frame rate (variable_frame_rate true: game capture keeps the time "
                  "stamps of the frames it got, so where the game lagged the picture stands still).",
         {{"file", T::Path, "media file", false, true},
          {"files", T::Paths, "several media files at once (a list of results)"},
          {"project", T::Path, "count frames in this project's frame rate (default: the file's own)"},
          {"gaps", T::Boolean, "also find jumps in the picture's time stamps longer than 2 frame durations: 'jumps' "
                               "(at_s, length_s, missing_frames; from/to = frames that repeat the old picture) and "
                               "'stutter' (jumps less than 0.5 s apart as one stretch, from/to). Fast (reads the "
                               "packets, no decoding), cached. Cut stutter stretches out of the ranges you use"}},
         cmdProbe},
        {"silence", "Find pauses in the audio of a media file or a project's timeline. Returns 'silence' and "
                    "'sound' ranges [from, to); for a project they are timeline frames and 'sound' fits the 'keep' "
                    "edit operation directly (jump cut).",
         {{"file", T::Path, "media file, or a project (.schneidi or \"@app\"): its timeline sound as exported", true, true},
          {"project", T::Path, "for a media file: count frames in this project's frame rate"},
          {"timeline", T::String, "for a project: which timeline (id or name; default: the open one)"},
          {"tracks", T::String, "for a project: only these audio tracks, e.g. \"A3,A5\" or by name \"Mikrofon\" "
                                "(track or audio stream name; default: what is audible)"},
          {"from", T::Time, "for a project: start of the range"},
          {"to", T::Time, "for a project: end of the range"},
          {"threshold_db", T::Number, "quieter than this counts as silence (default -35)"},
          {"min", T::Time, "shortest pause (default 0.5s)"},
          {"pad", T::Time, "sound kept around each pause (default 0.1s)"}},
         cmdSilence},
        {"overview", "A first look at material in one call, a few lines of text: per stretch of time (window, about 20 "
                     "of them) how loud the sound is and how much of it is silent, how much the picture moves (0 = "
                     "still, 10-30 = a fast game or handheld), how many cuts and how bright (media file), or which "
                     "clips play there (project). Cheap: read it first, then pick the detailed analysis (`silence`, "
                     "`scenes`, `peaks`, `transcribe`, `frames`) for the parts that matter.",
         {{"file", T::Path, "media file or project (.schneidi)", false, true},
          {"files", T::Paths, "several media files (one overview each)"},
          {"window", T::Time, "length of a stretch (default: about 1/20 of the whole, on whole seconds)"},
          {"timeline", T::String, "for a project: which timeline (default: the open one)"},
          {"audio_stream", T::Integer, "for a media file: which audio stream, 0 = first"},
          {"project", T::Path, "count frames in this project's frame rate"}},
         cmdOverview},
        {"scenes", "Find scene changes (cuts) in a video file. Returns the 'cuts' and the 'scenes' between them as "
                   "ranges [from, to) in source frames, e.g. to pick shots for 'keep' or to look at them with `frames`. "
                   "With 'region' it finds events in one part of the picture instead ('changes': a new kill feed line, "
                   "a score or HUD change); look at the region with `frames` crop first to pick it.",
         {{"file", T::Path, "video file", false, true},
          {"files", T::Paths, "several video files (one result each)"},
          {"region", T::String, "only this part of the picture \"x,y,w,h\" (pixels or fractions 0..1, like frames crop)"},
          {"color", T::String, "with region: only pixels of this colour count (\"#e7e371\": a mark of what you look "
                               "for, e.g. the yellow of your own kill feed lines; pick it from a `frames` crop). "
                               "'changes' are then moments where more of it appears in a new place (a new line) "
                               "and stays; the same mark looking stronger on another background does not count"},
          {"tolerance", T::Integer, "with color: largest difference per channel, 0..255 (default 25)"},
          {"only", T::String, "with color: appear (default: more of the colour), disappear or both"},
          {"min_area", T::Number, "with color: smallest change as a part of the whole picture (default 0.0001, about "
                                  "200 pixels in 1080p; a kill feed mark is 0.01-0.05 %)"},
          {"project", T::Path, "count frames in this project's frame rate (recommended before editing)"},
          {"threshold", T::Number, "how different two frames must be, 0..1 (default 0.3; lower finds more cuts)"},
          {"min", T::Time, "shortest scene (default 0.5s)"}},
         cmdScenes},
        {"peaks", "Find loud moments in the sound of media files (shots, explosions, shouts, applause): the loudest "
                  "frame of each burst above the threshold, at least 'min' apart. Useful with `scenes` region to find "
                  "highlights.",
         {{"file", T::Path, "media file", false, true},
          {"files", T::Paths, "several media files (one result each)"},
          {"threshold_db", T::Number, "sample peak in dBFS that counts (default: 12 dB above the file's median level)"},
          {"min", T::Time, "shortest distance between two peaks (default 0.5s)"},
          {"top", T::Integer, "only the loudest N peaks (in time order)"},
          {"group", T::Time, "also return 'groups': peaks less than this apart as one burst (a fight, a rally) "
                             "with from/to, hits, the loudest peak_db and hits per second, e.g. \"2s\""},
          {"audio_stream", T::Integer, "which audio stream, 0 = first"},
          {"project", T::Path, "count frames in this project's frame rate"}},
         cmdPeaks},
        {"beats", "Tempo and structure of a song in one call, to cut to the music (not to a fixed grid): bpm (70-220 "
                  "by default, half/double tempo resolved by which beats are loud) with a confidence, the beat grid "
                  "(beat_phase = first beat, beat_period: beat k is at beat_phase + k * beat_period), the bar ones "
                  "(downbeats, 4/4 assumed, with a confidence), phrases of 4 or 8 bars with their energy, the energy "
                  "per bar as two lines of digits (overall and bass), breaks (the bass or the whole sound drops out) "
                  "and drops (the sound coming back after a break, best first). Times in seconds and frames. Put the "
                  "strongest moments on the drops and phrase starts, cuts on beats or bar ones. Source: a media file "
                  "or a project (its timeline sound as exported, times in timeline frames).",
         {{"file", T::Path, "song (media file), or a project (.schneidi or \"@app\"): its timeline sound as exported", true, true},
          {"project", T::Path, "for a media file: count frames in this project's frame rate"},
          {"timeline", T::String, "for a project: which timeline (id or name; default: the open one)"},
          {"tracks", T::String, "for a project: only these audio tracks, e.g. \"A2\" (the music; track or audio stream "
                                "name; default: what is audible)"},
          {"from", T::Time, "start of the range (a media file: in its own time)"},
          {"to", T::Time, "end of the range"},
          {"audio_stream", T::Integer, "for a media file: which audio stream, 0 = first"},
          {"min_bpm", T::Number, "slowest tempo to report (default 70)"},
          {"max_bpm", T::Number, "fastest tempo to report (default 220; e.g. 150 to get 100 instead of 200)"},
          {"beats", T::Boolean, "also return every beat (s, frame; bar_one on the bar ones)"},
          {"apply", T::String, "\"markers\": put timeline markers on the phrase starts (or the bars, see markers); "
                               "project or \"@app\" only, live in the app one undo step"},
          {"markers", T::String, "with apply: phrases (default) or bars"}},
         cmdBeats},
        {"highlights", "Candidates for a highlight video: kills from the kill feed (with region + color, see "
                       "`scenes` color) and loud moments, as source ranges with lead-in and lead-out, best first. "
                       "Kills closer than 'join' are one candidate (a multi kill scores more). Without a kill feed the "
                       "sound alone (bursts of loud moments) gives the candidates, e.g. for sport or vlogs. 'keep' "
                       "of each file fits the 'keep' edit operation; check candidates with `frames` crop. A candidate "
                       "over stutter in the recording (jumps in the time stamps, see `probe` gaps) has 'stutter' (the "
                       "stretches) and 'clean' (its parts without them, with their kills); 'keep_clean' is 'keep' with "
                       "only those parts - use it to drop the lag, not the moment. 'kill_feed_most_pct' = the most "
                       "of the colour at one time in percent of the picture (0 = never there: check region and colour; "
                       "kill feed marks stay well under 0.3); bigger patches of it (a banner in that colour) are not "
                       "counted as kills but listed in 'banners' (seconds).",
         {{"file", T::Path, "video file", false, true},
          {"files", T::Paths, "several video files (one result each, plus 'best': all candidates by score)"},
          {"region", T::String, "where the kill feed is, \"x,y,w,h\" (fractions 0..1 or pixels; look with `frames` "
                                "crop first). Only the part with the player's mark: e.g. Valorant \"0.70,0.04,0.22,0.23\", "
                                "CS2 \"0.72,0.04,0.28,0.2\""},
          {"color", T::String, "the mark of the player's own kill feed lines, e.g. Valorant \"#e7e371\" (yellow), "
                               "CS2 \"#d8444a\" (a thin red frame: tolerance 40)"},
          {"tolerance", T::Integer, "with color: largest difference per channel, 0..255 (default 25; thin marks such as "
                                    "a frame need more, ~40: fewer kills than the kill feed shows = raise it)"},
          {"min_area", T::Number, "with color: smallest new mark as a part of the whole picture (default 0.0001)"},
          {"pre", T::Time, "lead-in before the first kill / loud moment (default 2s)"},
          {"post", T::Time, "lead-out after the last one (default 1s)"},
          {"join", T::Time, "kills at most this far apart are one candidate (default 4s; loud moments without a kill feed: 2s)"},
          {"threshold_db", T::Number, "loud moments: sample peak in dBFS (default 12 dB above the file's median)"},
          {"audio_stream", T::Integer, "which audio stream, 0 = first"},
          {"top", T::Integer, "only the best N candidates ('best' with several files)"},
          {"project", T::Path, "count frames in this project's frame rate (recommended before 'keep')"}},
         cmdHighlights},
        {"transcribe", "Speech to text with whisper (offline; install it with `extensions`). Returns subtitle-sized "
                       "'segments' with times and, with words, every word's time (to cut out words or filler). "
                       "Source: a media file or a project (its timeline sound, times in timeline frames).",
         {{"source", T::Path, "media file or project (.schneidi)", true, true},
          {"project", T::Path, "for a media file: count frames in this project's frame rate"},
          {"language", T::String, "spoken language: de, en, fr, … (default auto = detect)"},
          {"model", T::String, "turbo, small, base, tiny or a model file (default: best installed)"},
          {"words", T::Boolean, "also return every word with its time"},
          {"music", T::Boolean, "sung words and shouts in music (a song, a chant): without voice detection, which "
                                "hears no speech there; slower, may hear words a bit wrong (\"cracker\" for \"Kraken\")"},
          {"srt", T::Path, "also write the segments as an SRT file"},
          {"max_chars", T::Integer, "longest segment in characters (default 42, one subtitle line)"},
          {"audio_stream", T::Integer, "for a media file: which audio stream, 0 = first"},
          {"from", T::Time, "for a project: start of the range"},
          {"to", T::Time, "for a project: end of the range"},
          {"timeline", T::String, "for a project: which timeline (id or name; default: the open one)"},
          {"tracks", T::String, "for a project: only these audio tracks, e.g. \"A3\", \"A2,A3\" or by name \"Discord,Mikrofon\" "
                                "(track or audio stream name; the voice without music; muted ones too; default: what is audible)"},
          {"apply", T::Boolean, "for a project: also put the segments onto the timeline as a new subtitle track "
                                "(live in the app: one undo step)"},
          {"name", T::String, "apply: name of the new subtitle track"},
          {"threads", T::Integer, "processor threads (default: the app setting Performance, else half of them)"}},
         cmdTranscribe},
        {"loudness", "Measure loudness like the Mixer's meter (EBU R128 / BS.1770): integrated LUFS, loudness range, "
                     "true peak. Source: a media file, a project's mix (as exported) or single clips of a project. "
                     "Common targets: -14 LUFS / -1 dBTP (streaming), -23 LUFS (EBU broadcast).",
         {{"source", T::Path, "media file or project (.schneidi)", true, true},
          {"timeline", T::String, "for a project: which timeline (id or name; default: the open one)"},
          {"from", T::Time, "for a project: start of the range"},
          {"to", T::Time, "for a project: end of the range"},
          {"tracks", T::String, "for a project: only these audio tracks, e.g. \"A1,A2\" or by name \"Mikrofon\" (track or audio stream name)"},
          {"clips", T::Times, "for a project: measure these clips (ids) one by one instead of the mix"},
          {"audio_stream", T::Integer, "for a media file: which audio stream, 0 = first"}},
         cmdLoudness},
        {"check", "Check before export or before handing a video over: what typically goes wrong. A project "
                  "(before rendering): missing media files, frames without any picture (a gap = black), subtitles too "
                  "short/too fast to read or past the end, recordings with stutter in the range a clip uses "
                  "(stutter: jumps in the time stamps, see `probe` gaps), and the sound of the mix as exported: no "
                  "sound, clipping, true peak above -1 dBTP, loudness off the target, silence at the end, and "
                  "under_music: a track with events more than 15 LU under the music while its events play (music = "
                  "a track named music/song/BGM, or one of sound files only that plays without pauses; events = the "
                  "sound of a video recording, or a track named voice/mic/game; fix: raise that track or `duck` the "
                  "music). Vertical videos (9:16): safe_area / subtitle_safe_area = text where TikTok, Reels and "
                  "Shorts put their caption and buttons (bottom 20 %), their button column (right 12 %, from 35 % of "
                  "the height down) or their top bar (top 8 %), or outside the picture; slow_hook (info, videos up to "
                  "3 min) = hardly any motion in the first second. A rendered file (after rendering): black "
                  "stretches, frozen picture, no sound, the same sound checks. 'issues' (errors first, each with check "
                  "id, message, time range and 'clip' when it is about one), counts of 'errors'/'warnings' and the "
                  "measured 'sound'. Fix errors before handing over; warnings may be intended (a fade to black, a "
                  "pause).",
         {{"source", T::Path, "project (.schneidi) or rendered media file", true, true},
          {"timeline", T::String, "for a project: which timeline (id or name; default: the open one)"},
          {"from", T::Time, "for a project: start of the range to check (default: start)"},
          {"to", T::Time, "for a project: end of the range (default: end of the timeline)"},
          {"target_lufs", T::Number, "loudness target (default -14 for web/streaming; -23 broadcast, -16 podcasts)"},
          {"sound", T::Boolean, "measure the sound (default true; false = no mixing: no sound and no under_music "
                                "checks)"}},
         cmdCheck},
        {"scopes", "Video scopes of one frame like the Color page (waveform, RGB parade, vectorscope, histogram) as "
                   "one image, plus numbers to judge exposure and color: luma spread (10-bit, 0..1023), clipped "
                   "black/white in %, RGB means and color cast in % (+ = too much of that channel), saturation.",
         {{"source", T::Path, "media file or project (.schneidi)", true, true},
          {"at", T::Time, "frame to look at (default 0)"},
          {"types", T::String, "comma list: waveform, parade, vectorscope, histogram (default all)"},
          {"out", T::Path, "image file to write (the MCP tool shows the image anyway)"},
          {"timeline", T::String, "for a project: which timeline (id or name; default: the open one)"},
          {"project", T::Path, "for a media file: count frames in this project's frame rate"}},
         cmdScopes},
        {"extensions", "Optional downloads (not part of schneidi): whisper speech recognition and its models. "
                       "Without arguments: list with sizes and what is installed.",
         {{"install", T::String, "ids to download and install, e.g. whisper,small (model sizes: turbo 574 MB, "
                                 "small 190 MB, base 60 MB, tiny 32 MB)"},
          {"remove", T::String, "ids to delete again"},
          {"reinstall", T::Boolean, "download again even if installed"}},
         cmdExtensions},
        {"effects", "Video and audio effects for the 'effect' edit operation: all ids with their kind and parameter "
                    "names, or one effect with types, ranges and defaults. Also lists LUTs (op 'color' lut), transition types and "
                    "luma images (op 'transition'), effect presets (op 'effect' preset) and accent presets (op "
                    "'accent': zoom punch, flash, shake on kills, beats, drops).",
         {{"effect", T::String, "only this effect, with parameter details", false, true}}, cmdEffects},
        {"sync", "Line up recordings of the same event (camera angles, player POVs) by their sound: the offset "
                 "of each file to the first one (the reference), found by shared sounds like a round start or "
                 "announcements. Files that do not overlap the reference are matched over the others. A file without "
                 "a clear match is not found (no guessing). The 'multicam' edit op does this itself; use this to check "
                 "first. Takes a while for long files (decodes the sound).",
         {{"files", T::Paths, "media files, the first is the reference (at least two)", true, true},
          {"stream", T::Integer, "audio stream used, 0 = first (default)"},
          {"max_offset_s", T::Number, "largest offset searched in seconds (default: any)"},
          {"project", T::Path, "count offset_frames in this project's frame rate (default: the reference file's)"}},
         cmdSync},
        {"frames", "Still images of a media file or a project timeline at given times, as files or one contact "
                   "sheet with timecodes (lets you look at the material). Several files at once with 'sources', a "
                   "detail of the picture (HUD, kill feed, score …) with 'crop'.",
         {{"source", T::Path, "media file or project (.schneidi)", false, true},
          {"sources", T::Sources, "several media files (on a sheet labelled 1, 2 … in this order); an entry may be an "
                                  "object {file, crop?, at?, every?, count?, from?, to?} with its own region/times"},
          {"at", T::Times, "times to show"},
          {"every", T::Time, "one frame every … (e.g. 10s)"},
          {"count", T::Integer, "this many frames spread evenly"},
          {"from", T::Time, "every/count: start of the range (default: start)"},
          {"to", T::Time, "every/count: end of the range (default: end)"},
          {"crop", T::String, "only this region \"x,y,w,h\" in pixels of the full picture, or fractions 0..1 "
                              "(\"0.75,0.05,0.25,0.3\" = top right); shown at its own size up to 'width'"},
          {"out", T::Path, "folder for the images (with sheet: the image file, .jpg or .png)"},
          {"sheet", T::Boolean, "one contact sheet image instead of single images"},
          {"columns", T::Integer, "sheet: number of columns (default: 2–4 depending on the count)"},
          {"tile_width", T::Integer, "width of every image/tile in pixels (instead of 'width'; e.g. 320 for many tiles, "
                                     "800 to read small HUD text)"},
          {"width", T::Integer, "longer edge of each image in pixels, on a sheet of each tile (default 640, MCP 480)"},
          {"project", T::Path, "for a media file: count frames in this project's frame rate"},
          {"timeline", T::String, "for a project: which timeline (id or name; default: the open one)"}},
         cmdFrames},
    };
    return list;
}

bool runsInApp(const Command& cmd, const QJsonObject& args)
{
    if (cmd.name == "app" || cmd.name == "import" || cmd.name == "restore") return true;
    // A `watch` pass or setting folders up changes the project; follow only starts passes (each one goes there)
    if (cmd.name == "watch") return !args["list"].toBool() && !args["follow"].toBool();
    return cmd.name == "edit" && !args["dry_run"].toBool();
}

QJsonObject execute(const Command& cmd, const QJsonObject& args0, Context& ctx)
{
    QJsonObject args = args0;
    if (cmd.name == "app" && args["project"].toString().isEmpty()) args["project"] = QString(Live::AppProject);
    if (runsInApp(cmd, args) && !Live::host())
        if (Live::Connection* c = Live::connectionFor(args["project"].toString())) {
            if (cmd.name == "edit") args = withSoundSync(args, ctx); // decode here, not in the app
            QJsonObject result = Live::run(c, cmd.name, args);
            result["live"] = true;
            return result;
        }
    // Analyses, render … while an AI is connected to the app: the app shows that it works (cli/Live.h)
    Live::Activity activity(cmd.name, args, ctx);
    try {
        QJsonObject result = cmd.run(args, ctx);
        activity.finished(result);
        return result;
    } catch (const Error& e) {
        activity.failed(e);
        throw;
    }
}

const Command* find(const QString& name)
{
    for (const Command& c : commands())
        if (c.name == name) return &c;
    return nullptr;
}

QJsonObject inputSchema(const Command& cmd)
{
    QJsonObject props;
    QJsonArray required;
    for (const Param& p : cmd.params) {
        QJsonObject s{{"description", p.help}};
        switch (p.type) {
        case ParamType::String:
        case ParamType::Path: s["type"] = "string"; break;
        case ParamType::Integer: s["type"] = "integer"; break;
        case ParamType::Number: s["type"] = "number"; break;
        case ParamType::Boolean: s["type"] = "boolean"; break;
        case ParamType::Time: s["type"] = QJsonArray{"integer", "string"}; break;
        case ParamType::Times: s["type"] = "array"; s["items"] = QJsonObject{{"type", QJsonArray{"integer", "string"}}}; break;
        case ParamType::Paths: s["type"] = "array"; s["items"] = QJsonObject{{"type", "string"}}; break;
        case ParamType::Sources: s["type"] = "array"; s["items"] = QJsonObject{{"type", QJsonArray{"string", "object"}}}; break;
        case ParamType::Json: s["type"] = "array"; s["items"] = QJsonObject{{"type", "object"}}; break;
        }
        props[p.name] = s;
        if (p.required) required << p.name;
    }
    QJsonObject o{{"type", "object"}, {"properties", props}};
    if (!required.isEmpty()) o["required"] = required;
    return o;
}

namespace {

// Names an AI often uses for a parameter that has another name in this command (the analysis commands take `file`,
// the ones that also read projects `source`; files are written to `out`): taken as that parameter when the command
// has no parameter of the name, has the target and the call does not give the target as well
const QHash<QString, QStringList>& parameterAliases()
{
    static const QHash<QString, QStringList> aliases = {
        {"file", {"source"}},
        {"source", {"file"}},
        {"files", {"sources"}},
        {"sources", {"files"}},
        {"path", {"source", "file", "project"}},
        {"input", {"source", "file"}},
        {"project", {"source"}},
        {"output", {"out"}},
        {"output_file", {"out"}},
        {"out_file", {"out"}},
        {"destination", {"out"}},
    };
    return aliases;
}

} // namespace

QJsonObject normalize(const Command& cmd, const QJsonObject& given)
{
    const auto has = [&cmd](const QString& name) {
        return std::any_of(cmd.params.begin(), cmd.params.end(), [&](const Param& p) { return p.name == name; });
    };
    QJsonObject raw = given;
    for (auto it = given.begin(); it != given.end(); ++it) {
        if (has(it.key())) continue;
        for (const QString& target : parameterAliases().value(it.key()))
            if (has(target) && !raw.contains(target)) {
                raw[target] = raw.take(it.key());
                break;
            }
    }
    QStringList names;
    for (const Param& p : cmd.params) names << p.name;
    for (auto it = raw.begin(); it != raw.end(); ++it)
        if (!has(it.key()))
            fail("BAD_ARGUMENT", QString("unknown parameter '%1' for %2 (parameters: %3)")
                                     .arg(it.key(), cmd.name, names.join(", ")));
    QJsonObject out;
    for (const Param& p : cmd.params) {
        if (!raw.contains(p.name) || raw[p.name].isNull()) {
            if (p.required) fail("BAD_ARGUMENT", QString("missing parameter '%1' for %2").arg(p.name, cmd.name));
            continue;
        }
        const QJsonValue v = raw[p.name];
        const QString str = v.isString() ? v.toString() : QString();
        const auto list = [&]() {
            if (v.isArray()) return v.toArray();
            if (str.trimmed().startsWith('[')) return QJsonDocument::fromJson(str.toUtf8()).array();
            QJsonArray arr;
            for (const QString& x : str.split(',', Qt::SkipEmptyParts)) arr << x.trimmed();
            return arr;
        };
        bool ok = true;
        switch (p.type) {
        case ParamType::String: out[p.name] = v.isString() ? str : v.toVariant().toString(); break;
        case ParamType::Path:
            out[p.name] = absolute(str);
            ok = v.isString() && !str.isEmpty();
            break;
        case ParamType::Integer:
            if (v.isDouble()) out[p.name] = v.toInt();
            else out[p.name] = str.toInt(&ok);
            break;
        case ParamType::Number:
            if (v.isDouble()) out[p.name] = v.toDouble();
            else out[p.name] = QLocale::c().toDouble(str, &ok);
            break;
        case ParamType::Boolean:
            if (v.isBool()) out[p.name] = v.toBool();
            else if (str == "true" || str == "1" || str == "yes") out[p.name] = true;
            else if (str == "false" || str == "0" || str == "no") out[p.name] = false;
            else ok = false;
            break;
        case ParamType::Time: out[p.name] = v; ok = v.isDouble() || v.isString(); break;
        case ParamType::Times: out[p.name] = list(); break;
        case ParamType::Paths: {
            QJsonArray arr;
            for (const QJsonValue& x : list()) arr << absolute(x.toString());
            out[p.name] = arr;
            break;
        }
        case ParamType::Sources: {
            QJsonArray arr;
            for (const QJsonValue& x : list()) {
                if (!x.isObject()) {
                    arr << absolute(x.toString());
                    continue;
                }
                QJsonObject o = x.toObject();
                if (!o["file"].isString() || o["file"].toString().isEmpty())
                    fail("BAD_ARGUMENT", QString("%1: every object needs 'file'").arg(p.name));
                o["file"] = absolute(o["file"].toString());
                arr << o;
            }
            out[p.name] = arr;
            break;
        }
        case ParamType::Json:
            if (v.isString()) {
                QJsonParseError err;
                const QJsonDocument doc = QJsonDocument::fromJson(str.toUtf8(), &err);
                if (err.error != QJsonParseError::NoError)
                    fail("BAD_ARGUMENT", QString("%1: invalid JSON (%2)").arg(p.name, err.errorString()));
                out[p.name] = doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object());
            } else {
                out[p.name] = v;
            }
            break;
        }
        if (!ok) fail("BAD_ARGUMENT", QString("parameter '%1' has a wrong value: %2").arg(p.name, v.toVariant().toString()));
    }
    return out;
}

namespace {

QJsonObject cmdGuide(const QJsonObject& a, Context&)
{
    const QString topic = a["topic"].toString().trimmed().toLower().remove(".md").remove("references/");
    const QString text = Guide::text(topic);
    if (text.isEmpty()) fail("NOT_FOUND", "no guide topic '" + topic + "' (topics: " + Guide::topics().join(", ") + ")");
    if (!topic.isEmpty()) return {{"topic", topic}, {"guide", text}};
    QJsonArray topics;
    for (const QString& t : Guide::topics()) topics << QJsonObject{{"topic", t}, {"title", Guide::title(t)}};
    return {{"guide", text},
            {"topics", topics},
            {"note", "references/<topic>.md in the guide = guide with that topic; command and op details: help"}};
}

QJsonObject cmdHelp(const QJsonObject& a, Context&)
{
    // The servers are not commands of the table: their options
    const QJsonObject servers = Serve::help();
    if (servers.contains(a["command"].toString()))
        return {{"program", "schneidi-cli"}, {"version", QCoreApplication::applicationVersion()},
                {a["command"].toString(), servers[a["command"].toString()]}};
    QJsonArray list;
    for (const Command& c : commands()) {
        if (a.contains("command") && c.name != a["command"].toString()) continue;
        QJsonArray positional;
        for (const Param& p : c.params)
            if (p.positional) positional << p.name;
        QJsonObject o{{"name", c.name}, {"summary", c.summary}, {"positional", positional}, {"params", inputSchema(c)}};
        if (c.name == "edit" && a.contains("command")) {
            QJsonArray ops = editOpsHelp();
            if (!a.contains("op")) {
                // What each op is for; the details of all of them are ~40 KB, an AI asks for the ones it uses
                for (QJsonValueRef v : ops) {
                    QJsonObject op = v.toObject();
                    op.remove("details");
                    v = op;
                }
                o["details"] = "help with op:'<name>' (or a comma list, \"all\" = every op) shows the details of ops";
            } else if (a["op"].toString() != "all") {
                // Only the asked ops (the full list is long); the parameter list of `edit` itself is left out too
                const QStringList wanted = a["op"].toString().split(',', Qt::SkipEmptyParts);
                QJsonArray only;
                QStringList names;
                for (const QJsonValue& v : ops) {
                    names << v["op"].toString();
                    for (const QString& w : wanted)
                        if (w.trimmed() == v["op"].toString()) only << v;
                }
                if (only.isEmpty()) fail("NOT_FOUND", "no op " + a["op"].toString() + " (ops: " + names.join(", ") + ")");
                ops = only;
                o.remove("params");
            }
            o["ops"] = ops;
        }
        list << o;
    }
    if (list.isEmpty()) fail("NOT_FOUND", "no command " + a["command"].toString());
    QJsonObject o{{"program", "schneidi-cli"}, {"version", QCoreApplication::applicationVersion()},
                  {"api_version", ApiVersion}, {"commands", list}};
    if (a.contains("command")) return o; // one command (or op): only that, an AI asks for it in the middle of work
    o["usage"] = "schneidi-cli <command> [positional…] [--param value]; output is JSON on stdout, exit code 0 = ok, 1 = "
                 "error, 2 = usage error. `schneidi-cli mcp` runs an MCP server on stdio, `schneidi-cli serve` one over "
                 "HTTP (see 'servers'); `schneidi-cli setup claude` connects Claude Code and Claude Desktop to it. "
                 "--verbose: progress notes and engine messages on stderr.";
    o["servers"] = servers;
    return o;
}

} // namespace

} // namespace Cli