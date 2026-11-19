// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The Direct page's material (left column): a drop area for Media Pool clips (MediaPool::ListMimeType) and files from
// a file manager (URLs), or "the current timeline" as the material, and the list of what is in. Each clip shows its
// length and the analysis points of the last suggestion run (Ton ✓, Bild ✓, Ereignisse …, Sprache –). Delete removes
// the selected clips from the list (the page's own key, shielded from the window's: never timeline clips).

#include "ui/direct/DirectThumbs.h"

#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QWidget>

class QAction;
class QLabel;
class QListWidget;
class QMimeData;
class QPushButton;

class DirectMaterial : public QWidget {
    Q_OBJECT
public:
    explicit DirectMaterial(QWidget* parent = nullptr);

    QStringList files() const;
    bool usesTimeline() const { return m_timeline; }
    // The current timeline as the material instead of files (the file list is kept for when it is switched off)
    void setUseTimeline(bool on);
    // Existing files only, each once; returns how many came in
    int addFiles(const QStringList& files);
    void removeSelected();
    void clear();
    bool isEmpty() const { return !m_timeline && m_files.isEmpty(); }
    // The material as `direct` takes it: {files:[…]} or {timeline:"@current"}
    QJsonObject materialJson() const;
    // Lengths and analysis points from a session's `material` (rows of other files stay as they are)
    void setMaterialState(const QJsonArray& material);

    // The files a drop carries (Media Pool list or URLs; folders and other things left out: looks at the disk, for
    // the drop only); mayCarryFiles = the drag's formats alone (no disk access while it moves)
    static QStringList filesOf(const QMimeData* mime);
    static bool mayCarryFiles(const QMimeData* mime);

    // The current timeline's length (seconds) for its row ("Aktuelle Timeline"); its picture comes through the
    // thumbs' resolver
    void setTimelineLength(double seconds);
    // Pictures of the clips (shared with the suggestions)
    void setThumbs(DirectThumbs* thumbs);
    // The page's key to take clips out (registered at the start, MainWindow): replaces the list's own action
    void setRemoveAction(QAction* action);

    QListWidget* list() const { return m_list; }
    QAction* removeAction() const { return m_remove; }

signals:
    void changed();

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void rebuild();

    QStringList m_files;
    QHash<QString, QJsonObject> m_state; // file -> {length_s, analysis{…}}
    QHash<QString, QImage> m_pictures;   // file -> its picture (kept across rebuilds)
    DirectThumbs* m_thumbs = nullptr;
    DirectThumbs::Generation m_gen;
    bool m_timeline = false;
    double m_timelineLength = 0;
    QLabel* m_hint;
    QListWidget* m_list;
    QPushButton* m_useTimeline;
    QPushButton* m_clear;
    QAction* m_remove;
};
