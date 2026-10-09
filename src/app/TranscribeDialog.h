#pragma once
#include "core/Types.h"
#include "engine/Transcriber.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;

// Timeline → Subtitles → Create Subtitles from Audio (like DaVinci): the timeline's sound (whole or In/Out, chosen
// audio tracks mixed or one after another) through whisper (engine/Transcriber); accepted = results() holds the new
// subtitle tracks in timeline frames.
class TranscribeDialog : public QDialog {
    Q_OBJECT
public:
    // from/to: range in timeline frames [from, to); format/timeline: what is transcribed; trackLabels: one per audio
    // track (empty = track without clips, not offered)
    TranscribeDialog(const Timeline& timeline, const ProjectFormat& format, int from, int to,
                     const QStringList& trackLabels, QWidget* parent = nullptr);
    struct Result {
        QString name; // "Transkript" or "Transkript – Mikrofon" (one track per audio track)
        QVector<SubtitleCue> cues;
    };
    QVector<Result> results() const { return m_results; }
    QString language() const { return m_language; } // detected or chosen

    void reject() override;

signals:
    void openExtensions(); // whisper or a model is missing

private:
    void updateState();
    void run();
    void startJob();

    Timeline m_timeline;
    ProjectFormat m_format;
    int m_from, m_to;
    QComboBox* m_lang;
    QComboBox* m_model;
    QSpinBox* m_chars;
    struct TrackBox {
        int index;    // audio track
        QString name; // subtitle track name part ("Mikrofon" or "A3")
        QCheckBox* box;
    };
    QVector<TrackBox> m_tracks; // only with several audio tracks
    QCheckBox* m_separate = nullptr;
    struct Job {
        QVector<int> tracks;
        QString name;
    };
    QVector<Job> m_jobs;
    int m_job = 0;
    QLabel* m_missing;
    QProgressBar* m_progress;
    QPushButton* m_start;
    QPushButton* m_extensions;
    Transcriber m_transcriber;
    QVector<Result> m_results;
    QString m_language;
};
