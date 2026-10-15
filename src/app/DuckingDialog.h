#pragma once
#include "core/Ducking.h"

#include <QDialog>
#include <QVector>

class Editor;
class QCheckBox;
class QDialogButtonBox;
class QLineEdit;

// Ducking (core/Ducking.h): music = the selected audio clips, "lower while there is sound on" = one checkbox per
// other audio track, values like the CLI op `duck`. Remembers the tracks (by name) and values (settings "ducking/").
class DuckingDialog : public QDialog {
    Q_OBJECT
public:
    DuckingDialog(Editor* editor, const QVector<int>& musicIds, QWidget* parent = nullptr);
    QVector<int> musicIds() const { return m_music; }
    QVector<int> underTracks() const; // audio track indices
    Ducking::Params params() const;

    void accept() override;

private:
    void updateOk();

    Editor* m_editor;
    QVector<int> m_music; // usable music clips (audio, not locked)
    double m_fps = 25;
    QVector<QPair<int, QCheckBox*>> m_tracks;
    QLineEdit* m_amount;
    QLineEdit* m_threshold;
    QLineEdit* m_attack;  // seconds
    QLineEdit* m_release; // seconds
    QLineEdit* m_hold;    // seconds
    QDialogButtonBox* m_buttons;
};
