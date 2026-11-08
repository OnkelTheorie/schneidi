// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/comp/CompCanvas.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/TimelineOps.h"
#include "engine/picture/CompNodes.h"
#include "ui/comp/CompShared.h"

#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QUndoStack>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace PC = Picture::Comp;

namespace {

constexpr double kNodeW = 132;   // world units (= pixels at zoom 1)
constexpr double kHeader = 22;
constexpr double kRow = 17;
constexpr double kPort = 5;      // port radius
constexpr double kGrab = 10;     // grab distance of a port (widget pixels)
constexpr double kMinZoom = 0.25, kMaxZoom = 3;

} // namespace

// ---- Node search (Tab) ----

CompNodeSearch::CompNodeSearch(QWidget* parent) : QWidget(parent, Qt::Popup)
{
    setObjectName("comp_node_search");
    setAttribute(Qt::WA_DeleteOnClose, false);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(4);
    m_field = new QLineEdit;
    m_field->setObjectName("comp_node_search_text");
    m_field->setPlaceholderText(T("Knoten suchen…"));
    m_list = new QListWidget;
    m_list->setObjectName("comp_node_search_list");
    lay->addWidget(m_field);
    lay->addWidget(m_list);
    for (const PC::NodeType& k : PC::all()) {
        auto* item = new QListWidgetItem(k.name);
        item->setData(Qt::UserRole, k.id);
        item->setToolTip(k.description);
        m_list->addItem(item);
    }
    resize(240, 300);
    connect(m_field, &QLineEdit::textChanged, this, &CompNodeSearch::filter);
    connect(m_field, &QLineEdit::returnPressed, this, &CompNodeSearch::choose);
    connect(m_list, &QListWidget::itemActivated, this, &CompNodeSearch::choose);
    connect(m_list, &QListWidget::itemClicked, this, &CompNodeSearch::choose);
    m_field->installEventFilter(this);
    filter({});
}

void CompNodeSearch::filter(const QString& text)
{
    QListWidgetItem* first = nullptr;
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem* it = m_list->item(i);
        const bool show = text.isEmpty() || it->text().contains(text, Qt::CaseInsensitive)
                          || it->data(Qt::UserRole).toString().contains(text, Qt::CaseInsensitive);
        it->setHidden(!show);
        if (show && !first) first = it;
    }
    m_list->setCurrentItem(first);
}

void CompNodeSearch::choose()
{
    QListWidgetItem* it = m_list->currentItem();
    if (!it || it->isHidden()) return;
    const QString type = it->data(Qt::UserRole).toString();
    hide();
    emit chosen(type);
}

