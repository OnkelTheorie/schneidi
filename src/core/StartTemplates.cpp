#include "core/StartTemplates.h"

#include "core/I18n.h"
#include "core/Project.h"
#include "core/RenderJob.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>

namespace {

constexpr const char* kDefaultKey = "templates/default";
constexpr const char* kAskKey = "templates/askOnNew";
constexpr const char* kBuiltInIds[] = {StartTemplates::NeutralId, "game", "podcast", "vertical"};

StartTemplate make(const char* id, const char* name, const char* description)
{
    StartTemplate t;
    t.id = QString::fromLatin1(id);
    t.name = QString::fromUtf8(name);
    t.description = QString::fromUtf8(description);
    t.builtIn = true;
    t.data.timeline = emptyTimeline();
    return t;
}

Track track(TrackKind kind, const QString& name = {}, const QString& color = {})
{
    Track t;
    t.kind = kind;
    t.name = name;
    t.color = color;
    return t;
}

// A template file: the project file of the new project without what a template never sets (media, ids, playhead),
// plus its name and description
QByteArray toJson(const StartTemplate& t)
{
    QJsonObject root = QJsonDocument::fromJson(ProjectFile::toJson(StartTemplates::apply(t), t.path)).object();
    for (const char* key : {"media", "bins", "playhead", "lastClipId", "lastLinkId"}) root.remove(key);
    QJsonObject meta{{"name", t.name}};
    if (!t.description.isEmpty()) meta["description"] = t.description;
    root["template"] = meta;
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool write(const StartTemplate& t, QString* error)
{
    QDir().mkpath(QFileInfo(t.path).absolutePath());
    QSaveFile f(t.path);
    if (!f.open(QIODevice::WriteOnly) || f.write(toJson(t)) < 0 || !f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

std::optional<StartTemplate> ownById(const QString& id)
{
    for (const StartTemplate& t : StartTemplates::own())
        if (t.id == id) return t;
    return std::nullopt;
}

QString decibels(double db) { return QString::number(db, 'f', 1).replace('.', I18n::decimalPoint()); }

} // namespace

QString StartTemplate::displayName() const { return builtIn ? T(name) : name; }

QString StartTemplate::displayDescription() const { return builtIn && !description.isEmpty() ? T(description) : description; }

namespace StartTemplates {

QVector<StartTemplate> builtIn()
{
    QVector<StartTemplate> list;
    list << make(NeutralId, N_("Neutral"),
                 N_("Ein neues Projekt wie immer: zwei Video- und zwei Audiospuren, nichts stumm; beim ersten Clip "
                    "bietet schneidi dessen Format an."));

    // OBS and other recorders write the Mix plus single streams: the Mix muted, so nothing plays twice. On top of the
    // user's own import presets (their rules per stream stay); the limiter catches shots and explosions.
    StartTemplate game = make("game", N_("Spielaufnahme"),
                              N_("Für Aufnahmen mit mehreren Ton-Streams (z. B. OBS): Der Mix-Stream kommt stumm in "
                                 "die Timeline, wenn es Einzelspuren gibt – so ist nichts doppelt zu hören. Limiter "
                                 "auf dem Master gegen Übersteuern."));
    StreamPresets presets = StreamPresets::load();
    presets.muteMix = true;
    game.data.importPresets = presets;
    game.data.timeline.masterLimiter = true;
    list << game;

    // Names `check` and the AI understand (voice and music tracks: under_music, duck) in every language
    StartTemplate podcast = make("podcast", N_("Podcast / Interview"),
                                 N_("Benannte Audiospuren für zwei Stimmen und die Musik (so merkt die Prüfung vor dem "
                                    "Export, wenn die Musik eine Stimme übertönt). Limiter auf dem Master."));
    podcast.data.timeline.audio = {track(TrackKind::Audio, T("Stimme 1")), track(TrackKind::Audio, T("Stimme 2")),
                                   track(TrackKind::Audio, T("Musik"), "purple")};
    podcast.data.timeline.masterLimiter = true;
    list << podcast;

    StartTemplate vertical = make("vertical", N_("Hochformat 9:16"),
                                  N_("1080 × 1920 für Shorts, Reels und TikTok. Die Auflösung bleibt beim ersten Clip, "
                                     "die Framerate übernimmt schneidi von ihm."));
    vertical.data.format.width = 1080;
    vertical.data.format.height = 1920;
    vertical.data.fixedResolution = true;
    list << vertical;
    return list;
}

QString dir() { return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("templates"); }

QVector<StartTemplate> own()
{
    QVector<StartTemplate> list;
    for (const QFileInfo& fi : QDir(dir()).entryInfoList({"*.json"}, QDir::Files, QDir::Name))
        if (std::optional<StartTemplate> t = load(fi.absoluteFilePath())) list << *t;
    std::stable_sort(list.begin(), list.end(),
                     [](const StartTemplate& a, const StartTemplate& b) { return I18n::compareNames(a.name, b.name) < 0; });
    return list;
}

QVector<StartTemplate> all() { return builtIn() + own(); }

std::optional<StartTemplate> find(const QString& idOrName)
{
    const QString key = idOrName.trimmed();
    if (key.isEmpty()) return std::nullopt;
    const auto same = [&key](const QString& s) { return s.compare(key, Qt::CaseInsensitive) == 0; };
    const QVector<StartTemplate> list = all();
    for (const StartTemplate& t : list)
        if (same(t.id)) return t;
    for (const StartTemplate& t : list)
        if (same(t.name) || same(t.displayName())) return t;
    return std::nullopt;
}

std::optional<StartTemplate> load(const QString& path, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return std::nullopt;
    }
    const QByteArray json = f.readAll();
    ProjectData d;
    if (!ProjectFile::fromJson(json, path, &d, error)) return std::nullopt;
    const QJsonObject meta = QJsonDocument::fromJson(json).object().value("template").toObject();
    const QFileInfo fi(path);
    StartTemplate t = fromProject(d, meta.value("name").toString(), meta.value("description").toString());
    if (t.name.isEmpty()) t.name = fi.completeBaseName(); // a project, or a file written by hand
    t.id = fi.completeBaseName();
    t.path = fi.absoluteFilePath();
    return t;
}

QString defaultId() { return QSettings().value(kDefaultKey, QString::fromLatin1(NeutralId)).toString(); }

void setDefaultId(const QString& id)
{
    if (id.isEmpty() || id == QLatin1String(NeutralId)) QSettings().remove(kDefaultKey);
    else QSettings().setValue(kDefaultKey, id);
}

StartTemplate defaultTemplate()
{
    const QString id = defaultId();
    QVector<StartTemplate> list = builtIn();
    if (id != QLatin1String(NeutralId)) list += own();
    for (const StartTemplate& t : list)
        if (t.id == id) return t;
    return list.first(); // deleted or never there: Neutral
}

bool askOnNew() { return QSettings().value(kAskKey, false).toBool(); }

void setAskOnNew(bool on)
{
    if (on) QSettings().setValue(kAskKey, true);
    else QSettings().remove(kAskKey);
}

StartTemplate fromProject(const ProjectData& d, const QString& name, const QString& description)
{
    StartTemplate t;
    t.name = name.trimmed();
    t.description = description.trimmed();
    ProjectData& n = t.data;
    n.format = d.format;
    n.fixedResolution = d.fixedResolution;
    n.fixedFrameRate = d.fixedFrameRate;
    n.importPresets = d.importPresets;
    Timeline& tl = n.timeline;
    tl.video = d.timeline.video;
    tl.audio = d.timeline.audio;
    tl.subtitles = d.timeline.subtitles;
    for (auto* tracks : {&tl.video, &tl.audio})
        for (Track& tr : *tracks) tr.clips.clear();
    for (SubtitleTrack& s : tl.subtitles) s.cues.clear();
    tl.masterVolumeDb = d.timeline.masterVolumeDb;
    tl.masterLimiter = d.timeline.masterLimiter;
    tl.masterLimiterDb = d.timeline.masterLimiterDb;
    return t;
}

ProjectData apply(const StartTemplate& t)
{
    // Only what a template sets (a file may carry more: a project used as template)
    ProjectData d = fromProject(t.data, {}).data;
    d.format.width = std::clamp(d.format.width, 16, 8192) & ~1; // even, as the export needs (yuv420p)
    d.format.height = std::clamp(d.format.height, 16, 8192) & ~1;
    // A timeline always has a video and an audio track (Project::load adds them too; here already, for the file)
    if (d.timeline.video.isEmpty()) d.timeline.video << track(TrackKind::Video);
    if (d.timeline.audio.isEmpty()) d.timeline.audio << track(TrackKind::Audio);
    return d;
}

QString idForName(const QString& name)
{
    static const QRegularExpression bad(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]"));
    QString id = name;
    id.replace(bad, QStringLiteral("_"));
    id = id.trimmed();
    while (id.startsWith('.')) id.remove(0, 1); // no hidden files
    if (id.isEmpty()) id = QStringLiteral("template");
    // The rules of render file names: no trailing dot or space (Windows drops them), no Windows device name ("Aux",
    // "NUL.v2" …), at most 200 characters
    id = renderFileBaseName(id);
    for (const char* builtInId : kBuiltInIds)
        if (id.compare(QLatin1String(builtInId), Qt::CaseInsensitive) == 0) id += QStringLiteral("-own");
    return id;
}

bool save(StartTemplate& t, QString* error)
{
    t.name = t.name.trimmed();
    if (t.name.isEmpty()) {
        if (error) *error = T("Die Vorlage braucht einen Namen.");
        return false;
    }
    t.id = idForName(t.name);
    t.path = QDir(dir()).filePath(t.id + ".json");
    t.builtIn = false;
    return write(t, error);
}

bool rename(const QString& id, const QString& name, QString* error)
{
    std::optional<StartTemplate> t = ownById(id);
    if (!t) {
        if (error) *error = T("Die Vorlage gibt es nicht mehr.");
        return false;
    }
    const QString clean = name.trimmed();
    if (clean.isEmpty()) {
        if (error) *error = T("Die Vorlage braucht einen Namen.");
        return false;
    }
    const QString newId = idForName(clean);
    const QString newPath = QDir(dir()).filePath(newId + ".json");
    // Another file of that name (only the case changes: the same file on Windows and macOS)
    if (newId.compare(id, Qt::CaseInsensitive) != 0 && QFileInfo::exists(newPath)) {
        if (error) *error = T("Eine Vorlage „%1“ gibt es schon.").arg(clean);
        return false;
    }
    const bool wasDefault = defaultId() == id;
    t->name = clean;
    if (!write(*t, error)) return false; // the new name inside, then the file
    if (newId != id && !QFile::rename(t->path, newPath)) {
        if (error) *error = T("%1 ließ sich nicht umbenennen.").arg(QDir::toNativeSeparators(t->path));
        return false;
    }
    if (wasDefault) setDefaultId(newId);
    return true;
}

bool remove(const QString& id)
{
    const std::optional<StartTemplate> t = ownById(id);
    if (!t || !QFile::remove(t->path)) return false;
    if (defaultId() == id) setDefaultId(NeutralId);
    return true;
}

QStringList summary(const StartTemplate& t)
{
    QStringList lines;
    const ProjectData& d = t.data;
    const QString size = resolutionLabel(d.format.width, d.format.height);
    const QString rate = d.format.rate.label();
    if (d.fixedResolution && d.fixedFrameRate) lines << T("Format: %1, %2 fps (bleibt)").arg(size, rate);
    else if (d.fixedResolution) lines << T("Format: %1 (bleibt), Framerate vom ersten Clip").arg(size);
    else if (d.fixedFrameRate) lines << T("Format: Auflösung vom ersten Clip, %1 fps (bleibt)").arg(rate);
    else lines << T("Format vom ersten Clip (bis dahin %1, %2 fps)").arg(size, rate);

    QStringList tracks;
    const auto describe = [&tracks](TrackKind kind, const QVector<Track>& list) {
        for (int i = 0; i < list.size(); ++i) {
            const Track& tr = list[i];
            QString s = trackShortName({kind, i});
            if (!tr.name.isEmpty()) s = T("%1 „%2“").arg(s, tr.name);
            QStringList states;
            if (tr.muted) states << T("stumm");
            if (tr.solo) states << T("solo");
            if (tr.hidden) states << T("ausgeblendet");
            if (tr.locked) states << T("gesperrt");
            if (!states.isEmpty()) s += QString(" (%1)").arg(states.join(", "));
            tracks << s;
        }
    };
    describe(TrackKind::Video, d.timeline.video);
    describe(TrackKind::Audio, d.timeline.audio);
    for (int i = 0; i < d.timeline.subtitles.size(); ++i)
        tracks << subtitleTrackDisplayName(d.timeline.subtitles[i], i);
    lines << T("Spuren: %1").arg(tracks.join(", "));

    QStringList more;
    if (d.timeline.masterLimiter) more << T("Limiter auf dem Master (%1 dB)").arg(decibels(d.timeline.masterLimiterDb));
    if (d.timeline.masterVolumeDb != 0) more << T("Master %1 dB").arg(decibels(d.timeline.masterVolumeDb));
    if (d.importPresets) {
        if (d.importPresets->muteMix) more << T("Mix-Stream stumm, wenn es Einzelspuren gibt");
        if (!d.importPresets->rules.isEmpty()) more << T("%1 Regeln für Ton-Streams").arg(d.importPresets->rules.size());
        if (*d.importPresets == StreamPresets{}) more << T("keine Import-Vorgaben (alle Streams wie in der Datei)");
    }
    if (!more.isEmpty()) lines << more.join(" · ");
    return lines;
}

} // namespace StartTemplates
