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
#include <QScroller>
#include <QScrollerProperties>
#include <QTimer>
#include <QScrollBar>
#include <QToolButton>
#include <algorithm>

namespace {

constexpr int kIconPx = 22;
constexpr double kMinShare = 0.3, kMaxShare = 0.95, kDefaultShare = 0.55;

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
    icon.addPixmap(draw(Theme::text), QIcon::Normal, QIcon::Off);
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
        // As phones' own tool bars: no frames, a small symbol over a short small label in the text color, the open
        // one in the accent color with a short line under it; items as wide as their label (at least 60 px)
        QString("QWidget#PhoneToolBox { background: %1; }"
                "QToolButton { color: %2; background: transparent; border: none; padding: 6px 4px 6px 4px;"
                " border-radius: 0; min-width: 0; min-height: 0; font-size: 11px; border-bottom: 2px solid transparent; }"
                "QToolButton:checked { color: %3; border-bottom: 2px solid %3; }"
                "QToolButton:pressed { background: %4; }")
            .arg(Theme::window.name(), Theme::text.name(), Theme::primary.name(), Theme::control.name()));
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
    // The clip bar's back arrow: fixed at the bar's start (it never scrolls away), back to the main tools
    m_bar = new QWidget(this);
    m_bar->setObjectName("PhoneBar");
    m_bar->setAttribute(Qt::WA_StyledBackground);
    m_bar->setStyleSheet(QString("QWidget#PhoneBar { background: %1; border-top: 1px solid %2; }"
                                 "QToolButton#phoneTool_back { color: %3; background: transparent; border: none;"
                                 " border-right: 1px solid %2; min-width: 0; padding: 0 10px; }"
                                 "QToolButton#phoneTool_back:pressed { background: %4; }")
                             .arg(Theme::window.name(), Theme::border.name(), Theme::text.name(), Theme::control.name()));
    m_barLay = new QBoxLayout(QBoxLayout::LeftToRight, m_bar);
    m_barLay->setContentsMargins(0, 0, 0, 0);
    m_barLay->setSpacing(0);
    m_back = new QToolButton;
    m_back->setObjectName("phoneTool_back");
    m_back->setIcon(glyphIcon(QStringLiteral("←"), kIconPx + 4));
    m_back->setIconSize(QSize(kIconPx + 4, kIconPx + 4));
    m_back->setToolTip(T("Zurück"));
    m_back->setFocusPolicy(Qt::NoFocus);
    m_back->hide();
    connect(m_back, &QToolButton::clicked, this, [this] { emit clipToolsLeft(); });
    m_barLay->addWidget(m_back);
    m_barLay->addWidget(m_toolScroll, 1);

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
    b->setToolButtonStyle(m_landscape ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonTextUnderIcon); // (as relayout)
    b->setSizePolicy(m_landscape ? QSizePolicy::Preferred : QSizePolicy::Expanding,
                     m_landscape ? QSizePolicy::Fixed : QSizePolicy::Preferred);
    b->setFocusPolicy(Qt::NoFocus); // a tap never takes the keyboard focus from the timeline
    // Two lines at most: a long label breaks at its space ("Medien generieren"); a single long word stays one line
    if (label.contains(' ') && QFontMetrics(b->font()).horizontalAdvance(label) > 72) b->setText(QString(label).replace(' ', '\n'));
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
    if (id == "back") return m_back;
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
        for (QLabel* title : p->findChildren<QLabel*>("PanelTitle", Qt::FindDirectChildrenOnly))
            if (!title->isHidden()) {
                title->hide();
                title->setProperty("phoneHidTitle", true); // (shown again by releasePanels)
            }
    }
    for (Tool& other : m_tools)
        if (other.scroll) other.scroll->setVisible(&other == t);
    if (QWidget* p = t->scroll->widget()) p->show(); // a panel switch of the desktop may have hidden it
    m_sheetTitle->setText(t->label);
    const bool changed = m_open != id;
    m_open = id;
    m_openedBy.clear();
    if (changed) m_share = m_shares.contains(id) ? m_shares.value(id) : defaultShare(); // its own height, else the usual
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

void PhoneShell::setTablet(bool on)
{
    if (m_tablet == on) return;
    m_tablet = on;
    m_laidOut = false; // the arrangement anew
    relayout();
}

void PhoneShell::releasePanels()
{
    for (Tool& t : m_tools)
        if (t.scroll && t.scroll->widget()) {
            QWidget* p = t.scroll->takeWidget();
            for (QLabel* title : p->findChildren<QLabel*>("PanelTitle", Qt::FindDirectChildrenOnly))
                if (title->property("phoneHidTitle").toBool()) {
                    title->setProperty("phoneHidTitle", QVariant());
                    title->show();
                }
            if (p->property("phoneOwned").toBool()) p->deleteLater(); // a box the shell made (its content goes home)
        }
}