bool CompNodeSearch::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_field && event->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(event);
        if (k->key() == Qt::Key_Down || k->key() == Qt::Key_Up) { // arrows move in the list while typing
            int row = m_list->currentRow();
            const int step = k->key() == Qt::Key_Down ? 1 : -1;
            for (int i = row + step; i >= 0 && i < m_list->count(); i += step)
                if (!m_list->item(i)->isHidden()) {
                    m_list->setCurrentRow(i);
                    break;
                }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

// ---- Canvas ----

CompCanvas::CompCanvas(Editor* editor, QWidget* parent) : QWidget(parent), m_editor(editor)
{
    setObjectName("comp_canvas");
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumHeight(120);
    setToolTip(T("Tab oder Umschalt+Leertaste: Knoten hinzufügen · Entf: entfernen · Strg+D: duplizieren · Ausgang auf Eingang ziehen: "
                 "verbinden · Mausrad: zoomen · leere Fläche ziehen: verschieben"));
}

void CompCanvas::setClip(int clipId)
{
    if (clipId != m_clipId) {
        if (m_clipId) m_views[m_clipId] = m_view;
        m_clipId = clipId;
        m_view = m_views.value(clipId);
        m_selected = 0;
    }
    refresh();
}

void CompCanvas::refresh()
{
    m_graph = {};
    const Clip* c = m_clipId ? TimelineOps::findClip(m_editor->project()->timeline(), m_clipId) : nullptr;
    if (c) CompUi::graphOf(*c, &m_graph);
    layoutUnplaced();
    if (m_selected && !m_graph.node(m_selected)) setSelected(0);
    // A clip shown for the first time: its graph in view once (opening it is the user's act; later edits never move
    // the view)
    // (only when shown: a hidden canvas has no real size yet and the fit would cut nodes off)
    if (!m_view.set && !m_graph.isEmpty() && isVisible() && width() > 0) fit();
    update();
}

void CompCanvas::layoutUnplaced()
{
    m_places.clear();
    // Unplaced = every node at the same place (the CLI and "Comp from selection" leave pos at 0)
    m_unplaced = m_graph.nodes.size() > 1 && std::all_of(m_graph.nodes.begin(), m_graph.nodes.end(), [&](const CompNode& n) {
                     return n.pos == m_graph.nodes.first().pos;
                 });
    if (!m_unplaced) {
        for (const CompNode& n : m_graph.nodes) m_places.insert(n.id, n.pos);
        return;
    }
    // Column = the longest way from a node without inputs; rows in the order of the nodes
    QHash<int, int> column;
    for (int pass = 0; pass < m_graph.nodes.size(); ++pass)
        for (const CompLink& l : m_graph.links)
            column[l.to] = std::max(column.value(l.to), column.value(l.from) + 1);
    QHash<int, int> rows;
    for (const CompNode& n : m_graph.nodes) {
        const int col = column.value(n.id);
        const int row = rows[col]++;
        m_places.insert(n.id, QPointF(col * (kNodeW + 48), row * 90));
    }
}

QPointF CompCanvas::toWidget(const QPointF& w) const { return (w - m_view.offset) * m_view.zoom; }
QPointF CompCanvas::toWorld(const QPointF& p) const { return p / m_view.zoom + m_view.offset; }
QPointF CompCanvas::place(int node) const { return m_places.value(node); }

QStringList CompCanvas::inputsOf(const CompNode& n) const
{
    if (const PC::NodeType* k = PC::find(n.type)) return k->inputs;
    QStringList in; // unknown kind: the inputs its links use
    for (const CompLink& l : m_graph.links)
        if (l.to == n.id && !in.contains(l.input)) in << l.input;
    return in;
}

QSizeF CompCanvas::nodeSize(const CompNode& n) const
{
    // Header, a row per input (the output port sits in the first), a row for the node's key value
    return {kNodeW, kHeader + std::max<qsizetype>(1, inputsOf(n).size()) * kRow + kRow + 4};
}

QRectF CompCanvas::nodeRect(int node) const
{
    const CompNode* n = m_graph.node(node);
    if (!n) return {};
    const QPointF tl = toWidget(place(node));
    const QSizeF s = nodeSize(*n) * m_view.zoom;
    return {tl, s};
}

QPointF CompCanvas::outputPos(int node) const
{
    const CompNode* n = m_graph.node(node);
    if (!n) return {};
    return toWidget(place(node) + QPointF(kNodeW, kHeader + kRow / 2));
}

QPointF CompCanvas::inputPos(int node, const QString& input) const
{
    const CompNode* n = m_graph.node(node);
    if (!n) return {};
    const int i = int(inputsOf(*n).indexOf(input));
    if (i < 0) return {};
    return toWidget(place(node) + QPointF(0, kHeader + i * kRow + kRow / 2));
}

int CompCanvas::nodeAt(const QPointF& pos) const
{
    for (auto it = m_graph.nodes.crbegin(); it != m_graph.nodes.crend(); ++it) // the top one first
        if (nodeRect(it->id).contains(pos)) return it->id;
    return 0;
}

int CompCanvas::inputAt(const QPointF& pos, QString* input) const
{
    for (const CompNode& n : m_graph.nodes)
        for (const QString& in : inputsOf(n)) {
            const QPointF p = inputPos(n.id, in);
            if (std::hypot(p.x() - pos.x(), p.y() - pos.y()) <= kGrab) {
                *input = in;
                return n.id;
            }
        }
    return 0;
}

int CompCanvas::outputAt(const QPointF& pos) const
{
    for (const CompNode& n : m_graph.nodes) {
        const QPointF p = outputPos(n.id);
        if (std::hypot(p.x() - pos.x(), p.y() - pos.y()) <= kGrab) return n.id;
    }
    return 0;
}

void CompCanvas::setSelected(int node)
{
    if (node == m_selected) return;
    m_selected = node;
    update();
    emit selectedChanged(node);
}

void CompCanvas::fit()
{
    if (m_graph.isEmpty() || width() <= 0 || height() <= 0) return;
    QRectF r;
    for (const CompNode& n : m_graph.nodes) r |= QRectF(place(n.id), nodeSize(n));
    r.adjust(-30, -30, 30, 30);
    m_view.zoom = std::clamp(std::min(width() / r.width(), height() / r.height()), kMinZoom, 1.0);
    m_view.offset = r.center() - QPointF(width(), height()) / 2 / m_view.zoom;
    m_view.set = true;
    update();
}

void CompCanvas::report(bool ok, const QString& err)
{
    if (!ok && !err.isEmpty()) emit message(T("Geht nicht: %1").arg(err));
}

void CompCanvas::openSearch()
{
    if (!m_clipId) return;
    if (!m_search) {
        m_search = new CompNodeSearch(this);
        connect(m_search, &CompNodeSearch::chosen, this, &CompCanvas::addNode);
    }
    m_search->field()->clear();
    const QPoint at = rect().contains(m_mouse.toPoint()) ? m_mouse.toPoint() : rect().center();
    m_search->move(mapToGlobal(at));
    m_search->show();
    m_search->field()->setFocus();
}

void CompCanvas::addNode(const QString& type)
{
    QStringList inputs;
    if (!PC::kindInfo(type, &inputs)) return;
    // Behind the selected node (or the output): it feeds the new node's first input. Only added (the user wires it):
    // a generator, and a mask behind a picture (its input combines masks; it goes into a merge's mask input)
    const PC::NodeType* k = PC::find(type);
    const int from = m_selected ? m_selected : m_graph.out;
    const CompNode* f = m_graph.node(from);
    const PC::NodeType* fk = f ? PC::find(f->type) : nullptr;
    const bool maskBehindPicture = k && k->output == PC::Buf::Mask && (!fk || fk->output != PC::Buf::Mask);
    const QString input = inputs.isEmpty() || (maskBehindPicture && !m_graph.isEmpty()) ? QString() : inputs.first();
    // Its place: right of `from` in a chain, left below it when only added (it mostly feeds `from`: a merge's fg);
    // further down while that is taken. Placed in the same undo step (an unplaced graph gets every shown place, as a
    // node drag does)
    const QHash<int, QPointF> before = m_unplaced ? m_places : QHash<int, QPointF>();
    QPointF at = f ? place(from) + (input.isEmpty() ? QPointF(-(kNodeW + 48), 100) : QPointF(kNodeW + 48, 0)) : QPointF();
    const auto taken = [&](const QPointF& p) {
        const QRectF r(p, QSizeF(kNodeW, kHeader + 2 * kRow));
        for (const CompNode& n : m_graph.nodes)
            if (QRectF(place(n.id), nodeSize(n)).adjusted(-8, -8, 8, 8).intersects(r)) return true;
        return false;
    };
    for (int i = 0; i < 50 && taken(at); ++i) at.ry() += 70;
    QUndoStack* undo = m_editor->project()->undoStack();
    undo->beginMacro(T("Knoten hinzufügen"));
    QString err;
    const int id = m_editor->addCompNode(m_clipId, type, {}, m_selected, input, CompUi::kinds(), &err);
    if (id) {
        QHash<int, QPointF> places = before;
        places.insert(id, at);
        m_editor->moveCompNodes(m_clipId, places);
    }
    undo->endMacro();
    report(id != 0, err);
    if (id) {
        refresh();
        setSelected(id);
    }
}

void CompCanvas::removeSelected()
{
    if (!m_selected) return;
    QString err;
    const bool ok = m_editor->removeCompNode(m_clipId, m_selected, CompUi::kinds(), &err);
    report(ok, err);
}

void CompCanvas::duplicateSelected()
{
    if (!m_selected) return;
    QString err;
    const int id = m_editor->duplicateCompNode(m_clipId, m_selected, place(m_selected) + QPointF(24, 48), &err);
    report(id != 0, err);
    if (id) {
        refresh();
        setSelected(id);
    }
}

void CompCanvas::setOut(int node)
{
    QString err;
    report(m_editor->setCompOut(m_clipId, node, CompUi::kinds(), &err), err);
}

void CompCanvas::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), Theme::timelineBg);
    // Dot grid (moves with the view: shows scrolling and zoom)
    const double step = 40 * m_view.zoom;
    if (step >= 8) {
        p.setPen(QPen(Theme::trackBg.lighter(130), 1.5));
        const QPointF o = toWidget(QPointF(0, 0));
        for (double x = std::fmod(o.x(), step) - step; x < width(); x += step)
            for (double y = std::fmod(o.y(), step) - step; y < height(); y += step) p.drawPoint(QPointF(x, y));
    }
    if (!m_clipId || m_graph.isEmpty()) {
        p.setPen(Theme::textDim);
        p.drawText(rect(), Qt::AlignCenter, m_clipId ? T("Leerer Graph – Tab fügt einen Knoten hinzu")
                                                     : T("Kein Clip – wähle einen Videoclip in der Timeline"));
        return;
    }
    const double z = m_view.zoom;
    const Clip* clipPtr = TimelineOps::findClip(m_editor->project()->timeline(), m_clipId);
    // Wires
    auto wire = [&](const QPointF& a, const QPointF& b, const QColor& col) {
        QPainterPath path(a);
        const double dx = std::max(30.0 * z, std::abs(b.x() - a.x()) / 2);
        path.cubicTo(a + QPointF(dx, 0), b - QPointF(dx, 0), b);
        p.setPen(QPen(col, std::max(1.2, 2 * z)));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    };
    for (const CompLink& l : m_graph.links) {
        if (m_drag == Drag::Wire && l.to == m_detachTo && l.input == m_detachInput) continue; // being moved
        const bool hot = l.from == m_selected || l.to == m_selected;
        wire(outputPos(l.from), inputPos(l.to, l.input), hot ? Theme::primary : Theme::textDim);
    }
    if (m_drag == Drag::Wire) wire(outputPos(m_wireFrom), m_mouse, Theme::secondary);
    // Nodes
    QFont small = font();
    small.setPointSizeF(std::max(5.0, font().pointSizeF() * std::min(1.0, z) * 0.85));
    QFont bold = font();
    bold.setPointSizeF(std::max(5.0, font().pointSizeF() * std::min(1.0, z)));
    bold.setBold(true);
    for (const CompNode& n : m_graph.nodes) {
        const PC::NodeType* k = PC::find(n.type);
        const QRectF r = nodeRect(n.id);
        const QColor head = !k ? Theme::warning.darker(160)
                            : k->output == PC::Buf::Mask ? QColor(0x5a, 0x5a, 0x66)
                            : k->inputs.isEmpty() ? Theme::titleClip.darker(115)
                                                  : Theme::videoClip.darker(115);
        QPainterPath box;
        box.addRoundedRect(r, 5 * z, 5 * z);
        p.fillPath(box, Theme::panelHeader);
        QPainterPath top;
        top.addRoundedRect(QRectF(r.topLeft(), QSizeF(r.width(), kHeader * z)), 5 * z, 5 * z);
        p.fillPath(top, head);
        const bool out = n.id == m_graph.out;
        p.setPen(QPen(n.id == m_selected ? Theme::primary : out ? Theme::secondary : Theme::border,
                      n.id == m_selected || out ? 2 : 1));
        p.setBrush(Qt::NoBrush);
        p.drawPath(box);
        p.setFont(bold);
        p.setPen(Theme::text);
        const QRectF titleRect(r.left() + 7 * z, r.top(), r.width() - 14 * z, kHeader * z);
        p.drawText(titleRect, Qt::AlignVCenter | Qt::AlignLeft,
                   QFontMetrics(bold).elidedText(CompUi::nodeName(n), Qt::ElideRight, int(titleRect.width())));
        p.setFont(small);
        if (out) {
            p.setPen(Theme::secondary);
            p.drawText(QRectF(r.left(), r.top() + kHeader * z, r.width() - 10 * z, kRow * z),
                       Qt::AlignVCenter | Qt::AlignRight, T("Ausgang"));
        }
        // Input ports with their names
        const QStringList ins = inputsOf(n);
        for (const QString& in : ins) {
            const QPointF pt = inputPos(n.id, in);
            const bool linked = m_graph.linkInto(n.id, in) != nullptr;
            p.setPen(QPen(Theme::border, 1));
            p.setBrush(linked ? Theme::secondary : Theme::controlOff);
            p.drawEllipse(pt, kPort * z, kPort * z);
            p.setPen(Theme::textDim);
            p.drawText(QRectF(pt.x() + 9 * z, pt.y() - kRow * z / 2, r.width() / 2, kRow * z), Qt::AlignVCenter, in);
        }
        // What the node does (its key value), in the last row
        if (const Clip* c = clipPtr) {
            const QString summary = CompUi::nodeSummary(*c, n);
            if (!summary.isEmpty()) {
                const QRectF body(r.left() + 7 * z, r.bottom() - (kRow + 4) * z, r.width() - 14 * z, kRow * z);
                // An fx node whose custom code is skipped (not trusted) says so in warning color
                p.setPen(CompUi::untrustedCode(*c, n.id).isEmpty() ? Theme::text : Theme::warning);
                p.drawText(body, Qt::AlignVCenter | Qt::AlignLeft,
                           QFontMetrics(small).elidedText(summary, Qt::ElideRight, int(body.width())));
            }
        }
        // Output port
        p.setPen(QPen(Theme::border, 1));
        p.setBrush(Theme::primary);
        p.drawEllipse(outputPos(n.id), kPort * z, kPort * z);
    }
}

