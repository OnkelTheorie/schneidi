// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// A restart that comes back where it was (a new interface size needs one: MainWindow::restartRestoring). The project as
// it is - unsaved changes too - goes into a project file of its own, a few lines beside it say what else to bring back
// (its own file name, unsaved or not, page, selection); playhead and every timeline's zoom/scroll are in the project
// data (ProjectData::playhead/views). The new process gets the state file (`--resume <file>`, main.cpp), reads it and
// removes both files. Nothing of the user's own project file is touched.

#include "core/ProjectFile.h"

#include <QSet>
#include <QString>

namespace SessionResume {

struct State {
    QString projectPath;    // the project's own file (empty: never saved)
    QString saveSuggestion; // where Save As would suggest (never saved)
    bool modified = false;  // unsaved changes (the new window shows the project as modified)
    QString page;           // MainWindow::pageId
    QSet<int> selection;    // selected clip ids
};

// The folder of the two files (in the app's data folder; a test run's own)
QString folder();
// Writes the project data and the state; returns the state file (empty = not written, `error` says why)
QString write(const State& state, const ProjectData& data, QString* error = nullptr);
// Reads both (false = not readable, `error` says why)
bool read(const QString& stateFile, State* state, ProjectData* data, QString* error = nullptr);
// Both files gone (after reading them)
void remove(const QString& stateFile);

} // namespace SessionResume
