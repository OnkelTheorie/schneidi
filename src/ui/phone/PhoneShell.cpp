// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/phone/PhoneShell.h"

#include "app/Theme.h"
#include "core/I18n.h"

#include <QApplication>
#include <QBoxLayout>
#include <QEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolButton>
#include <algorithm>

namespace {

constexpr int kIconPx = 22;
constexpr double kMinShare = 0.3, kMaxShare = 0.95;

// A symbol of the UI font (the same ones the desktop's bars use) as an icon, in the colors the page bar gives its
// text: dim, the hovered text color, the primary color for the open panel (QIcon::On)
QIcon glyphIcon(const QString& symbol, int px)
{
    const auto draw = [&](const QColor& color) {
        const qreal dpr = qApp->devicePixelRatio();
        QPixmap pm(QSize(px, px) * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);
        QFont f = QApplication::font();
        f.setPixelSize(px - 2);
        p.setFont(f);
        p.setPen(color);
        p.drawText(QRect(0, 0, px, px), Qt::AlignCenter, symbol);
        return pm;
    };
    QIcon icon;
    icon.addPixmap(draw(Theme::textDim), QIcon::Normal, QIcon::Off);
    icon.addPixmap(draw(Theme::text), QIcon::Active, QIcon::Off);
    icon.addPixmap(draw(Theme::primary), QIcon::Normal, QIcon::On);
    icon.addPixmap(draw(Theme::primary), QIcon::Active, QIcon::On);
    icon.addPixmap(draw(Theme::textDim.darker(140)), QIcon::Disabled, QIcon::Off);
    return icon;
}

} // namespace

bool PhoneShell::isPhoneSize(const QSize& logical)
{
    const int edge = std::min(logical.width(), logical.height());
    return edge > 0 && edge < kPhoneEdge;
}

PhoneShell::PhoneShell(QWidget* parent) : QWidget(parent)
{
    setObjectName("PhoneShell");
    setAttribute(Qt::WA_StyledBackground);
    setStyleSheet(QString("QWidget#PhoneShell { background: %1; }").arg(Theme::border.name()));

    // The tool bar: the desktop page bar's look (dim names, the open one in the primary color, underlined)
    m_toolScroll = new QScrollArea(this);
    m_toolScroll->setObjectName("PhoneTools");
    m_toolScroll->setFrameShape(QFrame::NoFrame);
    m_toolScroll->setWidgetResizable(true);
    m_toolScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_toolScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_toolBox = new QWidget;
    m_toolBox->setObjectName("PhoneToolBox");
    m_toolBox->setStyleSheet(
        QString("QWidget#PhoneToolBox { background: %1; }"
                "QToolButton { color: %2; padding: 6px 8px 4px 8px; border-radius: 0; min-width: 58px; min-height: 50px;"
                " border-bottom: 2px solid transparent; }"
                "QToolButton:checked { color: %3; background: transparent; border-bottom: 2px solid %3; }"
                "QToolButton:hover { color: %4; }"
                "QToolButton:pressed { background: %5; }")
            .arg(Theme::window.name(), Theme::textDim.name(), Theme::primary.name(), Theme::text.name(),
                 Theme::control.name()));
    m_toolLay = new QBoxLayout(QBoxLayout::LeftToRight, m_toolBox);
    m_toolLay->setContentsMargins(4, 0, 4, 0);
    m_toolLay->setSpacing(0);
    m_mainTools = new QWidget;
    m_mainLay = new QBoxLayout(QBoxLayout::LeftToRight, m_mainTools);
    m_mainLay->setContentsMargins(0, 0, 0, 0);
    m_mainLay->setSpacing(0);
    m_clipTools = new QWidget;
    m_clipLay = new QBoxLayout(QBoxLayout::LeftToRight, m_clipTools);
    m_clipLay->setContentsMargins(0, 0, 0, 0);
    m_clipLay->setSpacing(0);
    m_clipTools->hide();
    m_toolLay->addWidget(m_mainTools, 1);
    m_toolLay->addWidget(m_clipTools, 1);
    m_toolScroll->setWidget(m_toolBox);
    // The clip bar's first button: back to the main tools (the selection goes)
    auto* back = makeToolButton(QStringLiteral("←"), T("Zurück"));
    back->setObjectName("phoneTool_back");
    connect(back, &QToolButton::clicked, this, [this] { emit clipToolsLeft(); });
    m_clipLay->addWidget(back);

    // The sheet: a header (drag handle, the panel's name, close) above the open panel
    m_sheet = new QWidget(this);
    m_sheet->setObjectName("PhoneSheet");
    m_sheet->setAttribute(Qt::WA_StyledBackground);
    m_sheet->setStyleSheet(QString("QWidget#PhoneSheet { background: %1; border-top: 1px solid %2; }")
                               .arg(Theme::panel.name(), Theme::border.name()));
    m_sheetLay = new QVBoxLayout(m_sheet);
    m_sheetLay->setContentsMargins(0, 0, 0, 0);
    m_sheetLay->setSpacing(0);
    m_sheetHeader = new QWidget;
    m_sheetHeader->setObjectName("PhoneSheetHeader");
    m_sheetHeader->setAttribute(Qt::WA_StyledBackground);
    m_sheetHeader->setStyleSheet(QString("QWidget#PhoneSheetHeader { background: %1; }").arg(Theme::panelHeader.name()));
    auto* headerLay = new QVBoxLayout(m_sheetHeader);
    headerLay->setContentsMargins(8, 6, 4, 2);
    headerLay->setSpacing(2);
    auto* grip = new QWidget;
    grip->setObjectName("PhoneSheetGrip");
    grip->setAttribute(Qt::WA_StyledBackground);
    grip->setFixedSize(40, 4);
    grip->setStyleSheet(QString("QWidget#PhoneSheetGrip { background: %1; border-radius: 2px; }")
                            .arg(Theme::controlLight.name()));
    grip->setAttribute(Qt::WA_TransparentForMouseEvents);
    headerLay->addWidget(grip, 0, Qt::AlignHCenter);
    auto* row = new QHBoxLayout;
    row->setSpacing(4);
    m_sheetTitle = new QLabel;
    m_sheetTitle->setStyleSheet(QString("color: %1; font-weight: 600; padding-left: 4px;").arg(Theme::text.name()));
    m_sheetTitle->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* close = new QToolButton;
    close->setObjectName("phoneSheetClose");
    close->setText(QStringLiteral("✕"));
    close->setToolTip(T("Schließen"));
    connect(close, &QToolButton::clicked, this, &PhoneShell::closeSheet);
    row->addWidget(m_sheetTitle, 1);
    row->addWidget(close);
    headerLay->addLayout(row);
    m_sheetHeader->installEventFilter(this);
    m_sheetLay->addWidget(m_sheetHeader);
    m_sheet->hide();
    relayout();
}