void CompCanvas::mousePressEvent(QMouseEvent* e)
{
    setFocus();
    m_press = m_mouse = e->position();
    m_moved = false;
    if (e->button() == Qt::MiddleButton) {
        m_drag = Drag::Pan;
        m_pressPlace = m_view.offset;
        return;
    }
    if (e->button() != Qt::LeftButton || !m_clipId) return;
    if (const int from = outputAt(e->position())) { // a new wire from this output
        m_drag = Drag::Wire;
        m_wireFrom = from;
        m_detachTo = 0;
        m_detachInput.clear();
        update();
        return;
    }
    QString input;
    if (const int to = inputAt(e->position(), &input)) {
        if (const CompLink* l = m_graph.linkInto(to, input)) { // grab the link at its end: move or remove it
            m_drag = Drag::Wire;
            m_wireFrom = l->from;
            m_detachTo = to;
            m_detachInput = input;
            update();
            return;
        }
    }
    if (const int node = nodeAt(e->position())) {
        setSelected(node);
        m_drag = Drag::Node;
        m_pressPlace = place(node);
        return;
    }
    setSelected(0);
    m_drag = Drag::Pan;
    m_pressPlace = m_view.offset;
}

void CompCanvas::mouseMoveEvent(QMouseEvent* e)
{
    m_mouse = e->position();
    const QPointF d = e->position() - m_press;
    if (std::abs(d.x()) + std::abs(d.y()) >= 3) m_moved = true;
    switch (m_drag) {
    case Drag::Pan:
        m_view.offset = m_pressPlace - d / m_view.zoom;
        m_view.set = true;
        update();
        break;
    case Drag::Node:
        if (m_moved) {
            m_places[m_selected] = m_pressPlace + d / m_view.zoom;
            update();
        }
        break;
    case Drag::Wire: update(); break;
    case Drag::None: {
        QString in;
        setCursor(outputAt(e->position()) || inputAt(e->position(), &in) ? Qt::CrossCursor : Qt::ArrowCursor);
        break;
    }
    }
}

