// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// What the running app offers to the live connection (schneidi-cli / MCP editing the project open in the app).
// MainWindow implements it; the server (cli/Live.cpp) only talks to this interface, so the app code does not
// depend on the command layer. All calls happen in the GUI thread.

#include "core/Types.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

class Project;
class Selection;

// What one AI command did inside the app (cli/Live.cpp compares the clips before and after it): the app lights the
// clips up in the timeline and lists the step in its AI log
struct LiveChange {
    QString command;      // "edit", "app", "import", "restore"
    QString step;         // what it did: the edit ops ("split, delete"), the app action ("undo") …
    QVector<int> added;   // new clips (ids, all timelines)
    QVector<int> changed; // clips that are still there but differ
    struct Removed {      // deleted clips: where they were before the command
        int id = 0;
        int sequence = 0; // timeline (Project::currentSequence id)
        TrackRef track;
        int start = 0, end = 0; // timeline frames, end exclusive
        QString name;
    };
    QVector<Removed> removed;
    QString error; // the command failed and changed nothing: why
    bool ownChat = false; // the app's own AI chat did it (not an AI from outside)
};

// Work of an AI connected to the app that runs outside it (analyses, render, transcribe … in schneidi-cli), so the
// user sees it working before (or without) any edit
struct LiveActivity {
    enum class State { Started, Progress, Finished, Failed };
    int id = 0; // the same for all messages of one job, unique in the app (the server numbers the clients' jobs)
    State state = State::Started;
    QString tool;        // command: "silence", "render" …
    QString target;      // file or project it works on (may be empty)
    int percent = -1;    // -1 = unknown
    QJsonObject summary; // Finished: {"count": 18} (things found), {"lufs": -14.2} or {"file": path written}
    QString error;       // Failed
};

class LiveHost {
public:
    virtual ~LiveHost() = default;

    virtual Project* liveProject() = 0;
    virtual QString liveProjectPath() const = 0; // empty = never saved
    virtual Selection* liveSelection() = 0;      // the user's selection in the timeline
    virtual int livePlayhead() const = 0;
    virtual bool livePlaying() const = 0;
    virtual void liveSeek(int frame) = 0;
    virtual void livePlay(bool play) = 0;
    // Saves to the project's file, or to `path` (a project never saved; the caller checks it is a new file)
    virtual bool liveSave(const QString& path, QString* error) = 0;
    // Proxy media: generate (in the background), and the state per file ("ready", "pending" with percent, "none")
    virtual void liveGenerateProxies(const QStringList& files) = 0;
    virtual QJsonObject liveProxyState(const QString& file) const = 0;
    // Proxy file being written right now (empty = none): cleaning up the proxy folder leaves it alone
    virtual QString liveProxyWriting() const { return {}; }

    // Shown to the user: an AI session is connected (client = its name) or not anymore
    virtual void liveConnectionChanged(bool connected, const QString& client) = 0;
    // An AI command ran in the app (also when it failed); not for reading ones
    virtual void liveChanged(const LiveChange& change) = 0;
    // The connected AI works outside the app (start, progress, end of a job)
    virtual void liveActivity(const LiveActivity& activity) = 0;
    // A command of an AI runs in the app right now (before and after it; ownChat = the app's own AI chat sent it):
    // everything the undo history gets meanwhile comes from that command
    virtual void liveCommandRunning(bool running, bool ownChat)
    {
        Q_UNUSED(running);
        Q_UNUSED(ownChat);
    }
};
