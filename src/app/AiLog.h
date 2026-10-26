#pragma once
// AI log (click on the AI indicator in the top bar, Workspace › AI Log): what AIs connected to the app did in this
// session, newest first – steps inside the app (edits, app actions, also failed ones; each edit is one undo step)
// and work outside it (analyses, render … in schneidi-cli) with progress and a short result. Only in memory, never in
// the project; the length is set in Settings › AI (0 = no log, running work is still shown).

#include "core/LiveHost.h"

#include <QFrame>
#include <QObject>
#include <QTime>
#include <QVector>

class QLabel;
class QTreeWidget;

class AiLog : public QObject {
    Q_OBJECT
public:
    struct Entry {
        enum class State { Done, Running, Failed };
        int id = 0; // own number (stays while the entry is kept)
        QTime time;
        QString step;       // what the AI called: "split, delete", "undo: KI: split", "silence · a.mp4"
        QString detail;     // the clips, or the result of work outside the app
        QVector<int> clips; // new and edited clips (selected on a click)
        int sequence = -1;  // timeline of the clips
        int frame = -1;     // where to jump (-1 = nowhere)
        State state = State::Done;
        int job = 0;        // running work outside the app: its activity id
        QString tool;       // work outside the app: the command ("silence")
        int percent = -1;
    };

    explicit AiLog(QObject* parent = nullptr);
    void setLimit(int n); // finished entries kept (running ones always stay)
    int limit() const { return m_limit; }
    const QVector<Entry>& entries() const { return m_entries; } // newest first
    const Entry* entry(int id) const;
    const Entry* runningJob() const; // the newest work outside the app that still runs (nullptr = none)
    int add(Entry e);                // returns the id
    // Work outside the app: "start" adds an entry, progress/end/failed update the running one with that id
    void activity(const LiveActivity& a);
    void interruptRunning(); // the AI left while its work ran
    void clear();

    // Texts of a job: while it runs ("sucht Stille", after "KI") and its result ("Pausen gefunden: 18")
    static QString jobText(const QString& tool);
    static QString jobResult(const QString& tool, const QJsonObject& summary);
    static QString targetName(const QString& target); // "a.mp4", "a.mp4 +3", the project for "@app"

signals:
    void changed();

private:
    void trim();
    Entry* runningEntry(int job);

    QVector<Entry> m_entries;
    int m_limit = 200;
    int m_next = 1;
};

// The list itself, opened as a popup under the AI indicator: time, step, clips/result; a click on an entry asks the
// main window to select its clips and jump there
class AiLogView : public QFrame {
    Q_OBJECT
public:
    explicit AiLogView(AiLog* log, QWidget* parent = nullptr);

signals:
    void activated(int entryId);

private:
    void rebuild();

    AiLog* m_log;
    QTreeWidget* m_list;
    QLabel* m_empty;
};
