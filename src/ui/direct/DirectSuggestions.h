// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The Direct page's suggestions (right column): one row per suggestion of a session - a picture of its start, the
// time, the reason in the UI language and a checkbox. On top the sum of the checked ones ("42 s von 45 s") and all
// on/off; under the list the session's notes. A click on a row asks for a preview (MainWindow: the file in the viewer,
// stopping at the row's end - never the project's marks); dragging rows changes the order; Delete takes the selected
// rows out (the page's own key, shielded from the window's: never timeline clips). Nothing here edits the project:
// choice() is what MainWindow hands to DirectBackend::apply.

#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QWidget>
#include <functional>

class QAction;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;

class DirectSuggestions : public QWidget {
    Q_OBJECT
public:
    explicit DirectSuggestions(QWidget* parent = nullptr);

    // A session of `direct` (its `session` object): rows in the session's order (the checked ones as in `order`,
    // then the others by number)
    void setSession(const QJsonObject& session);
    const QJsonObject& session() const { return m_session; }
    bool hasSession() const { return !m_session.isEmpty(); }
    void clear();
    // Busy text instead of the list's empty text ("Analysiert …"); empty = not busy
    void setBusyText(const QString& text);
    // A failed run: the message where the list is
    void setError(const QString& text);

    QList<int> rowOrder() const;    // the numbers of all rows as listed
    QList<int> checkedOrder() const; // the checked ones as listed
    void setChecked(int n, bool on);
    void setAllChecked(bool on);
    void removeSelected();
    double checkedSeconds() const;
    QString sumText() const;
    // For DirectBackend::apply: {on:[n…], order:[n…]} (only rows still in the list)
    QJsonObject choice() const;
    // A suggestion of the session by number (empty = none)
    QJsonObject suggestion(int n) const;

    // Pictures of the rows: called on a worker thread (Engine::thumbnail), results come back in the UI thread
    void setThumbnailer(std::function<QImage(const QString& file, double seconds, const QSize& size)> thumb);

    QListWidget* list() const { return m_list; }
    QAction* removeAction() const { return m_remove; }
    QLabel* sumLabel() const { return m_sum; }

signals:
    void previewRequested(const QString& file, double from, double to);
    void changed(); // checked, order or rows changed (the result strip follows)

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void addRow(const QJsonObject& s, bool on);
    void updateSum();
    void loadThumbs();

    QJsonObject m_session;
    QHash<int, QJsonObject> m_byN;
    QLabel* m_sum;
    QPushButton* m_allOn;
    QPushButton* m_allOff;
    QListWidget* m_list;
    QLabel* m_empty;
    QLabel* m_notes;
    QAction* m_remove;
    std::function<QImage(const QString&, double, const QSize&)> m_thumb;
    int m_generation = 0; // a new session drops the pictures still coming for the old one
    bool m_filling = false;
};
