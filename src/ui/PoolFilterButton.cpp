#include "ui/PoolFilterButton.h"

#include "app/Theme.h"
#include "core/I18n.h"
#include "ui/MediaPool.h"

#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

namespace {

// Ticking an entry keeps the menu open (several kinds/flags/colors in one go); other entries close it as usual
class StayOpenMenu : public QMenu {
public:
    using QMenu::QMenu;

protected:
    static bool ticks(const QAction* a) { return a && a->isEnabled() && a->isCheckable() && !a->menu(); }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (QAction* a = actionAt(e->position().toPoint()); ticks(a)) {
            a->trigger();
            return;
        }
        QMenu::mouseReleaseEvent(e);
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        const bool select = e->key() == Qt::Key_Space || e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter;
        if (select && ticks(activeAction())) {
            activeAction()->trigger();
            return;
        }
        QMenu::keyPressEvent(e);
    }
};

// Funnel (filter symbol), drawn so it looks the same on every system font
QIcon funnelIcon(const QColor& color)
{
    QPixmap pm(32, 32);
    pm.setDevicePixelRatio(2);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath funnel;
    funnel.moveTo(1.5, 2.5);
    funnel.lineTo(14.5, 2.5);
    funnel.lineTo(9.5, 8.5);
    funnel.lineTo(9.5, 13.5);
    funnel.lineTo(6.5, 12);
    funnel.lineTo(6.5, 8.5);
    funnel.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawPath(funnel);
    return QIcon(pm);
}

QPixmap swatch(const QColor& c)
{
    QPixmap pm(12, 12);
    pm.fill(c);
    return pm;
}

QPixmap flagIcon(const QColor& c)
{
    QPixmap pm(14, 14);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    MediaPool::drawFlag(p, QRectF(1, 1, 12, 12), c);
    return pm;
}

} // namespace

PoolFilterButton::PoolFilterButton(QWidget* parent) : QToolButton(parent)
{
    setAutoRaise(true);
    setPopupMode(QToolButton::InstantPopup);
    m_menu = new StayOpenMenu(this);
    const struct { PoolFilter::Kind kind; const char* name; } kinds[] = {
        {PoolFilter::Video, N_("Video")},
        {PoolFilter::Audio, N_("Audio")},
        {PoolFilter::Image, N_("Standbilder")},
        {PoolFilter::Timeline, N_("Timelines und Compound Clips")},
    };
    for (const auto& k : kinds) {
        QAction* a = m_menu->addAction(T(k.name));
        a->setCheckable(true);
        m_kindActions.insert(a, k.kind);
    }
    m_menu->addSeparator();
    m_flagMenu = new StayOpenMenu(T("Flags"), m_menu);
    for (const auto& f : kFlagColors) {
        QAction* a = m_flagMenu->addAction(QIcon(flagIcon(QColor::fromRgba(f.rgb))), T(f.name));
        a->setCheckable(true);
        m_flagActions.insert(a, QString::fromLatin1(f.id));
    }
    m_menu->addMenu(m_flagMenu);
    m_colorMenu = new StayOpenMenu(T("Clipfarbe"), m_menu);
    for (const auto& c : kTrackColors) {
        QAction* a = m_colorMenu->addAction(QIcon(swatch(QColor::fromRgba(c.rgb))), T(c.name));
        a->setCheckable(true);
        m_colorActions.insert(a, QString::fromLatin1(c.id));
    }
    m_colorMenu->addSeparator();
    QAction* none = m_colorMenu->addAction(T("Ohne Clipfarbe"));
    none->setCheckable(true);
    m_colorActions.insert(none, QString::fromLatin1(PoolFilter::NoColor));
    m_menu->addMenu(m_colorMenu);
    m_menu->addSeparator();
    m_reset = m_menu->addAction(T("Filter zurücksetzen"), this, [this] { setFilter({}); });
    for (const QList<QAction*>& actions : {m_kindActions.keys(), m_flagActions.keys(), m_colorActions.keys()})
        for (QAction* a : actions) connect(a, &QAction::triggered, this, [this, a](bool on) { toggle(a, on); });
    setMenu(m_menu);
    sync();
}

void PoolFilterButton::setFilter(const PoolFilter& filter)
{
    if (filter == m_filter) return;
    m_filter = filter;
    sync();
    emit changed(m_filter);
}

void PoolFilterButton::toggle(QAction* action, bool on)
{
    PoolFilter f = m_filter;
    if (m_kindActions.contains(action)) {
        const unsigned bit = m_kindActions.value(action);
        f.kinds = on ? f.kinds | bit : f.kinds & ~bit;
    } else if (m_flagActions.contains(action)) {
        if (on) f.flags.insert(m_flagActions.value(action));
        else f.flags.remove(m_flagActions.value(action));
    } else if (m_colorActions.contains(action)) {
        if (on) f.colors.insert(m_colorActions.value(action));
        else f.colors.remove(m_colorActions.value(action));
    } else {
        return;
    }
    setFilter(f);
}

void PoolFilterButton::sync()
{
    for (auto it = m_kindActions.cbegin(); it != m_kindActions.cend(); ++it) it.key()->setChecked(m_filter.kinds & it.value());
    for (auto it = m_flagActions.cbegin(); it != m_flagActions.cend(); ++it) it.key()->setChecked(m_filter.flags.contains(it.value()));
    for (auto it = m_colorActions.cbegin(); it != m_colorActions.cend(); ++it)
        it.key()->setChecked(m_filter.colors.contains(it.value()));
    // Submenus say how much is ticked inside them
    m_flagMenu->setTitle(m_filter.flags.isEmpty() ? T("Flags") : T("Flags (%1)").arg(m_filter.flags.size()));
    m_colorMenu->setTitle(m_filter.colors.isEmpty() ? T("Clipfarbe") : T("Clipfarbe (%1)").arg(m_filter.colors.size()));
    const bool active = m_filter.active();
    m_reset->setEnabled(active);
    setIcon(funnelIcon(active ? Theme::onPrimary : Theme::text));
    setStyleSheet(active ? QString("QToolButton { background: %1; } QToolButton:hover { background: %2; }")
                               .arg(Theme::primary.name(), Theme::primary.lighter(115).name())
                         : QString());
    setToolTip(active ? T("Filter: %1").arg(m_filter.summary()) : T("Clips filtern (Art, Flag, Clipfarbe)"));
}