void CompCanvas::mouseReleaseEvent(QMouseEvent* e)
{
    const Drag drag = std::exchange(m_drag, Drag::None);
    m_mouse = e->position();
    QString err;
    if (drag == Drag::Node && m_selected) {
        const QPointF d = e->position() - m_press;
        m_moved = m_moved || std::abs(d.x()) + std::abs(d.y()) >= 3;
        if (m_moved) m_places[m_selected] = m_pressPlace + d / m_view.zoom;
    }
    if (drag == Drag::Node && m_moved && m_selected) {
        // An unplaced graph gets every shown place, so the layout stays as the user saw it
        QHash<int, QPointF> places;
        if (m_unplaced) places = m_places;
        else places.insert(m_selected, m_places.value(m_selected));
        report(m_editor->moveCompNodes(m_clipId, places, &err), err);
    } else if (drag == Drag::Wire) {
        QString input;
        const int to = inputAt(e->position(), &input);
        const bool same = to && to == m_detachTo && input == m_detachInput;
        if (!same) {
            QUndoStack* undo = m_editor->project()->undoStack();
            const bool both = m_detachTo && to; // moving a link = one undo step of two ops
            if (both) undo->beginMacro(T("Verbindung verschieben"));
            bool ok = true;
            if (m_detachTo)
                ok = m_editor->linkCompNodes(m_clipId, CompLink{m_wireFrom, m_detachTo, m_detachInput}, false,
                                             CompUi::kinds(), &err);
            if (ok && to)
                ok = m_editor->linkCompNodes(m_clipId, CompLink{m_wireFrom, to, input}, true, CompUi::kinds(), &err);
            if (both) undo->endMacro();
            if (!ok && both) undo->undo(); // half a move (the new link would make a cycle): back as it was
            report(ok, err);
        }
    }
    m_detachTo = 0;
    m_detachInput.clear();
    refresh();
}

