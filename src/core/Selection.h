#pragma once
#include <QObject>
#include <QSet>

// Ausgewählte Clips (UI-Zustand, nicht Teil des Projekts / Undo).
class Selection : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    const QSet<int>& ids() const { return m_ids; }
    bool contains(int id) const { return m_ids.contains(id); }
    bool isEmpty() const { return m_ids.isEmpty(); }

    void set(const QSet<int>& ids)
    {
        if (ids == m_ids) return;
        m_ids = ids;
        emit changed();
    }
    void clear() { set({}); }

signals:
    void changed();

private:
    QSet<int> m_ids;
};
