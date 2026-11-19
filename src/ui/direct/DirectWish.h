// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The Direct page's own wish line (0.25, R5): one line above the suggestions ("nur Runden mit AWP"), shown only when
// an AI chat provider is set up (MainWindow decides; hidden without one, no hint). Enter asks for wishRequested with
// the trimmed text (nothing on an empty line or while busy); MainWindow sends it to the app's AI chat with the page's
// context, the model answers by calling `direct` and its suggestions replace the page's list. While the chat works a
// busy line "KI denkt …" with ✕ (stopRequested); afterwards the model's short answer or an error as a note under the
// line. The text stays in the line (a wish is often refined).

#include <QWidget>

class QLabel;
class QLineEdit;
class QToolButton;

class DirectWish : public QWidget {
    Q_OBJECT
public:
    explicit DirectWish(QWidget* parent = nullptr);

    void setBusy(bool busy);
    bool busy() const { return m_busy; }
    // The note under the line (empty = none); error = in the warning color
    void setNote(const QString& text, bool error = false);
    QString note() const;
    bool noteIsError() const { return m_error; }

    QLineEdit* edit() const { return m_edit; }
    QWidget* busyRow() const { return m_busyRow; }
    QToolButton* stopButton() const { return m_stop; }
    QLabel* noteLabel() const { return m_note; }

signals:
    void wishRequested(const QString& text);
    void stopRequested();

private:
    QLineEdit* m_edit;
    QWidget* m_busyRow;
    QLabel* m_busyText;
    QToolButton* m_stop;
    QLabel* m_note;
    bool m_busy = false;
    bool m_error = false;
};
