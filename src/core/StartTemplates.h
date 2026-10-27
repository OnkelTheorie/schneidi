#pragma once
// Start templates: how a new project looks (File › New Project, File › New Project from Template, schneidi-cli
// `new`). A template holds the project format (and whether the first clip may still change its resolution / frame
// rate, as it always could), the tracks of the first timeline without content (number, names, colors, mute, solo,
// lock, hidden, mixer, subtitle tracks, master) and optionally the project's own import presets for audio streams.
// "Neutral" is a new project as it always was and stays the default: the built-in templates are only offered, the
// user chooses one (Settings › Start Templates).
// Own templates are JSON files in AppDataLocation/templates, one per template: a project file without media and
// clips plus "template": {name, description}, so they follow the project format (ProjectFile reads them).

#include "core/ProjectFile.h"

#include <QString>
#include <QVector>
#include <optional>

struct StartTemplate {
    QString id;          // built-in: "neutral", "game", "podcast", "vertical"; own: its file name without ".json"
    QString name;        // built-in: German key (displayName() translates it); own: as the user named it
    QString description; // likewise
    bool builtIn = false;
    QString path;        // own: its file
    // The new project: format, fixedResolution/fixedFrameRate, the first timeline (tracks without clips, subtitle
    // tracks without text, master) and the own import presets; nothing else is set
    ProjectData data;

    QString displayName() const;
    QString displayDescription() const;
};

namespace StartTemplates {

inline constexpr const char* NeutralId = "neutral";

QVector<StartTemplate> builtIn(); // Neutral first; track names in the UI language
QVector<StartTemplate> own();     // the files in dir(), by name (unreadable files are left out)
QVector<StartTemplate> all();     // built-in, then own
QString dir();                    // AppDataLocation/templates

// By id (built-in ones first), then by name (also the translated one of a built-in), any case
std::optional<StartTemplate> find(const QString& idOrName);
// A template file (.json), or a project (.schneidi): its format, the open timeline's tracks, its import presets
std::optional<StartTemplate> load(const QString& path, QString* error = nullptr);

// Settings: the template new projects start with ("templates/default"; Neutral when not set or gone) and whether
// File › New Project asks which one (off by default)
QString defaultId();
void setDefaultId(const QString& id);
StartTemplate defaultTemplate();
bool askOnNew();
void setAskOnNew(bool on);

// A project as template: its format and fixed flags, the tracks of its open timeline without clips (subtitle tracks
// without text, no markers or in/out), master fader and limiter, its own import presets
StartTemplate fromProject(const ProjectData& d, const QString& name, const QString& description = {});
// Data for a new project from t (Project::load). Neutral gives exactly the project schneidi always started with.
ProjectData apply(const StartTemplate& t);

// File name (without .json) of an own template of this name: safe on every system, never a built-in id
QString idForName(const QString& name);
// Own templates: save under t.name (replaces one of the same id; sets id/path), rename (a default follows), delete
// (the default falls back to Neutral)
bool save(StartTemplate& t, QString* error = nullptr);
bool rename(const QString& id, const QString& name, QString* error = nullptr);
bool remove(const QString& id);

// For lists: format, tracks and what else the template sets, one line each (UI language)
QStringList summary(const StartTemplate& t);

} // namespace StartTemplates
