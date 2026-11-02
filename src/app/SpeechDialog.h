// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "engine/Tts.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

// Timeline → Sprechertext…: text, voice, speed and pause -> a spoken voice clip (engine/Tts, Piper). "Anhören" makes
// the file and asks the window to play it (preview()); "Einfügen" makes it (or takes the one made for the preview)
// and accepts: result() is the file, the window puts it at the playhead on a free audio track.
class SpeechDialog : public QDialog {
    Q_OBJECT
public:
    // folder: where the files go (Speech::folder of the project)
    explicit SpeechDialog(const QString& folder, QWidget* parent = nullptr);
    const Tts::Result& result() const { return m_speaker.result(); }
    bool captions() const; // word captions of it (result().words)
    void reject() override;

signals:
    void openExtensions(); // piper or a voice is missing / more voices
    void preview(const QString& path);

private:
    void updateState();
    void fillVoices();
    void make(bool insert);

    QString m_folder;
    QPlainTextEdit* m_text;
    QComboBox* m_voice;
    QDoubleSpinBox* m_speed;
    QDoubleSpinBox* m_pause;
    QCheckBox* m_captions;
    QLabel* m_missing;
    QPushButton* m_extensions;
    QPushButton* m_preview;
    QPushButton* m_insert;
    QProgressBar* m_progress;
    Tts::Speaker m_speaker;
    bool m_insertWhenDone = false;
};
