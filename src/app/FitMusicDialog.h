// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Clip › "Musik auf Länge…": the length a song is cut to (edit op fit_music, cli/EditOpsMusic.cpp) - the length of
// the timeline (its other clips), the In/Out range (the song then starts at In) or a number of seconds - and how it
// ends (its own end, or faded out). The dialog only asks; MainWindow::fitMusic() runs the op.

#include <QDialog>
#include <QJsonObject>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QRadioButton;

class FitMusicDialog : public QDialog {
    Q_OBJECT
public:
    // songFrames = the song file's length, timelineFrames = from the song's start to the end of the other clips (<= 0 =
    // nothing to take the length from), markIn/markOut = the In/Out range (frames, out inclusive; -1 = not set)
    FitMusicDialog(int songFrames, int timelineFrames, int markIn, int markOut, double fps, QWidget* parent = nullptr);

    enum class Mode { Timeline, InOut, Seconds };
    Mode mode() const;
    void setMode(Mode m);
    void setSeconds(double s);
    double seconds() const;
    bool fadeOut() const; // end:'fade' instead of the song's own end
    // The op's fields for the choice: length (frames or 'timeline'), at for In/Out, end
    QJsonObject opFields() const;
    int targetFrames() const; // the length chosen (frames)

private:
    void updateHint();

    QRadioButton* m_timeline;
    QRadioButton* m_inOut;
    QRadioButton* m_number;
    QDoubleSpinBox* m_seconds;
    QComboBox* m_end;
    QLabel* m_hint;
    int m_song, m_timelineFrames, m_in, m_out;
    double m_fps;
};
