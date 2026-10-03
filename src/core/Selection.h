#pragma once
#include <QObject>
#include <QSet>

// Übergang an einem Schnitt: Clip davor/danach (0 = keiner -> Ein-/Ausblenden)
struct TransitionKey {
    int leftId = 0, rightId = 0;
    bool isNull() const { return !leftId && !rightId; }
    bool operator==(const TransitionKey&) const = default;
};

// Ausgewählter Schnittpunkt (DaVinci V/U): Clip links/rechts vom Schnitt (0 = keiner, Kante an einer Lücke);
// side: 0 = beide Seiten (Roll), -1 = nur Ende des linken Clips, +1 = nur Anfang des rechten
struct EditPoint {
    int leftId = 0, rightId = 0;
    int side = 0;
    bool isNull() const { return !leftId && !rightId; }
    bool operator==(const EditPoint&) const = default;
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
        if (ids == m_ids && m_transition.isNull() && m_keyTimes.isEmpty() && m_edit.isNull()) return;
        m_ids = ids;
        m_transition = {};
        m_edit = {};
        m_keyClip = 0;
        m_keyTimes.clear();
        m_keyParam = -1;
        emit changed();
    }
    void clear() { set({}); }

    // Übergang auswählen hebt die Clip-Auswahl auf (wie DaVinci)
    const TransitionKey& transition() const { return m_transition; }
    void setTransition(const TransitionKey& t)
    {
        if (t == m_transition && m_ids.isEmpty() && m_edit.isNull()) return;
        m_ids.clear();
        m_transition = t;
        m_edit = {};
        m_keyClip = 0;
        m_keyTimes.clear();
        m_keyParam = -1;
        emit changed();
    }

    // Schnittpunkt auswählen hebt Clip- und Übergangsauswahl auf (wie DaVinci)
    const EditPoint& editPoint() const { return m_edit; }
    void setEditPoint(const EditPoint& e)
    {
        if (e == m_edit && m_ids.isEmpty() && m_transition.isNull()) return;
        m_ids.clear();
        m_transition = {};
        m_edit = e;
        m_keyClip = 0;
        m_keyTimes.clear();
        m_keyParam = -1;
        emit changed();
    }

    // Ausgewählte Keyframe-Rauten in der Keyframe-Spur eines Clips (Clip-Frames); Clip-Auswahl bleibt.
    // keyParam: -1 = alle Parameter (Keyframe-Spur), sonst int(AnimParam) = Punkte im Kurven-Editor
    int keyClip() const { return m_keyClip; }
    const QSet<int>& keyTimes() const { return m_keyTimes; }
    int keyParam() const { return m_keyParam; }
    void setKeyframes(int clipId, const QSet<int>& times, int param = -1)
    {
        if (clipId == m_keyClip && times == m_keyTimes && param == m_keyParam) return;
        m_keyClip = times.isEmpty() ? 0 : clipId;
        m_keyTimes = times;
        m_keyParam = times.isEmpty() ? -1 : param;
        emit changed();
    }

signals:
    void changed();

private:
    QSet<int> m_ids;
    TransitionKey m_transition;
    EditPoint m_edit;
    int m_keyClip = 0;
    QSet<int> m_keyTimes;
    int m_keyParam = -1;
};
