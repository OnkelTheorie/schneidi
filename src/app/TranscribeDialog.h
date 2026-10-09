#pragma once
#include "core/Types.h"
#include "engine/Transcriber.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;

// Timeline → Subtitles → Create Subtitles from Audio (like DaVinci): the timeline's sound (whole or In/Out) through
// whisper (engine/Transcriber); accepted = cues() holds the new subtitles in timeline frames.
class TranscribeDialog : public QDialog {
    Q_OBJECT
public:
    // from/to: range in timeline frames [from, to); format/timeline: what is transcribed
    TranscribeDialog(const Timeline& timeline, const ProjectFormat& format, int from, int to, QWidget* parent = nullptr);
    QVector<SubtitleCue> cues() const { return m_cues; }
    QString language() const { return m_language; } // detected or chosen

    void reject() override;

signals:
    void openExtensions(); // whisper or a model is missing

private:
    void updateState();
    void run();

    Timeline m_timeline;
    ProjectFormat m_format;
    int m_from, m_to;
    QComboBox* m_lang;
    QComboBox* m_model;
    QSpinBox* m_chars;
    QLabel* m_missing;
    QProgressBar* m_progress;
    QPushButton* m_start;
    QPushButton* m_extensions;
    Transcriber m_transcriber;
    QVector<SubtitleCue> m_cues;
    QString m_language;
};