QToolButton* PhoneShell::makeToolButton(const QString& symbol, const QString& label)
{
    auto* b = new QToolButton;
    b->setIcon(glyphIcon(symbol, kIconPx));
    b->setIconSize(QSize(kIconPx, kIconPx));
    b->setText(label);
    b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    b->setSizePolicy(m_landscape ? QSizePolicy::Preferred : QSizePolicy::Expanding,
                     m_landscape ? QSizePolicy::Fixed : QSizePolicy::Preferred);
    b->setFocusPolicy(Qt::NoFocus); // a tap never takes the keyboard focus from the timeline
    // A tool half out of the bar comes in whole when tapped (its checked state stays in sight)
    connect(b, &QToolButton::clicked, this, [this, b] { m_toolScroll->ensureWidgetVisible(b, 0, 0); });
    return b;
}

void PhoneShell::setTopBar(QWidget* bar)
{
    m_top = bar;
    bar->setParent(this);
    m_laidOut = false;
    relayout();
}

void PhoneShell::setViewer(QWidget* viewer)
{
    m_viewer = viewer;
    viewer->setParent(this);
    viewer->show();
    m_laidOut = false;
    relayout();
}

void PhoneShell::setTimeline(QWidget* timeline)
{
    m_timeline = timeline;
    timeline->setParent(this);
    timeline->show();
    m_laidOut = false;
    relayout();
}

PhoneShell::Tool* PhoneShell::tool(const QString& id)
{
    for (Tool& t : m_tools)
        if (t.id == id) return &t;
    return nullptr;
}

void PhoneShell::addPanelTool(const QString& id, const QString& symbol, const QString& label, std::function<QWidget*()> make)
{
    Tool t;
    t.id = id;
    t.label = label;
    t.make = std::move(make);
    t.button = makeToolButton(symbol, label);
    t.button->setObjectName("phoneTool_" + id);
    t.button->setCheckable(true);
    connect(t.button, &QToolButton::clicked, this, [this, id] {
        if (m_open == id) closeSheet();
        else openSheet(id);
    });
    m_mainLay->addWidget(t.button);
    m_tools << t;
    updateButtons();
}