void CompCanvas::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (const int node = nodeAt(e->position())) emit showRequested(node);
}

void CompCanvas::wheelEvent(QWheelEvent* e)
{
    const double steps = e->angleDelta().y() / 120.0;
    if (steps == 0) return;
    const QPointF at = e->position();
    const QPointF world = toWorld(at);
    m_view.zoom = std::clamp(m_view.zoom * std::pow(1.15, steps), kMinZoom, kMaxZoom);
    m_view.offset = world - at / m_view.zoom; // the point under the mouse stays
    m_view.set = true;
    update();
    e->accept();
}

namespace {
// The node search: Tab, or Shift+Space as in other node compositors (those who switch find it where they look)
bool isSearchKey(const QKeyEvent* k)
{
    const Qt::KeyboardModifiers m = k->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
    return (k->key() == Qt::Key_Tab && m == Qt::NoModifier) || (k->key() == Qt::Key_Space && m == Qt::ShiftModifier);
}
} // namespace

bool CompCanvas::event(QEvent* e)
{
    if (e->type() == QEvent::ShortcutOverride) {
        const auto* k = static_cast<QKeyEvent*>(e);
        const bool ctrlD = k->key() == Qt::Key_D && (k->modifiers() & Qt::ControlModifier);
        const bool plain = !(k->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier));
        if (ctrlD || isSearchKey(k) || (plain && (k->key() == Qt::Key_Delete || k->key() == Qt::Key_Backspace || k->key() == Qt::Key_Tab
                                || k->key() == Qt::Key_F))) {
            e->accept();
            return true;
        }
    }
    return QWidget::event(e);
}