void PhoneShell::setPanelShare(const QString& id, double share)
{
    m_shares[id] = std::clamp(share, kMinShare, kMaxShare);
    if (m_open == id) setSheetShare(share);
}

void PhoneShell::setSheetShare(double share)
{
    m_share = std::clamp(share, kMinShare, kMaxShare);
    if (!m_open.isEmpty()) m_shares[m_open] = m_share;
    placeSheet();
}

void PhoneShell::setClipToolsShown(bool on)
{
    if (m_clipMode == on) return;
    m_clipMode = on;
    m_mainTools->setVisible(!on);
    m_clipTools->setVisible(on);
    m_back->setVisible(on);
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
    // Upright: each item as wide as its label (at least 60 px), the bar scrolls sideways; the one at the edge shows
    // cut, which says there is more. On its side (rail): the rail's width
    for (QToolButton* b : m_toolBox->findChildren<QToolButton*>()) {
        if (m_landscape) {
            b->setMinimumWidth(0);
            b->setMaximumWidth(QWIDGETSIZE_MAX);
        } else {
            b->setFixedWidth(std::max(60, b->sizeHint().width() + 8));
        }
    }
    const QMargins m = m_toolLay->contentsMargins();
    hint += QSize(m.left() + m.right(), m.top() + m.bottom());
    if (m_landscape) {
        // The rail as wide as its widest tool or the top bar's head, whichever is wider
        const int rail = std::max(hint.width(), m_top && !m_tablet ? m_top->minimumSizeHint().width() : 0);
        m_toolScroll->setFixedWidth(rail);
        m_back->setFixedSize(rail, 44);
        m_toolScroll->setMinimumHeight(0);
        m_toolScroll->setMaximumHeight(QWIDGETSIZE_MAX);
        if (m_top && !m_tablet) m_top->setFixedWidth(rail);
        else if (m_top) {
            m_top->setMinimumWidth(0);
            m_top->setMaximumWidth(QWIDGETSIZE_MAX);
        }
    } else {
        m_toolScroll->setFixedHeight(hint.height());
        m_back->setFixedSize(44, hint.height());
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
    const bool land = m_tablet || width() > height();
    if (m_laidOut && land == m_landscape) {
        updateButtons(); // (the first show polishes the buttons: their size with the style sheet)
        placeSheet();
        return;
    }
    m_landscape = land;
    m_laidOut = true;
    delete layout(); // the items go, the widgets stay (children of the shell)
    emit landscapeChanged(land && !m_tablet);
    const auto dir = land ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
    m_toolLay->setDirection(dir);
    m_mainLay->setDirection(dir);
    m_clipLay->setDirection(dir);
    for (QToolButton* b : m_toolBox->findChildren<QToolButton*>()) {
        b->setSizePolicy(land ? QSizePolicy::Preferred : QSizePolicy::Expanding,
                         land ? QSizePolicy::Fixed : QSizePolicy::Preferred);
        // On its side the rail has no height for a label under each symbol (it was cut away): beside it
        b->setToolButtonStyle(land ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonTextUnderIcon);
    }
    constexpr int kGap = 1; // the border color between the areas, as thin as the desktop's panel edges
    if (m_tablet) {
        // The top bar across (as on the desktop), below it the tools as a rail, the viewer above the timeline, the
        // open panel docked at the right
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(kGap);
        if (m_top) root->addWidget(m_top);
        auto* row = new QHBoxLayout;
        row->setSpacing(kGap);
        row->addWidget(m_bar);
        auto* column = new QVBoxLayout;
        column->setSpacing(kGap);
        if (m_viewer) column->addWidget(m_viewer, 3);
        if (m_timeline) column->addWidget(m_timeline, 2);
        row->addLayout(column, 3);
        row->addWidget(m_sheet, 2);
        root->addLayout(row, 1);
    } else if (land) {
        // The height is the scarce side: the top bar's buttons head the rail of tools at the left (no bar across);
        // the viewer above the timeline in a column (the timeline gets the larger share: its first tracks in sight);
        // the open panel at the right over the whole height
        auto* root = new QHBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(kGap);
        auto* rail = new QVBoxLayout;
        rail->setSpacing(kGap);
        if (m_top) rail->addWidget(m_top);
        rail->addWidget(m_bar, 1);
        root->addLayout(rail);
        auto* column = new QVBoxLayout;
        column->setSpacing(kGap);
        if (m_viewer) column->addWidget(m_viewer, 1);
        if (m_timeline) column->addWidget(m_timeline, m_timelineHeight ? 0 : 2); // (a height hint: the viewer the rest)
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
        root->addWidget(m_bar);
    }
    m_barLay->setDirection(land ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    refreshTimelineHeight();
    m_toolScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_toolScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_sheet->setVisible(!m_open.isEmpty());
    updateButtons();
    layout()->activate();
    placeSheet();
}

double PhoneShell::defaultShare() const
{
    if (!m_sheetTopHint || m_landscape || m_tablet) return kDefaultShare;
    const int bottom = m_bar->geometry().top() - 1;
    const int top = m_top ? m_top->geometry().bottom() + 1 : 0;
    const int room = bottom - top;
    if (room <= 0) return kDefaultShare;
    // Never higher than the usual share (the viewer stays as large), never lower than the least
    return std::clamp(double(bottom - m_sheetTopHint()) / room, kMinShare, kDefaultShare);
}

void PhoneShell::placeSheet()
{
    if (m_landscape || m_open.isEmpty()) return;
    // Over the timeline (and as far up as its share reaches), above the tool bar; the viewer stays in sight at the
    // usual share
    const int bottom = m_bar->geometry().top() - 1;
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

void PhoneShell::refreshTimelineHeight()
{
    if (!m_timeline) return;
    if (m_tablet || !m_timelineHeight || !m_laidOut) {
        m_timeline->setMinimumHeight(0);
        m_timeline->setMaximumHeight(QWIDGETSIZE_MAX);
        return;
    }
    int h = 0;
    if (m_landscape) {
        // Its first rows, but never more than 60 % of the height (the picture keeps at least the rest) nor less than a
        // third
        const int most = height() * 3 / 5;
        h = std::clamp(m_timelineHeight(most), height() / 3, most);
    } else {
        // Between the top bar and the tools: never taller than its old share (5 of 9 against the viewer: many tracks
        // scroll as before), never lower than 3/10 (room to pan, a sheet over it); in between as tall as its rows, and
        // the viewer gets what is left (a portrait picture grows, a wide one stays as wide as the screen)
        constexpr int kGap = 1;
        const int top = m_top ? std::max(m_top->height(), m_top->sizeHint().height()) + kGap : 0;
        const int bar = std::max(m_bar->height(), m_bar->sizeHint().height()) + kGap;
        const int room = height() - top - bar - kGap;
        if (room <= 0) return;
        const int most = room * 5 / 9;
        h = std::clamp(m_timelineHeight(most), room * 3 / 10, most);
    }
    m_timeline->setMinimumHeight(0);
    m_timeline->setMaximumHeight(h);
    m_timeline->setMinimumHeight(h);
}

void PhoneShell::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    relayout();
    refreshTimelineHeight();
    if (!m_scrollTuned && m_toolScroll->isVisible()) QTimer::singleShot(0, this, [this] { tuneScroller(); });
}

void PhoneShell::tuneScroller()
{
    // The bar's flick as on a phone (after Touch's kinetic filter made its scroller, at the first show): light to
    // drag, a fling that carries and slows down, a soft bounce at both ends (back to the end, never past it to the
    // other side); only along the bar
    if (m_scrollTuned) return;
    m_scrollTuned = true;
    QScroller* sc = QScroller::scroller(m_toolScroll->viewport());
    QScrollerProperties p = sc->scrollerProperties();
    p.setScrollMetric(QScrollerProperties::DecelerationFactor, 0.2);
    p.setScrollMetric(QScrollerProperties::MaximumVelocity, 1.2);
    p.setScrollMetric(QScrollerProperties::MinimumVelocity, 0.02);
    p.setScrollMetric(QScrollerProperties::DragVelocitySmoothingFactor, 0.4);
    p.setScrollMetric(QScrollerProperties::AxisLockThreshold, 0.0);
    p.setScrollMetric(QScrollerProperties::HorizontalOvershootPolicy, QScrollerProperties::OvershootWhenScrollable);
    p.setScrollMetric(QScrollerProperties::VerticalOvershootPolicy, QScrollerProperties::OvershootWhenScrollable);
    p.setScrollMetric(QScrollerProperties::OvershootDragResistanceFactor, 0.35);
    p.setScrollMetric(QScrollerProperties::OvershootDragDistanceFactor, 0.12);
    p.setScrollMetric(QScrollerProperties::OvershootScrollDistanceFactor, 0.06);
    p.setScrollMetric(QScrollerProperties::OvershootScrollTime, 0.35);
    p.setScrollMetric(QScrollerProperties::FrameRate, QScrollerProperties::Fps60);
    sc->setScrollerProperties(p);
}

bool PhoneShell::eventFilter(QObject* obj, QEvent* event)
{
    // Dragging the sheet's header (portrait): taller or lower; let go low enough, it closes
    if (obj == m_sheetHeader && !m_landscape) {
        const int room = std::max(1, m_bar->geometry().top() - (m_top ? m_top->geometry().bottom() : 0));
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
                } else if (!m_open.isEmpty()) {
                    m_shares[m_open] = m_share; // this panel keeps the height it was dragged to
                }
            }
            return true;
        default: break;
        }
    }
    return QWidget::eventFilter(obj, event);
}
