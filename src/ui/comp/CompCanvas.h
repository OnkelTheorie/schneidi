// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Node canvas of the Comp page: the clip's compositing graph drawn as boxes with inputs (left) and the output (right),
// wires between them. Everything maps onto one Editor comp op (one undo step): drag a node = moveCompNodes, drag a
// wire from an output onto an input = linkCompNodes, grab a connected input and drop it elsewhere = move/remove that
// link, Tab = node search (addCompNode behind the selected node), Del = removeCompNode, Ctrl+D = duplicateCompNode.
// Zoom (wheel) and scroll (drag on the empty canvas, middle button) change only by user input; each clip keeps its
// own view for the session. A graph whose nodes were never placed (made by the CLI or "Comp from selection") is laid
// out by its order for the display; the first node moved stores every place.

#include "core/CompGraph.h"

#include <QHash>
#include <QPointF>
#include <QPointer>
#include <QWidget>

class Editor;
class QLineEdit;
class QListWidget;

// Tab popup: a search field over the node kinds; Enter or a click adds the kind
class CompNodeSearch : public QWidget {
    Q_OBJECT
public:
    explicit CompNodeSearch(QWidget* parent = nullptr);
    QLineEdit* field() const { return m_field; }
    QListWidget* list() const { return m_list; }

signals:
    void chosen(const QString& type);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void filter(const QString& text);
    void choose();
    QLineEdit* m_field;
    QListWidget* m_list;
};

class CompCanvas : public QWidget {
    Q_OBJECT
public:
    explicit CompCanvas(Editor* editor, QWidget* parent = nullptr);

    void setClip(int clipId); // 0 = none
    int clip() const { return m_clipId; }
    void refresh();           // the graph changed (any edit, undo, the CLI)
    void setSelected(int node);
    int selected() const { return m_selected; }
    void fit();               // the whole graph in view (button "Einpassen", key F)
    void openSearch();        // Tab
    void addNode(const QString& type); // behind the selected node (or the output), selected afterwards
    CompNodeSearch* search() const { return m_search; }

    // Widget coordinates (tests, tooltips): a node's box, its output port, one of its input ports
    QRectF nodeRect(int node) const;
    QPointF outputPos(int node) const;
    QPointF inputPos(int node, const QString& input) const;
    // Parts the in-app tours point at (widget coordinates, empty = not there): "output" = the output node, "node:<type>"
    // = the first node of a kind, "port:<input>" = the first input port of that name (a merge's for bg/fg/mask)
    QRect tourArea(const QString& what) const;
    class QPushButton* tourButton() const { return m_tourButton; } // "Start the Compositing tour" in an empty graph
    void setTourButtonAllowed(bool on); // off while a tour runs (the button would sit in its spotlight)
    double zoom() const { return m_view.zoom; }
    QPointF offset() const { return m_view.offset; }

signals:
    void tourRequested(); // the button in the empty graph: the Compositing tour
    void selectedChanged(int node);
    void showRequested(int node); // double-click: show the node's picture in the viewer
    void message(const QString& text);

protected:
    bool event(QEvent*) override; // its keys win over the window's (Del never deletes timeline clips here)
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    bool focusNextPrevChild(bool) override { return false; } // Tab opens the search
    void resizeEvent(QResizeEvent*) override;

private:
    struct View {
        double zoom = 1;
        QPointF offset; // world point at the widget's top left
        bool set = false;
    };
    QPointF toWidget(const QPointF& world) const;
    QPointF toWorld(const QPointF& widget) const;
    QPointF place(int node) const; // world top left of a node
    QSizeF nodeSize(const CompNode& n) const;
    QStringList inputsOf(const CompNode& n) const;
    int nodeAt(const QPointF& pos) const;
    // The input port under pos: node id (0 = none) and input name
    int inputAt(const QPointF& pos, QString* input) const;
    int outputAt(const QPointF& pos) const;
    void layoutUnplaced();
    void removeSelected();
    void duplicateSelected();
    void setOut(int node);
    void report(bool ok, const QString& err);

    Editor* m_editor;
    int m_clipId = 0;
    CompGraph m_graph;
    QHash<int, QPointF> m_places; // shown places (stored ones or the layout of an unplaced graph)
    bool m_unplaced = false;
    int m_selected = 0;
    View m_view;
    QHash<int, View> m_views; // per clip, this session
    QPointer<CompNodeSearch> m_search;
    class QPushButton* m_tourButton = nullptr;
    void placeTourButton();
    bool m_tourButtonAllowed = true;

    enum class Drag { None, Node, Pan, Wire };
    Drag m_drag = Drag::None;
    QPointF m_press, m_mouse;
    QPointF m_pressPlace;
    bool m_moved = false;
    int m_wireFrom = 0;          // output the wire comes from
    int m_detachTo = 0;          // a link grabbed at its input: its node and input (0 = a new wire)
    QString m_detachInput;
};
