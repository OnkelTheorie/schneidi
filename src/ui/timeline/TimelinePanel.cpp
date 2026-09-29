#include "ui/timeline/TimelinePanel.h"

#include "core/I18n.h"
#include "ui/timeline/TimelineView.h"

#include <QAction>
#include <QButtonGroup>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
// Zeichnet ein Symbol in ein 20×20-Feld (doppelt aufgelöst für scharfe Kanten)
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
    case I::Select: { // Mauspfeil
        const QPointF pts[] = { { 6, 3 }, { 6, 16 }, { 9.3, 12.8 }, { 11.6, 17.6 }, { 13.6, 16.7 }, { 11.3, 12 }, { 15.8, 12 } };
        p.setBrush(c);
        p.setPen(QPen(c, 0.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolygon(pts, 7);
        break;
    }
    case I::Trim: { // [◁|▷] – Kasten mit Schnitt und Pfeilen nach außen
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
    case I::Blade: { // Rasierklinge mit Schlitz
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
    case I::Snap: { // Hufeisenmagnet, schräg
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
        p.drawLine(QPointF(5.5, 3.5), QPointF(5.5, 6.8)); // Pole mit kleiner Lücke
        p.drawLine(QPointF(14.5, 3.5), QPointF(14.5, 6.8));
        break;
    }
    case I::Link: { // zwei Kettenglieder, schräg
        p.translate(10, 10);
        p.rotate(-45);
        p.translate(-10, -10);
        p.setPen(QPen(c, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawRoundedRect(QRectF(1.5, 7.3, 9.5, 5.4), 2.7, 2.7);
        p.drawRoundedRect(QRectF(9, 7.3, 9.5, 5.4), 2.7, 2.7);
        break;
    }
    }
    return pm;
}
} // namespace

QIcon TimelinePanel::toolIcon(Icon icon)
{
    // DaVinci: aus = grau, an = weiß (Auswahl-Pfeil rot)
    const QColor off("#8c8c94"), hover("#c4c4ca"), on(icon == Icon::Select ? "#e5484d" : "#ececf0");
    QIcon ic;
    ic.addPixmap(drawIcon(icon, off), QIcon::Normal, QIcon::Off);
    ic.addPixmap(drawIcon(icon, hover), QIcon::Active, QIcon::Off);
    ic.addPixmap(drawIcon(icon, on), QIcon::Normal, QIcon::On);
    ic.addPixmap(drawIcon(icon, on), QIcon::Active, QIcon::On);
    ic.addPixmap(drawIcon(icon, QColor("#55555c")), QIcon::Disabled, QIcon::Off);
    return ic;
}

TimelinePanel::TimelinePanel(Editor* editor, QWidget* parent) : QWidget(parent)
{
    setObjectName("Panel");
    m_view = new TimelineView(editor);

    // --- Werkzeugleiste (wie die Leiste über der DaVinci-Timeline) ---
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
    auto* bladeBtn = makeTool(Icon::Blade, T("Klingen-Werkzeug (B)"));
    auto* snapBtn = makeTool(Icon::Snap, T("Snapping an/aus (N)"));
    selectBtn->setChecked(true);
    snapBtn->setChecked(m_view->snapping());

    auto* tools = new QButtonGroup(this);
    tools->addButton(selectBtn);
    tools->addButton(trimBtn);
    tools->addButton(bladeBtn);
    connect(selectBtn, &QToolButton::clicked, m_view, [this] { m_view->setTool(TimelineView::Tool::Select); });
    connect(trimBtn, &QToolButton::clicked, m_view, [this] { m_view->setTool(TimelineView::Tool::Trim); });
    connect(bladeBtn, &QToolButton::clicked, m_view, [this] { m_view->setTool(TimelineView::Tool::Blade); });
    connect(m_view, &TimelineView::toolChanged, this, [=](TimelineView::Tool t) {
        (t == TimelineView::Tool::Blade ? bladeBtn : t == TimelineView::Tool::Trim ? trimBtn : selectBtn)->setChecked(true);
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
    bar->addWidget(selectBtn);
    bar->addWidget(trimBtn);
    bar->addWidget(bladeBtn);
    auto* sep = new QWidget; // dünne Trennlinie wie in DaVinci
    sep->setFixedSize(1, 18);
    sep->setStyleSheet("background: #45454d;");
    bar->addSpacing(8);
    bar->addWidget(sep);
    bar->addSpacing(8);
    bar->addWidget(snapBtn);
    m_bar = bar;
    m_toolInsert = bar->count();
    bar->addStretch(1);
    bar->addWidget(zoomOut);
    bar->addWidget(zoomIn);

    auto* barWidget = new QWidget;
    barWidget->setObjectName("Panel");
    // Schalter zeigen ihren Zustand nur über die Symbolfarbe (kein Hintergrund wie in DaVinci)
    barWidget->setStyleSheet("QWidget#Panel { background: #2f2f35; }"
                             "QToolButton { padding: 3px 5px; }"
                             "QToolButton:checked { background: transparent; }"
                             "QToolButton:checked:hover, QToolButton:hover { background: #3a3a42; }");
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
}

void TimelinePanel::syncScrollbars()
{
    // Scrollbars spiegeln nur den ViewState – sie setzen ihn hier nie zurück
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

void TimelinePanel::addToolAction(QAction* action, Icon icon)
{
    auto* b = new QToolButton;
    b->setDefaultAction(action);
    b->setIcon(toolIcon(icon)); // setDefaultAction übernimmt sonst Menütext/-symbol
    b->setIconSize(QSize(20, 20));
    b->setToolButtonStyle(Qt::ToolButtonIconOnly);
    b->setToolTip(action->toolTip());
    const QIcon ic = b->icon();
    connect(action, &QAction::changed, b, [b, ic] { b->setIcon(ic); });
    m_bar->insertWidget(m_toolInsert++, b);
}
