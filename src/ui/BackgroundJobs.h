// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Work running in the background, shown small at the bottom right of the main window (its page bar): a slim bar, one
// line of text ("Proxy 2/5 – clip.mp4 – 34 %") and a ✕ that cancels. Any part of the app reports there by an id
// (proxies today; render queue, transcription and analysis can follow): setJob() while it runs, finishJob() at the
// end - the done text stays a moment, then the indicator hides once no job is left. Several jobs: the first one is
// shown, the others counted ("+1"); the tooltip lists all of them. A notice (a failure to report) stays until its ✕.

#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>
#include <functional>

class ElidedLabel;
class QProgressBar;
class QTimer;
class QToolButton;

class BackgroundJobs : public QWidget {
    Q_OBJECT
public:
    struct Job {
        QString text;                // one short line
        double fraction = -1;        // 0..1, below 0 = busy without a known amount
        QString toolTip;             // details (what is waiting)
        std::function<void()> cancel; // empty = no ✕
        QString cancelToolTip;        // what the ✕ does, when not "Abbrechen" (e.g. it pauses)
        // A message, not work (e.g. "3 proxies failed", details in the tooltip): no bar, not counted as running, shown
        // when nothing runs, stays until its ✕ (cancel: dismiss it) or removeJob
        bool notice = false;
    };

    explicit BackgroundJobs(QWidget* parent = nullptr);

    void setJob(const QString& id, const Job& job);
    // The job ended: `doneText` is shown for holdMs(), then the job goes (a new setJob in between keeps it)
    void finishJob(const QString& id, const QString& doneText);
    void removeJob(const QString& id);

    QStringList jobIds() const;
    bool hasJob(const QString& id) const;
    // What the indicator shows now (tests)
    QString text() const;
    int percent() const; // -1 = busy bar or a done text
    int holdMs() const { return m_holdMs; }
    void setHoldMs(int ms) { m_holdMs = ms; } // tests

private:
    struct Entry {
        QString id;
        Job job;
        bool done = false;
        QTimer* timer = nullptr;
    };
    int indexOf(const QString& id) const;
    const Entry* shownEntry() const; // the one the line shows: the first running job, else a notice, else a done one
    void refresh();

    QVector<Entry> m_jobs;
    QProgressBar* m_bar;
    ElidedLabel* m_text;
    QToolButton* m_cancel;
    int m_holdMs = 2000;
};
