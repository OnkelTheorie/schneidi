#pragma once
#include <QObject>
#include <QSet>

// Transition at a cut: clip before/after (0 = none -> fade in/out)
struct TransitionKey {
    int leftId = 0, rightId = 0;
    bool isNull() const { return !leftId && !rightId; }
    bool operator==(const TransitionKey&) const = default;
};

// Selected edit point (DaVinci V/U): clip left/right of the cut (0 = none, edge at a gap);
// side: 0 = both sides (roll), -1 = only end of the left clip, +1 = only start of the right one
struct EditPoint {
    int leftId = 0, rightId = 0;
    int side = 0;
    bool isNull() const { return !leftId && !rightId; }
    bool operator==(const EditPoint&) const = default;
};

// Selected clips or a transition (UI state, not part of the project / undo).
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

    // Selecting a transition clears the clip selection (like DaVinci)
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

    // Selecting an edit point clears clip and transition selection (like DaVinci)
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

    // Selected keyframe diamonds in a clip's keyframe track (clip frames); clip selection stays.
    // keyParam: -1 = all parameters (keyframe track), otherwise int(AnimParam) = points in the curve editor
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
