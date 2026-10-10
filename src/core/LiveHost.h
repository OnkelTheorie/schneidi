#pragma once
// What the running app offers to the live connection (schneidi-cli / MCP editing the project open in the app).
// MainWindow implements it; the server (cli/Live.cpp) only talks to this interface, so the app code does not
// depend on the command layer. All calls happen in the GUI thread.

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

class Project;
class Selection;

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
    virtual bool liveSave(QString* error) = 0; // only projects with a file
    // Proxy media: generate (in the background), and the state per file ("ready", "pending" with percent, "none")
    virtual void liveGenerateProxies(const QStringList& files) = 0;
    virtual QJsonObject liveProxyState(const QString& file) const = 0;

    // Shown to the user: an AI session is connected (client = its name) or not anymore
    virtual void liveConnectionChanged(bool connected, const QString& client) = 0;
    // Clips an AI command just changed (flash them in the timeline)
    virtual void liveClipsChanged(const QVector<int>& clipIds) = 0;
};
