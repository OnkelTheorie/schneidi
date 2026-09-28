#pragma once
#include <QObject>
#include <QSet>

// Übergang an einem Schnitt: Clip davor/danach (0 = keiner -> Ein-/Ausblenden)
struct TransitionKey {
    int leftId = 0, rightId = 0;
    bool isNull() const { return !leftId && !rightId; }
    bool operator==(const TransitionKey& o) const { return leftId == o.leftId && rightId == o.rightId; }
    bool operator!=(const TransitionKey& o) const { return !(*this == o); }
};

// Ausgewählte Clips bzw. ein Übergang (UI-Zustand, nicht Teil des Projekts / Undo).
class Selection : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    const QSet<int>& ids() const { return m_ids; }
    bool contains(int id) const { return m_ids.contains(id); }
    bool isEmpty() const { return m_ids.isEmpty(); }

    void set(const QSet<int>& ids)
    {
        if (ids == m_ids && m_transition.isNull()) return;
        m_ids = ids;
        m_transition = {};
        emit changed();
    }
    void clear() { set({}); }

    // Übergang auswählen hebt die Clip-Auswahl auf (wie DaVinci)
    const TransitionKey& transition() const { return m_transition; }
    void setTransition(const TransitionKey& t)
    {
        if (t == m_transition && m_ids.isEmpty()) return;
        m_ids.clear();
        m_transition = t;
        emit changed();
    }

signals:
    void changed();

private:
    QSet<int> m_ids;
    TransitionKey m_transition;
};
