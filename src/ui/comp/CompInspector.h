// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Inspector of the Comp page: the selected node's parameters (NodeType::params) - numbers with a keyframe diamond
// like the Edit page's Inspector, choices, switches, colors (the key color with a pipette), files. Each change is one
// Editor op: setCompNodeValuesAt (a drag of a number field = one undo step; a number with keyframes gets one at the
// playhead), setCompNodeKeyframe for the diamond. Widgets are rebuilt only when another node (or another set of
// parameters, a mask's shape) is shown, never during a drag. An fx node shows its effect's own values (e_<key>); a
// text value (the filter chain, the shader code) is edited in a multi-line field and set with "Anwenden" after the
// same check as the CLI's (the error shown under the field, nothing set).

#include <QScrollArea>
#include <QStringList>
#include <QVector>
#include <functional>
#include <memory>

class Editor;
namespace Picture::Comp {
struct NodeType;
}
class QLabel;
class QGridLayout;
class QToolButton;
struct Clip;

class CompInspector : public QScrollArea {
    Q_OBJECT
public:
    explicit CompInspector(Editor* editor, QWidget* parent = nullptr);

    void setTarget(int clipId, int node);
    void setPlayhead(int frame);
    void refresh(); // values (and rows, if the shown parameters changed) from the project
    int node() const { return m_node; }

signals:
    // Pipette of a color parameter (the key color): pick it from the picture going into the node
    void keyPickRequested(int node, const QString& key);
    void outRequested(int node); // "Als Ausgang"

private:
    const Clip* clip() const;
    // The node's kind as shown: an fx node also gets the wrapped effect's own parameters as e_<key> (labels, ranges,
    // defaults of the effect's descriptor)
    std::shared_ptr<const Picture::Comp::NodeType> shownKind(const Clip& c) const;
    QStringList shownKeys(const Clip& c) const; // the parameters shown for the node now
    void rebuild(const Clip& c, const QStringList& keys);
    void set(const QString& key, const QVariant& value, bool drag);

    Editor* m_editor;
    int m_clipId = 0, m_node = 0, m_playhead = 0;
    QString m_type;
    QStringList m_keys;
    QWidget* m_body;
    QLabel* m_title;
    QLabel* m_about;
    QToolButton* m_outButton;
    QGridLayout* m_grid = nullptr;
    QWidget* m_rows = nullptr;
    QVector<std::function<void(const Clip&)>> m_updates;
};
