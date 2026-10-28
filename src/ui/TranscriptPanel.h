// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "core/TranscriptCut.h"
#include "engine/Transcriber.h"

#include <QWidget>

class Editor;
class Project;
class QAction;
class QComboBox;
class QKeyEvent;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QTextEdit;
class QToolButton;

// Edit page → Transcript (text-based editing like DaVinci): the words spoken in the timeline's audio clips, in
// timeline order. Click = jump there, select words + Delete = cut them out of the timeline (ripple, all tracks, one
// undo step), "Remove pauses" cuts the quiet between words. Transcripts belong to the media files
// (MediaInfo::transcripts), so the text follows every edit; "Transcribe" runs whisper for clips without one.
class TranscriptPanel : public QWidget {
    Q_OBJECT
public:
    TranscriptPanel(Project* project, Editor* editor, QWidget* parent = nullptr);
    ~TranscriptPanel() override;

    // Cut the selected words (as with the Delete key); false = no words selected
    bool cutSelection();
    // The action behind it with its keys (default Delete and Backspace; bindable as "transcript_cut", scope
    // "transcript"). It is not added to a widget: the text matches its keys itself, so they only act while the
    // transcript has the focus and words are selected - and come before the timeline's Delete there.
    QAction* cutAction() const { return m_cutAction; }
    const QVector<TranscriptCut::Word>& words() const { return m_words; }
    // Select words first..last in the text (tests, CLI-like use)
    void selectWords(int first, int last);

public slots:
    void setPlayhead(int frame);

signals:
    void seekRequested(int frame);
    void openExtensions(); // whisper or a model is missing

protected:
    void showEvent(QShowEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void scheduleRebuild();
    void rebuild();
    void rebuildTracks();
    int audioTrack() const; // -1 = all
    int wordAt(int charPos) const;
    QPair<int, int> selectedWords() const; // first, last; -1 = none
    bool isCutKey(const QKeyEvent* e) const; // one of cutAction's keys
    void updateHighlight();
    void updateState();
    void transcribe();
    void startNext();
    void removePauses();

    Project* m_project;
    Editor* m_editor;
    QComboBox* m_track;
    QToolButton* m_transcribe;
    QToolButton* m_pauses;
    QDoubleSpinBox* m_minPause;
    QProgressBar* m_progress;
    QToolButton* m_cancel;
    QLabel* m_hint;
    QTextEdit* m_text;
    QAction* m_cutAction;
    QVector<TranscriptCut::Word> m_words;
    QVector<QPair<int, int>> m_chars; // per word: [start, end) in the text
    int m_playhead = 0;
    int m_current = -1; // word under the playhead
    bool m_dirty = true;
    bool m_rebuildQueued = false;

    Transcriber m_transcriber;
    QVector<QPair<QString, int>> m_queue; // media + audio stream still to transcribe
    int m_queueTotal = 0;
};
