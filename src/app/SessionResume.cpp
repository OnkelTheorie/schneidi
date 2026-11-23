// SPDX-License-Identifier: AGPL-3.0-only
#include "app/SessionResume.h"

#include "core/AppPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>

namespace SessionResume {

namespace {
constexpr const char* kStateName = "resume.json";
constexpr const char* kDataName = "resume.schneidi";

QString dataFileOf(const QString& stateFile)
{
    return QFileInfo(stateFile).dir().filePath(kDataName);
}
} // namespace

QString folder()
{
    return QDir(AppPaths::writable(QStandardPaths::AppDataLocation)).filePath("resume");
}

QString write(const State& state, const ProjectData& data, QString* error)
{
    const QDir dir(folder());
    if (!QDir().mkpath(dir.path())) {
        if (error) *error = QStringLiteral("cannot create %1").arg(dir.path());
        return {};
    }
    const QString stateFile = dir.filePath(kStateName);
    if (!ProjectFile::save(data, dataFileOf(stateFile), error)) return {};
    QJsonArray selection;
    QList<int> ids(state.selection.begin(), state.selection.end());
    std::sort(ids.begin(), ids.end());
    for (int id : ids) selection << id;
    const QJsonObject o{{"projectPath", state.projectPath},
                        {"saveSuggestion", state.saveSuggestion},
                        {"modified", state.modified},
                        {"page", state.page},
                        {"selection", selection}};
    QSaveFile f(stateFile);
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(o).toJson()) < 0 || !f.commit()) {
        if (error) *error = f.errorString();
        QFile::remove(dataFileOf(stateFile));
        return {};
    }
    return stateFile;
}

bool read(const QString& stateFile, State* state, ProjectData* data, QString* error)
{
    QFile f(stateFile);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o.isEmpty()) {
        if (error) *error = QStringLiteral("not a resume file: %1").arg(stateFile);
        return false;
    }
    if (!ProjectFile::load(dataFileOf(stateFile), data, error)) return false;
    State s;
    s.projectPath = o.value("projectPath").toString();
    s.saveSuggestion = o.value("saveSuggestion").toString();
    s.modified = o.value("modified").toBool();
    s.page = o.value("page").toString();
    for (const QJsonValue& v : o.value("selection").toArray()) s.selection.insert(v.toInt());
    *state = s;
    return true;
}

void remove(const QString& stateFile)
{
    QFile::remove(dataFileOf(stateFile));
    QFile::remove(stateFile);
}

} // namespace SessionResume
