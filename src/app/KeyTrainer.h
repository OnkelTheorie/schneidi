// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QFrame>
#include <QStringList>

class QLabel;
class QPushButton;

// Help › Practice keys: a small card that asks for the key of a common action ("Press the key for: Split clip") with the
// binding of the current set, 10 rounds, skippable. Non-modal and quiet (no sound); while it has the focus it takes
// every key itself, so nothing in the window fires (Space does not start playback); Esc closes it. Nothing runs in the
// background.
class KeyTrainer : public QFrame {
    Q_OBJECT
public:
    static constexpr int kRounds = 10;
    // seed: order of the actions (tests pass a fixed one)
    explicit KeyTrainer(QWidget* parent = nullptr, quint32 seed = 0);

    QString currentId() const; // action asked for, empty when finished
    int round() const { return m_round; } // 0-based
    int hits() const { return m_hits; }   // rounds right on the first try
    bool finished() const { return m_round >= m_ids.size(); }
    void skip();    // next round, no hit
    void restart(); // 10 new rounds

    // Common actions the trainer asks for (those with a key in the current set)
    static QStringList candidates();

protected:
    bool event(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    void showRound();
    void pressed(const QString& input); // PortableText of the key pressed
    bool fits(const QString& input) const; // one of the keys of the action asked for

    QStringList m_ids;
    int m_round = 0;
    int m_hits = 0;
    bool m_missed = false; // a wrong key in this round
    quint32 m_seed;
    QLabel* m_progress;
    QLabel* m_ask;
    QLabel* m_action;
    QLabel* m_key;
    QLabel* m_feedback;
    QPushButton* m_skip;
    QPushButton* m_again;
    QPushButton* m_close;
};