void PhoneShell::addActionTool(const QString& id, const QString& symbol, const QString& label, std::function<void()> run,
                               bool clipBar)
{
    Tool t;
    t.id = id;
    t.label = label;
    t.run = std::move(run);
    t.clip = clipBar;
    t.button = makeToolButton(symbol, label);
    t.button->setObjectName("phoneTool_" + id);
    connect(t.button, &QToolButton::clicked, this, [this, id] {
        if (Tool* t = tool(id); t && t->run) t->run();
    });
    (clipBar ? m_clipLay : m_mainLay)->addWidget(t.button);
    m_tools << t;
}

void PhoneShell::addClipPanelTool(const QString& id, const QString& symbol, const QString& label, const QString& panelId,
                                  std::function<void()> opened)
{
    Tool t;
    t.id = id;
    t.label = label;
    t.panelId = panelId;
    t.run = std::move(opened);
    t.clip = true;
    t.button = makeToolButton(symbol, label);
    t.button->setObjectName("phoneTool_" + id);
    t.button->setCheckable(true);
    connect(t.button, &QToolButton::clicked, this, [this, id, panelId] {
        if (m_open == panelId && m_openedBy == id) {
            closeSheet();
            return;
        }
        openSheet(panelId);
        if (m_open != panelId) return;
        m_openedBy = id; // this tool is the checked one, not every tool showing the same panel
        updateButtons();
        scrollPanelToTop(panelId);
        if (Tool* t = tool(id); t && t->run) t->run();
    });
    m_clipLay->addWidget(t.button);
    m_tools << t;
    updateButtons();
}

QToolButton* PhoneShell::toolButton(const QString& id) const
{
    if (id == "back") return m_clipTools->findChild<QToolButton*>("phoneTool_back");
    for (const Tool& t : m_tools)
        if (t.id == id) return t.button;
    return nullptr;
}

QWidget* PhoneShell::panel(const QString& id) const
{
    for (const Tool& t : m_tools)
        if (t.id == id && t.scroll) return t.scroll->widget();
    return nullptr;
}

void PhoneShell::scrollPanelToTop(const QString& id)
{
    if (Tool* t = tool(id); t && t->scroll) t->scroll->verticalScrollBar()->setValue(0);
}

QString PhoneShell::panelId(const QWidget* panel) const
{
    for (const Tool& t : m_tools)
        if (t.scroll && t.scroll->widget() == panel) return t.id;
    return {};
}

void PhoneShell::openSheet(const QString& id)
{
    Tool* t = tool(id);
    if (!t || !t->make) return;
    if (!t->scroll) {
        QWidget* p = t->make();
        if (!p) return;
        // The panel's minimum never widens the window: past the room the sheet has, it scrolls inside it
        t->scroll = new QScrollArea;
        t->scroll->setObjectName("phonePanel_" + id);
        t->scroll->setFrameShape(QFrame::NoFrame);
        t->scroll->setWidgetResizable(true);
        t->scroll->setWidget(p);
        m_sheetLay->addWidget(t->scroll, 1);
        // The panel's own title says what the sheet's header says already (its room goes to the panel's tabs)
        for (QLabel* title : p->findChildren<QLabel*>("PanelTitle", Qt::FindDirectChildrenOnly)) title->hide();
    }
    for (Tool& other : m_tools)
        if (other.scroll) other.scroll->setVisible(&other == t);
    if (QWidget* p = t->scroll->widget()) p->show(); // a panel switch of the desktop may have hidden it
    m_sheetTitle->setText(t->label);
    const bool changed = m_open != id;
    m_open = id;
    m_openedBy.clear();
    m_sheet->show();
    m_sheet->raise();
    placeSheet();
    updateButtons();
    if (changed) emit sheetChanged(id);
}

void PhoneShell::closeSheet()
{
    if (m_open.isEmpty()) return;
    m_open.clear();
    m_openedBy.clear();
    m_sheet->hide();
    updateButtons();
    emit sheetChanged({});
}

void PhoneShell::setSheetShare(double share)
{
    m_share = std::clamp(share, kMinShare, kMaxShare);
    placeSheet();
}

