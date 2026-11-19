// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The Direct page's suggestions (right column): one row per suggestion of a session - a picture of its start, the
// time, the reason in the UI language and a checkbox. On top the sum of the checked ones ("42 s von 45 s"), all
// on/off and, when the session has variants (short), the variant shown; under the list the In/Out of the current row
// (two boxes, seconds of the source) and the session's notes. A click on a row (not on its checkbox) asks for a
// preview (MainWindow: the file in the viewer, stopping at the row's end - never the project's marks); dragging rows
// changes the order; Delete takes the selected rows out (the page's own key, shielded from the window's: never
// timeline clips). A stale or expired session shows a line with "Neu vorschlagen". Nothing here edits the project:
// choice() is what MainWindow hands to DirectBackend::apply.

#include "ui/direct/DirectThumbs.h"

#include <QHash>
#include <QJsonObject>
#include <QPair>
#include <QWidget>

class QAction;
class QComboBox;
class QDoubleSpinBox;
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
    // A failed run: the message where the list is (details: the command's own words, only as a tooltip)
    void setError(const QString& text, const QString& details = {});
    // A hint where the list is (no session: what the recipe needs first)
    void setHint(const QString& text) { setError(text); }
    QString emptyText() const;
    // The session is stale/expired: the message and "Neu vorschlagen" (resuggestRequested); empty = hidden
    void setStale(const QString& text);

    // The rows of the shown variant, as listed (all of them / the checked ones)
    QList<int> rowOrder() const;
    QList<int> checkedOrder() const;
    void setChecked(int n, bool on);
    void setAllChecked(bool on);
    void removeSelected();
    double checkedSeconds() const;
    QString sumText() const;
    // For DirectBackend::apply: {on:[n…], order:[n…] (the checked ones as listed), adjust:[{n, from_s, to_s}] (the
    // changed ones among them), variant (sessions with variants)}
    QJsonObject choice() const;
    // A suggestion of the session by number (empty = none) and its range with the user's In/Out
    QJsonObject suggestion(int n) const;
    QPair<double, double> range(int n) const;
    void setRange(int n, double from, double to); // clamped to the file (0..its length), at least 0.1 s

    // Variants (short): 0 = the session has none
    int variantCount() const { return m_variants; }
    int variant() const { return m_variant; }
    void setVariant(int v);

    void setThumbs(DirectThumbs* thumbs) { m_thumbs = thumbs; }
    // The page's key to take rows out (registered at the start, MainWindow): replaces the list's own action
    void setRemoveAction(QAction* action);

    QListWidget* list() const { return m_list; }
    QAction* removeAction() const { return m_remove; }
    QLabel* sumLabel() const { return m_sum; }
    QComboBox* variantBox() const { return m_variantBox; }
    QDoubleSpinBox* inBox() const { return m_in; }
    QDoubleSpinBox* outBox() const { return m_out; }

signals:
    void previewRequested(const QString& file, double from, double to);
    void changed(); // checked, order, ranges, variant or rows changed (the result strip follows)
    void resuggestRequested();

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void addRow(const QJsonObject& s, bool on);
    QString rowText(int n) const;
    void updateSum();
    void updateRangeBoxes();
    void loadThumbs();
    double fileLength(const QString& file) const;

    QJsonObject m_session;
    QHash<int, QJsonObject> m_byN;
    QHash<int, QPair<double, double>> m_adjust; // n -> the user's In/Out (seconds of the source)
    int m_variants = 0, m_variant = 1;
    QLabel* m_sum;
    QComboBox* m_variantBox;
    QPushButton* m_allOn;
    QPushButton* m_allOff;
    QListWidget* m_list;
    QWidget* m_rangeRow;
    QDoubleSpinBox* m_in;
    QDoubleSpinBox* m_out;
    QLabel* m_empty;
    QWidget* m_staleRow;
    QLabel* m_staleText;
    QLabel* m_notes;
    QAction* m_remove;
    DirectThumbs* m_thumbs = nullptr;
    DirectThumbs::Generation m_gen; // a new session drops the pictures still coming for the old one
    bool m_filling = false;
    int m_pressedN = -1; // the row a press went to and its check state then: a click that toggled it is no preview
    Qt::CheckState m_pressedState = Qt::Unchecked;
};
