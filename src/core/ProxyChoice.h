// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Which files a "Generate Proxy Media" entry means (Playback menu, timeline and Media Pool right-click): the video
// files (no stills, no sound-only files, only those on disk) of the selected clips, of every clip of the open
// timeline or of the whole Media Pool. Compound and multicam clips count with the files inside them. Pure function
// on the model; the queue itself (and skipping files that have a proxy) is engine/ProxyManager.

#include <QSet>
#include <QStringList>

class Project;

namespace ProxyChoice {

enum class Scope {
    Selected, // clips of the open timeline with these ids (video or audio part, the file counts once)
    Timeline, // every clip of the open timeline
    Pool,     // every file in the Media Pool
};

// In the order found (track by track, then by time; the pool's order), each file once
QStringList files(const Project& project, Scope scope, const QSet<int>& clipIds = {});

// Menu text of a "Generate" entry with why it is greyed out: `files` video files meant, `missing` of them without a
// proxy (to be queued), `pending` being made. "Generate Proxy Media (3)", "… (already there)", "… (no video files)"
QString label(const QString& action, int files, int missing, int pending);

} // namespace ProxyChoice
