#include "ui/timeline/TimelinePanel.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "ui/timeline/TimelineView.h"

#include <QAction>
#include <QButtonGroup>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace {
// Draws an icon into a 20×20 field (double resolution for sharp edges)
QPixmap drawIcon(TimelinePanel::Icon icon, const QColor& c)
{
    using I = TimelinePanel::Icon;
    QPixmap pm(40, 40);
    pm.setDevicePixelRatio(2);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(c, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    switch (icon) {
    case I::Select: { // mouse arrow
        const QPointF pts[] = { { 6, 3 }, { 6, 16 }, { 9.3, 12.8 }, { 11.6, 17.6 }, { 13.6, 16.7 }, { 11.3, 12 }, { 15.8, 12 } };
        p.setBrush(c);
        p.setPen(QPen(c, 0.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolygon(pts, 7);
        break;
    }
    case I::Trim: { // [◁|▷] – box with cut and arrows pointing outwards
        p.drawRoundedRect(QRectF(2.5, 5, 15, 10), 1.5, 1.5);
        p.drawLine(QPointF(10, 3), QPointF(10, 17));
        p.setBrush(c);
        p.setPen(Qt::NoPen);
        const QPointF l[] = { { 5, 10 }, { 8, 7.5 }, { 8, 12.5 } };
        const QPointF r[] = { { 15, 10 }, { 12, 7.5 }, { 12, 12.5 } };
        p.drawPolygon(l, 3);
        p.drawPolygon(r, 3);
        break;
    }
    case I::Blade: { // razor blade with slot
        QPainterPath path;
        path.addRoundedRect(QRectF(2.5, 5.5, 15, 9), 1.5, 1.5);
        QPainterPath slot;
        slot.addRoundedRect(QRectF(6, 9, 8, 2), 1, 1);
        p.drawPath(path);
        p.setBrush(c);
        p.setPen(Qt::NoPen);
        p.drawPath(slot);
        p.drawRect(QRectF(9.2, 5.5, 1.6, 2.2));
        p.drawRect(QRectF(9.2, 12.3, 1.6, 2.2));
        break;
    }
    case I::Snap: { // horseshoe magnet, slanted
        p.translate(10, 10);
        p.rotate(45);
        p.translate(-10, -10);
        QPainterPath u;
        u.moveTo(5.5, 8);
        u.lineTo(5.5, 11);
        u.arcTo(QRectF(5.5, 6.5, 9, 9), 180, 180);
        u.lineTo(14.5, 8);
        p.setPen(QPen(c, 2.6, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
        p.drawPath(u);
        p.drawLine(QPointF(5.5, 3.5), QPointF(5.5, 6.8)); // poles with a small gap
        p.drawLine(QPointF(14.5, 3.5), QPointF(14.5, 6.8));
        break;
    }
    case I::Link: { // two chain links, slanted
        p.translate(10, 10);
        p.rotate(-45);
        p.translate(-10, -10);
        p.setPen(QPen(c, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawRoundedRect(QRectF(1.5, 7.3, 9.5, 5.4), 2.7, 2.7);
        p.drawRoundedRect(QRectF(9, 7.3, 9.5, 5.4), 2.7, 2.7);
        break;
    }
    case I::Slip: { // [<->] – the clip stays, its content moves inside (like the slip cursor)
        p.drawRoundedRect(QRectF(2.5, 5, 15, 10), 1.5, 1.5);
        p.drawLine(QPointF(6, 10), QPointF(14, 10));
        p.drawPolyline(QPolygonF{{8.2, 7.8}, {6, 10}, {8.2, 12.2}});
        p.drawPolyline(QPolygonF{{11.8, 7.8}, {14, 10}, {11.8, 12.2}});
        break;
    }
    case I::Slide: { // <-[ ]-> – the clip moves between its neighbours (like the slide cursor)
        p.drawRoundedRect(QRectF(6.5, 6, 7, 8), 1.2, 1.2);
        p.drawLine(QPointF(1.5, 10), QPointF(4.8, 10));
        p.drawLine(QPointF(15.2, 10), QPointF(18.5, 10));
        p.drawPolyline(QPolygonF{{3.7, 7.8}, {1.5, 10}, {3.7, 12.2}});
        p.drawPolyline(QPolygonF{{16.3, 7.8}, {18.5, 10}, {16.3, 12.2}});
        break;
    }
    }
    return pm;
}
} // namespace

QIcon TimelinePanel::toolIcon(Icon icon)
{
    // off = gray, on = bright (selection arrow in the primary color)
    const QColor off = Theme::textDim, hover = Theme::mix(Theme::textDim, Theme::text, 0.75);
    const QColor on = icon == Icon::Select ? Theme::primary : Theme::mix(Theme::text, Theme::readableOn(Theme::panelHeader), 0.5);
    QIcon ic;
    ic.addPixmap(drawIcon(icon, off), QIcon::Normal, QIcon::Off);
    ic.addPixmap(drawIcon(icon, hover), QIcon::Active, QIcon::Off);
    ic.addPixmap(drawIcon(icon, on), QIcon::Normal, QIcon::On);
    ic.addPixmap(drawIcon(icon, on), QIcon::Active, QIcon::On);
    ic.addPixmap(drawIcon(icon, Theme::controlOff), QIcon::Disabled, QIcon::Off);
    return ic;
}

TimelinePanel::TimelinePanel(Editor* editor, QWidget* parent) : QWidget(parent), m_editor(editor)
{
    setObjectName("Panel");
    m_view = new TimelineView(editor);

    // --- Toolbar (like the bar above the DaVinci timeline) ---
    auto makeTool = [](Icon icon, const QString& tip) {
        auto* b = new QToolButton;
        b->setIcon(toolIcon(icon));
        b->setIconSize(QSize(20, 20));
        b->setToolTip(tip);
        b->setCheckable(true);
        return b;
    };
    auto* selectBtn = makeTool(Icon::Select, T("Auswahl-Werkzeug (A)"));
    auto* trimBtn = makeTool(Icon::Trim, T("Trim-Modus (T): Kante = Ripple, Schnitt = Roll, Clip = Slip, Titelleiste = Slide"));
    auto* slipBtn = makeTool(Icon::Slip, T("Slip-Werkzeug: Clip ziehen verschiebt seinen Inhalt, Lage und Länge bleiben"));
    auto* slideBtn = makeTool(Icon::Slide, T("Slide-Werkzeug: Clip ziehen verschiebt ihn zwischen seinen Nachbarn"));
    auto* bladeBtn = makeTool(Icon::Blade, T("Klingen-Werkzeug (B)"));
    auto* snapBtn = makeTool(Icon::Snap, T("Snapping an/aus (N)"));
    selectBtn->setChecked(true);
    snapBtn->setChecked(m_view->snapping());

    using Tool = TimelineView::Tool;
    const QVector<QPair<QToolButton*, Tool>> toolButtons = {
        {selectBtn, Tool::Select}, {trimBtn, Tool::Trim}, {slipBtn, Tool::Slip}, {slideBtn, Tool::Slide},
        {bladeBtn, Tool::Blade}};
    selectBtn->setObjectName("tool_select"); // the ids of the menu actions (tests find the buttons by them)
    trimBtn->setObjectName("tool_trim");
    slipBtn->setObjectName("tool_slip");
    slideBtn->setObjectName("tool_slide");
    bladeBtn->setObjectName("tool_blade");
    auto* tools = new QButtonGroup(this);
    for (const auto& [b, tool] : toolButtons) {
        tools->addButton(b);
        connect(b, &QToolButton::clicked, m_view, [this, t = tool] { m_view->setTool(t); });
    }
    connect(m_view, &TimelineView::toolChanged, this, [toolButtons](Tool t) {
        for (const auto& [b, tool] : toolButtons)
            if (tool == t) b->setChecked(true);
    });
    connect(snapBtn, &QToolButton::toggled, m_view, &TimelineView::setSnapping);
    connect(m_view, &TimelineView::snappingChanged, snapBtn, &QToolButton::setChecked);

    auto* zoomOut = new QToolButton;
    zoomOut->setText("−");
    zoomOut->setToolTip(T("Herauszoomen (Strg+-)"));
    auto* zoomIn = new QToolButton;
    zoomIn->setText("+");
    zoomIn->setToolTip(T("Hineinzoomen (Strg+=)"));
    connect(zoomOut, &QToolButton::clicked, m_view, [this] { m_view->zoomBy(1 / 1.5); });
    connect(zoomIn, &QToolButton::clicked, m_view, [this] { m_view->zoomBy(1.5); });

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(6, 3, 6, 3);
    bar->setSpacing(2);
    for (const auto& [b, tool] : toolButtons) bar->addWidget(b);
    auto* sep = new QWidget; // thin separator line like in DaVinci
    sep->setFixedSize(1, 18);
    sep->setStyleSheet(QString("background: %1;").arg(Theme::controlLight.name()));
    bar->addSpacing(8);
    bar->addWidget(sep);
    bar->addSpacing(8);
    bar->addWidget(snapBtn);
    m_bar = bar;
    m_toolInsert = bar->count();
    bar->addStretch(1);
    // Timeline selection (like the Timeline menu above the DaVinci viewer): all timelines, plus an opened compound clip
    m_back = new QToolButton;
    m_back->setText(QStringLiteral("◂"));
    m_back->setToolTip(T("Zurück zur übergeordneten Timeline"));
    connect(m_back, &QToolButton::clicked, this, &TimelinePanel::back);
    m_timelines = new QComboBox;
    m_timelines->setToolTip(T("Timeline wechseln"));
    m_timelines->setMinimumWidth(160);
    m_timelines->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    connect(m_timelines, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        openSequence(m_timelines->itemData(i).toInt());
    });
    bar->addWidget(m_back);
    bar->addWidget(m_timelines);
    bar->addStretch(1);
    Project* project = editor->project();
    connect(project, &Project::sequencesChanged, this, &TimelinePanel::rebuildTimelines);
    connect(project, &Project::currentSequenceChanged, this, [this] {
        // Opened differently (undo, load, delete): keep the history only as long as it still fits
        const Project* p = m_editor->project();
        const Sequence* cur = p->sequence(p->currentSequence());
        if (!cur || !cur->compound) m_history.clear();
        m_history.erase(std::remove_if(m_history.begin(), m_history.end(), [p](int id) { return !p->sequence(id); }),
                        m_history.end());
        rebuildTimelines();
    });
    bar->addWidget(zoomOut);
    bar->addWidget(zoomIn);

    auto* barWidget = new QWidget;
    barWidget->setObjectName("Panel");
    // Switches show their state only via the icon color (no background as in DaVinci)
    barWidget->setStyleSheet(QString("QWidget#Panel { background: %1; }"
                                     "QToolButton { padding: 3px 5px; }"
                                     "QToolButton:checked { background: transparent; }"
                                     "QToolButton:checked:hover, QToolButton:hover { background: %2; }")
                                 .arg(Theme::panelHeader.name(), Theme::control.name()));
    barWidget->setLayout(bar);

    // --- Timeline + Scrollbars ---
    m_hbar = new QScrollBar(Qt::Horizontal);
    m_vbar = new QScrollBar(Qt::Vertical);
    connect(m_hbar, &QScrollBar::valueChanged, m_view, [this](int v) { m_view->setLeftFrame(v); });
    connect(m_vbar, &QScrollBar::valueChanged, m_view, &TimelineView::setScrollY);
    connect(m_view, &TimelineView::viewChanged, this, &TimelinePanel::syncScrollbars);

    auto* grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(0);
    grid->addWidget(m_view, 0, 0);
    grid->addWidget(m_vbar, 0, 1);
    grid->addWidget(m_hbar, 1, 0);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(barWidget);
    lay->addLayout(grid, 1);

    syncScrollbars();
    rebuildTimelines();
}

void TimelinePanel::syncScrollbars()
{
    // Scrollbars only mirror the ViewState – they never reset it here
    const QSignalBlocker bh(m_hbar), bv(m_vbar);
    const auto& v = m_view->view();
    const int vis = std::max(1, m_view->visibleFrames());
    m_hbar->setRange(0, m_view->scrollRangeFrames() - vis);
    m_hbar->setPageStep(vis);
    m_hbar->setSingleStep(std::max(1, vis / 20));
    m_hbar->setValue(int(v.leftFrame));

    const int vp = std::max(1, m_view->viewportHeight());
    m_vbar->setRange(0, std::max(0, m_view->contentHeight() - vp));
    m_vbar->setPageStep(vp);
    m_vbar->setSingleStep(20);
    m_vbar->setValue(v.scrollY);
}

void TimelinePanel::rebuildTimelines()
{
    const Project* p = m_editor->project();
    const QSignalBlocker block(m_timelines);
    m_timelines->clear();
    for (const Sequence& s : p->sequences())
        if (!s.compound || s.id == p->currentSequence())
            m_timelines->addItem(s.compound ? QStringLiteral("◆ ") + s.name : s.name, s.id);
    m_timelines->setCurrentIndex(m_timelines->findData(p->currentSequence()));
    m_back->setVisible(canGoBack());
}

void TimelinePanel::openSequence(int id)
{
    Project* p = m_editor->project();
    const Sequence* s = p->sequence(id);
    if (!s || id == p->currentSequence()) return;
    if (s->compound) m_history << p->currentSequence(); // compound clip: remember the way back
    else m_history.clear();
    const QVector<int> history = m_history;
    p->setCurrentSequence(id);
    m_history = history; // currentSequenceChanged may have cleared it
    rebuildTimelines();
}

void TimelinePanel::back()
{
    if (m_history.isEmpty()) return;
    const int id = m_history.takeLast();
    const QVector<int> history = m_history;
    m_editor->project()->setCurrentSequence(id);
    m_history = history;
    rebuildTimelines();
}

bool TimelinePanel::canGoBack() const
{
    return !m_history.isEmpty();
}

void TimelinePanel::addToolAction(QAction* action, Icon icon)
{
    auto* b = new QToolButton;
    b->setDefaultAction(action);
    b->setIcon(toolIcon(icon)); // setDefaultAction would otherwise take over menu text/icon
    b->setIconSize(QSize(20, 20));
    b->setToolButtonStyle(Qt::ToolButtonIconOnly);
    b->setToolTip(action->toolTip());
    const QIcon ic = b->icon();
    connect(action, &QAction::changed, b, [b, ic] { b->setIcon(ic); });
    m_bar->insertWidget(m_toolInsert++, b);
}
