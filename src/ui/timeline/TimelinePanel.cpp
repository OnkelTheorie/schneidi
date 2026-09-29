#include "ui/timeline/TimelinePanel.h"

#include "core/I18n.h"
#include "ui/timeline/TimelineView.h"

#include <QAction>
#include <QButtonGroup>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

TimelinePanel::TimelinePanel(Editor* editor, QWidget* parent) : QWidget(parent)
{
    setObjectName("Panel");
    m_view = new TimelineView(editor);

    // --- Werkzeugleiste (wie die Leiste über der DaVinci-Timeline) ---
    auto makeTool = [](const QString& text, const QString& tip) {
        auto* b = new QToolButton;
        b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(true);
        return b;
    };
    auto* selectBtn = makeTool(T("⮝ Auswahl"), T("Auswahl-Werkzeug (A)"));
    auto* trimBtn = makeTool(T("⇹ Trimmen"), T("Trim-Modus (T): Kante = Ripple, Schnitt = Roll, Clip = Slip, Titelleiste = Slide"));
    auto* bladeBtn = makeTool(T("✂ Klinge"), T("Klingen-Werkzeug (B)"));
    auto* snapBtn = makeTool(T("⊸ Snapping"), T("Snapping an/aus (N)"));
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
    bar->addSpacing(12);
    bar->addWidget(snapBtn);
    m_bar = bar;
    m_toolInsert = bar->count();
    bar->addStretch(1);
    bar->addWidget(zoomOut);
    bar->addWidget(zoomIn);

    auto* barWidget = new QWidget;
    barWidget->setObjectName("Panel");
    barWidget->setStyleSheet("QWidget#Panel { background: #2f2f35; }");
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

void TimelinePanel::addToolAction(QAction* action, const QString& text)
{
    auto* b = new QToolButton;
    b->setDefaultAction(action);
    b->setText(text); // setDefaultAction übernimmt sonst den Menütext
    b->setToolTip(action->toolTip());
    connect(action, &QAction::changed, b, [b, text] { b->setText(text); });
    m_bar->insertWidget(m_toolInsert++, b);
}