void CompCanvas::keyPressEvent(QKeyEvent* e)
{
    if (isSearchKey(e)) {
        openSearch();
        return;
    }
    if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
        removeSelected();
        return;
    }
    if (e->key() == Qt::Key_D && (e->modifiers() & Qt::ControlModifier)) {
        duplicateSelected();
        return;
    }
    if (e->key() == Qt::Key_F && e->modifiers() == Qt::NoModifier) {
        fit();
        return;
    }
    QWidget::keyPressEvent(e);
}

void CompCanvas::contextMenuEvent(QContextMenuEvent* e)
{
    if (!m_clipId) return;
    QMenu menu(this);
    const int node = nodeAt(e->pos());
    if (node) {
        setSelected(node);
        connect(menu.addAction(T("Im Viewer zeigen")), &QAction::triggered, this, [this, node] { emit showRequested(node); });
        QAction* out = menu.addAction(T("Als Ausgang setzen"));
        out->setEnabled(node != m_graph.out);
        connect(out, &QAction::triggered, this, [this, node] { setOut(node); });
        menu.addSeparator();
        connect(menu.addAction(T("Duplizieren") + "\tCtrl+D"), &QAction::triggered, this, &CompCanvas::duplicateSelected);
        connect(menu.addAction(T("Entfernen") + "\tDel"), &QAction::triggered, this, &CompCanvas::removeSelected);
    } else {
        m_mouse = e->pos();
        connect(menu.addAction(T("Knoten hinzufügen…") + "\tTab"), &QAction::triggered, this, &CompCanvas::openSearch);
        connect(menu.addAction(T("Einpassen") + "\tF"), &QAction::triggered, this, &CompCanvas::fit);
    }
    menu.exec(e->globalPos());
}
