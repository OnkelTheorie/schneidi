// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Layer view of the Comp page: the same graph as a list from top to bottom (layer, blend mode, mask, opacity) for
// everyone who does not want nodes. It shows a graph completely only when it is a merge chain - the output is a merge
// whose bg comes from a merge … down to a base picture, each fg/mask fed by a plain chain of nodes; anything else
// shows a hint to the node view. Mode and opacity are values of the layer's merge node (setCompNodeValuesAt, one undo
// step each; a drag of the opacity = one step); a click on a layer selects its picture's node.

#include "core/CompGraph.h"

#include <QVector>
#include <QWidget>

class Editor;
class QLabel;
class QPushButton;
class QVBoxLayout;
struct Clip;

class CompLayers : public QWidget {
    Q_OBJECT
public:
    explicit CompLayers(Editor* editor, QWidget* parent = nullptr);

    void setClip(int clipId);
    void setPlayhead(int frame) { m_playhead = frame; }
    void setSelected(int node);
    void refresh();
    // The clip has a graph that is not a plain layer stack (it only shows as nodes)
    bool onlyNodes() const { return m_onlyNodes; }

    // One layer of a merge chain, top first; the last one is the base picture (merge = 0)
    struct Layer {
        int merge = 0; // merge node that lays it on what is below (0 = the base)
        int node = 0;  // node whose picture it is (fg of the merge, or the base)
        int mask = 0;  // node at the merge's mask input (0 = none)
        bool operator==(const Layer&) const = default;
    };
    // The graph as layers; false = not a pure merge chain (or no graph)
    static bool layersOf(const CompGraph& g, QVector<Layer>* out);
    const QVector<Layer>& layers() const { return m_layers; }

signals:
    void selectedChanged(int node);
    void nodeViewRequested();

private:
    void rebuild(const Clip& c, const CompGraph& g);

    Editor* m_editor;
    int m_clipId = 0, m_selected = 0, m_playhead = 0;
    QVector<Layer> m_layers;
    CompGraph m_shown;
    QVBoxLayout* m_list;
    QWidget* m_listBox;
    QWidget* m_hint;
    bool m_onlyNodes = false;
    QLabel* m_hintText;
};