void PhoneShell::setClipToolsShown(bool on)
{
    if (m_clipMode == on) return;
    m_clipMode = on;
    m_mainTools->setVisible(!on);
    m_clipTools->setVisible(on);
    m_toolScroll->horizontalScrollBar()->setValue(0);
    m_toolScroll->verticalScrollBar()->setValue(0);
    updateButtons();
}

bool PhoneShell::back()
{
    if (!m_open.isEmpty()) {
        closeSheet();
        return true;
    }
    if (m_clipMode) {
        emit clipToolsLeft(); // MainWindow clears the selection, which switches the bar back
        if (m_clipMode) setClipToolsShown(false);
        return true;
    }
    return false;
}

void PhoneShell::updateButtons()
{
    for (Tool& t : m_tools) {
        if (!t.button->isCheckable()) continue;
        // A main tool: its panel is open. A clip tool (Volume -> the Inspector): it opened the sheet itself
        const bool on = t.panelId.isEmpty() ? !m_open.isEmpty() && t.id == m_open : t.panelId == m_open && m_openedBy == t.id;
        t.button->setChecked(on);
    }
    // The bar's thickness follows its buttons (one row / one column): the largest button, not the box's hint (stale
    // right after the direction changed)
    QSize hint;
    for (QToolButton* b : m_toolBox->findChildren<QToolButton*>()) hint = hint.expandedTo(b->sizeHint());
    // Portrait: a button never narrower than its label ("Ins…tor"); the bar scrolls instead. When the tools do not
    // fit, they are widened so that the last one in sight is cut in half: the cut says "there is more, swipe"
    for (QWidget* bar : {m_mainTools, m_clipTools}) {
        const QList<QToolButton*> buttons = bar->findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly);
        const int spacing = std::max(0, static_cast<QBoxLayout*>(bar->layout())->spacing());
        const int room = width() - m_toolLay->contentsMargins().left() - m_toolLay->contentsMargins().right();
        // Each button at its own label's width; when they do not fit, all widen by one factor so that the first one
        // past the edge is cut in half (a long label such as "Geschwindigkeit" never widens the short ones alone)
        QVector<int> hints;
        int total = 0;
        for (QToolButton* b : buttons) {
            if (b->isHidden()) continue;
            hints << b->sizeHint().width() + spacing;
            total += hints.back();
        }
        double factor = 1.0;
        if (!m_landscape && total > room && room > 0) {
            int whole = 0, sum = 0;
            while (whole < hints.size() && sum + hints[whole] <= room) sum += hints[whole++];
            if (whole < hints.size()) {
                const double wanted = sum + 0.5 * hints[whole];
                factor = std::max(1.0, room / wanted);
                // A button barely in sight (less than a third) says nothing: one button less in sight, wider ones
                if (room - sum < hints[whole] / 3 && whole > 0) factor = std::max(1.0, room / (sum - hints[whole - 1] * 0.5));
            }
        }
        for (QToolButton* b : buttons)
            b->setMinimumWidth(m_landscape ? 0 : int((b->sizeHint().width() + spacing) * factor) - spacing);
    }
    const QMargins m = m_toolLay->contentsMargins();
    hint += QSize(m.left() + m.right(), m.top() + m.bottom());
    if (m_landscape) {
        // The rail as wide as its widest tool or the top bar's head, whichever is wider
        const int rail = std::max(hint.width(), m_top ? m_top->minimumSizeHint().width() : 0);
        m_toolScroll->setFixedWidth(rail);
        m_toolScroll->setMinimumHeight(0);
        m_toolScroll->setMaximumHeight(QWIDGETSIZE_MAX);
        if (m_top) m_top->setFixedWidth(rail);
    } else {
        m_toolScroll->setFixedHeight(hint.height());
        m_toolScroll->setMinimumWidth(0);
        m_toolScroll->setMaximumWidth(QWIDGETSIZE_MAX);
        if (m_top) {
            m_top->setMinimumWidth(0);
            m_top->setMaximumWidth(QWIDGETSIZE_MAX);
        }
    }
}

