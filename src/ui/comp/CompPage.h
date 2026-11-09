// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The Comp page (between Edit and Color): one clip's compositing graph. Viewer (inserted by MainWindow) with the
// selected node's handles, the node Inspector on the right, the shared timeline in the middle and below it the graph -
// as layers (a merge chain, top to bottom) or as nodes on a canvas; a switch toggles both. The viewer shows the
// output or the selected node's picture, "Matte" its alpha as gray (CompUi::viewTimeline; MainWindow plays that).
// The page follows every project change (undo/redo, the CLI's @app edits) and keeps the selected node while it exists.

#include <QWidget>

class Editor;
class CompCanvas;
class CompInspector;
class CompLayers;
class CompOverlay;
class QLabel;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QToolButton;

class CompPage : public QWidget {
    Q_OBJECT
public:
    explicit CompPage(Editor* editor, QWidget* parent = nullptr);

    QSplitter* main() const { return m_main; } // the left column | the inspector
    QSplitter* top() const { return m_top; }   // left column: MainWindow puts the viewer at index 0, the graph below

    void setClip(int clipId); // the clip whose graph is shown (0 = none); keeps the node when it is the same clip
    int clip() const { return m_clipId; }
    void setPlayhead(int frame);
    void selectNode(int node);
    int selectedNode() const { return m_selected; }
    void refresh(); // the project changed

    enum class View { Layers, Nodes };
    void setView(View v);
    View view() const;
    // What the viewer shows: the selected node (true) or the output; the alpha as gray
    void setShowNode(bool on);
    bool showNode() const;
    void setMatte(bool on);
    bool matte() const;
    int viewNode() const; // node the viewer shows (0 = the output)

    // "Als Rezept speichern…": the clip's graph as an own recipe (CompRecipes::save, the same as op `comp
    // save_recipe`); empty + err when it fails (code EXISTS: a recipe of that name exists and `replace` is off).
    // The button asks for the name and whether to replace
    QString saveRecipe(const QString& name, bool replace, QString* err = nullptr, QString* code = nullptr);

    // While a tour runs: no "Create Compositing" (an empty graph, not the tour's example) and no tour button in the
    // empty graph (both would sit beside the tour's own button)
    void setTourRunning(bool on);

    CompCanvas* canvas() const { return m_canvas; }
    CompLayers* layers() const { return m_layers; }
    CompInspector* inspector() const { return m_inspector; }
    CompOverlay* overlay() const { return m_overlay; }

signals:
    void viewChanged(); // the viewer should show another node / the matte
    void keyPickRequested(int clipId, int node, const QString& key);
    void message(const QString& text);
    void recipeSaved(const QString& id); // the Effects Library lists it
    void extensionsRequested();          // a node's analysis needs an extension (the cutout model)

private:
    void updateHeader();

    Editor* m_editor;
    int m_clipId = 0, m_selected = 0, m_playhead = 0;
    QSplitter* m_main;
    QSplitter* m_top;
    CompInspector* m_inspector;
    CompCanvas* m_canvas;
    CompLayers* m_layers;
    CompOverlay* m_overlay;
    QStackedWidget* m_stack;
    QToolButton* m_layersButton;
    QToolButton* m_nodesButton;
    QToolButton* m_showNode;
    QToolButton* m_matte;
    QToolButton* m_fit;
    QPushButton* m_create;
    bool m_tourRunning = false;
    QToolButton* m_saveRecipe;
    QLabel* m_clipLabel;
    QLabel* m_message;
};
