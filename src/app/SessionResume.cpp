// SPDX-License-Identifier: AGPL-3.0-only
#include "app/SessionResume.h"

#include "core/AppPaths.h"

#include <QCoreApplication>
#include <QDateTime>
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
// resume-<process id>-<ms since epoch>.json + .schneidi beside it
QString dataFileOf(const QString& stateFile)
{
    return stateFile.chopped(5) + ".schneidi";
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
    const QString stateFile = dir.filePath(QString("resume-%1-%2.json")
                                               .arg(QCoreApplication::applicationPid())
                                               .arg(QDateTime::currentMSecsSinceEpoch()));
    if (!ProjectFile::save(data, dataFileOf(stateFile), error)) return {};
    QJsonArray selection;
    QList<int> ids(state.selection.begin(), state.selection.end());
    std::sort(ids.begin(), ids.end());
    for (int id : ids) selection << id;
    const QJsonObject o{{"projectPath", state.projectPath},
                        {"saveSuggestion", state.saveSuggestion},
                        {"modified", state.modified},
                        {"page", state.page},
                        {"selection", selection},
                        {"autosave", state.autosave},
                        {"autosaveKey", state.autosaveKey}};
    QSaveFile f(stateFile);
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(o).toJson()) < 0 || !f.commit()) {
        if (error) *error = f.errorString();
        QFile::remove(dataFileOf(stateFile));
        return {};
    }
    return stateFile;
}

bool isResumeFile(const QString& stateFile)
{
    const QFileInfo f(stateFile);
    return f.fileName().startsWith("resume-") && f.suffix() == "json" && f.exists()
           && f.absoluteDir().canonicalPath() == QDir(folder()).canonicalPath();
}

QStringList leftovers(int minAge)
{
    QStringList out;
    const QDateTime now = QDateTime::currentDateTime();
    for (const QFileInfo& f : QDir(folder()).entryInfoList({"resume-*.json"}, QDir::Files, QDir::Time | QDir::Reversed))
        if (f.lastModified().secsTo(now) >= minAge) out << f.absoluteFilePath();
    return out;
}

bool read(const QString& stateFile, State* state, ProjectData* data, QString* error)
{
    if (!isResumeFile(stateFile)) {
        if (error) *error = QStringLiteral("not a resume file of %1: %2").arg(folder(), stateFile);
        return false;
    }
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
    s.autosave = o.value("autosave").toString();
    s.autosaveKey = o.value("autosaveKey").toString();
    *state = s;
    return true;
}

void remove(const QString& stateFile)
{
    if (!isResumeFile(stateFile)) return;
    QFile::remove(dataFileOf(stateFile));
    QFile::remove(stateFile);
}

} // namespace SessionResume