void PhoneShell::relayout()
{
    const bool land = width() > height();
    if (m_laidOut && land == m_landscape) {
        updateButtons(); // (the first show polishes the buttons: their size with the style sheet)
        placeSheet();
        return;
    }
    m_landscape = land;
    m_laidOut = true;
    delete layout(); // the items go, the widgets stay (children of the shell)
    emit landscapeChanged(land);
    const auto dir = land ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
    m_toolLay->setDirection(dir);
    m_mainLay->setDirection(dir);
    m_clipLay->setDirection(dir);
    for (QToolButton* b : m_toolBox->findChildren<QToolButton*>())
        b->setSizePolicy(land ? QSizePolicy::Preferred : QSizePolicy::Expanding,
                         land ? QSizePolicy::Fixed : QSizePolicy::Preferred);
    constexpr int kGap = 1; // the border color between the areas, as thin as the desktop's panel edges
    if (land) {
        // The height is the scarce side: the top bar's buttons head the rail of tools at the left (no bar across);
        // the viewer above the timeline in a column (the timeline gets the larger share: its first tracks in sight);
        // the open panel at the right over the whole height
        auto* root = new QHBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(kGap);
        auto* rail = new QVBoxLayout;
        rail->setSpacing(kGap);
        if (m_top) rail->addWidget(m_top);
        rail->addWidget(m_toolScroll, 1);
        root->addLayout(rail);
        auto* column = new QVBoxLayout;
        column->setSpacing(kGap);
        if (m_viewer) column->addWidget(m_viewer, 1);
        if (m_timeline) column->addWidget(m_timeline, 2);
        root->addLayout(column, 3);
        root->addWidget(m_sheet, 2);
    } else {
        // Top to bottom: bar, viewer, timeline, tools; the sheet lies over the timeline (placeSheet)
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(kGap);
        if (m_top) root->addWidget(m_top);
        if (m_viewer) root->addWidget(m_viewer, 4);
        if (m_timeline) root->addWidget(m_timeline, 5);
        root->addWidget(m_toolScroll);
    }
    m_toolScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_toolScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_sheet->setVisible(!m_open.isEmpty());
    updateButtons();
    layout()->activate();
    placeSheet();
}

void PhoneShell::placeSheet()
{
    if (m_landscape || m_open.isEmpty()) return;
    // Over the timeline (and as far up as its share reaches), above the tool bar; the viewer stays in sight at the
    // usual share
    const int bottom = m_toolScroll->geometry().top() - 1;
    const int top = m_top ? m_top->geometry().bottom() + 1 : 0;
    const int room = std::max(0, bottom - top);
    const int least = std::min(room, m_sheetHeader->sizeHint().height() + 60);
    const int h = std::clamp(int(room * m_share), least, room);
    m_sheet->setGeometry(0, bottom - h, width(), h);
    m_sheet->raise();
}

QSize PhoneShell::minimumSizeHint() const
{
    if (!isVisible()) return {0, 0};
    // Never more than the window's shorter side either way: turned, the window gets the other orientation's size while
    // the layout is still the old one (the portrait layout's height must not hold a landscape window taller)
    const QSize window = this->window()->size();
    const int edge = std::min(window.width(), window.height());
    return QWidget::minimumSizeHint().boundedTo(QSize(edge, edge));
}

void PhoneShell::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    relayout();
}

bool PhoneShell::eventFilter(QObject* obj, QEvent* event)
{
    // Dragging the sheet's header (portrait): taller or lower; let go low enough, it closes
    if (obj == m_sheetHeader && !m_landscape) {
        const int room = std::max(1, m_toolScroll->geometry().top() - (m_top ? m_top->geometry().bottom() : 0));
        switch (event->type()) {
        case QEvent::MouseButtonPress:
            m_dragStartY = int(static_cast<QMouseEvent*>(event)->globalPosition().y());
            m_dragStartShare = m_share;
            return true;
        case QEvent::MouseMove:
            if (m_dragStartY >= 0) {
                const int y = int(static_cast<QMouseEvent*>(event)->globalPosition().y());
                const double share = m_dragStartShare + double(m_dragStartY - y) / room;
                m_share = std::clamp(share, 0.12, kMaxShare); // below kMinShare only while dragging (closing)
                placeSheet();
            }
            return true;
        case QEvent::MouseButtonRelease:
            if (m_dragStartY >= 0) {
                m_dragStartY = -1;
                if (m_share < kMinShare) { // pulled down: closed, the next one opens as tall as before
                    m_share = m_dragStartShare;
                    closeSheet();
                }
            }
            return true;
        default: break;
        }
    }
    return QWidget::eventFilter(obj, event);
}
