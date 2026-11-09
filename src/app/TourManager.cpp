// SPDX-License-Identifier: AGPL-3.0-only
#include "app/TourManager.h"

#include "app/InputBindings.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "ui/TourOverlay.h"

#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <utility>

TourManager::TourManager(QMainWindow* window, Project* project, Host host)
    : QObject(window), m_window(window), m_project(project), m_host(std::move(host)), m_overlay(new TourOverlay(window))
{
    m_runner.setConditions([this](const TourStep& s) { begin(s); }, [this](const QString& c) { return holds(c); });
    connect(&m_runner, &TourRunner::stepChanged, this, &TourManager::enterStep);
    connect(&m_runner, &TourRunner::waitingChanged, this, &TourManager::showStep);
    connect(&m_runner, &TourRunner::finished, this, &TourManager::finished);
    connect(m_overlay, &TourOverlay::nextClicked, &m_runner, &TourRunner::next);
    connect(m_overlay, &TourOverlay::backClicked, &m_runner, &TourRunner::back);
    connect(m_overlay, &TourOverlay::endClicked, &m_runner, &TourRunner::finish);
    connect(m_overlay, &TourOverlay::offerClicked, this, [this] {
        const TourStep* step = m_runner.step();
        if (!step) return;
        const auto it = m_host.offers.constFind(step->offer);
        if (it != m_host.offers.cend() && it->run) it->run();
        showStep(); // the button goes away once it is not needed any more
        poke();
    });
    connect(m_overlay, &TourOverlay::showMe, this, [this] {
        const QString id = m_asking;
        TourState::answer(id, TourState::Answer::Show);
        m_asking.clear();
        start(id);
    });
    connect(m_overlay, &TourOverlay::later, this, [this] {
        TourState::answer(m_asking, TourState::Answer::Later);
        m_asking.clear();
        m_overlay->end();
    });
    connect(m_overlay, &TourOverlay::never, this, [this] {
        TourState::answer(m_asking, TourState::Answer::Never);
        if (m_offerAction) m_offerAction->setChecked(false);
        m_asking.clear();
        m_overlay->end();
    });
    m_overlay->setTargets([this]() -> QVector<QRect> {
        const TourStep* s = m_runner.step();
        return s ? resolve(s->target) : QVector<QRect>{};
    });
    connect(m_project, &Project::timelineChanged, this, &TourManager::poke);
    connect(m_project, &Project::mediaChanged, this, &TourManager::poke);
}

void TourManager::addMenu(QMenu* help)
{
    QMenu* menu = help->addMenu(T("Rundgänge"));
    menu->setObjectName("tours_menu");
    QMenu* guides = nullptr; // Help › Tours › Guides: short tours that build one result
    for (const Tour& t : Tours::builtIn()) {
        QMenu* in = menu;
        if (t.group == QLatin1String("guides")) {
            if (!guides) {
                guides = menu->addMenu(T("Anleitungen"));
                guides->setObjectName("tours_guides");
            }
            in = guides;
        }
        QAction* a = in->addAction(T(t.title), this, [this, id = t.id] { start(id); });
        a->setObjectName("tour_" + t.id);
    }
    menu->addSeparator();
    m_offerAction = menu->addAction(T("Rundgänge anbieten"));
    m_offerAction->setObjectName("tours_offer");
    m_offerAction->setCheckable(true);
    m_offerAction->setChecked(TourState::offerTours());
    m_offerAction->setToolTip(T("Beim ersten Öffnen einer Seite einmal fragen, ob ein Rundgang sie zeigen soll"));
    connect(m_offerAction, &QAction::toggled, this, [](bool on) {
        if (!TourState::suppressed()) TourState::setOfferTours(on);
    });
    QAction* reset = menu->addAction(T("Alle Rundgänge wieder anbieten"), this, [this] {
        if (!TourState::suppressed()) TourState::resetAll();
        if (m_offerAction) m_offerAction->setChecked(true);
    });
    reset->setObjectName("tours_reset");
}

void TourManager::offerAtStart()
{
    if (TourState::suppressed() || m_runner.running() || asking()) return;
    for (const Tour& t : Tours::builtIn())
        if (t.offerOn == "start" && TourState::shouldAsk(t.id)) {
            ask(t, true);
            return;
        }
}

void TourManager::pageOpened(const QString& page)
{
    if (TourState::suppressed() || m_runner.running() || asking()) return;
    for (const Tour& t : Tours::builtIn())
        if (t.offerOn == "page:" + page && TourState::shouldAsk(t.id)) {
            ask(t, false);
            return;
        }
}

void TourManager::ask(const Tour& tour, bool firstStart)
{
    m_asking = tour.id;
    const QString text = firstStart ? T("Neu hier? Soll ich dir schneidi kurz zeigen? (ca. 1 Minute)")
                                    : T("Soll ich dir das kurz zeigen? (ca. 1 Minute)");
    m_overlay->showQuestion(T(tour.title), text, firstStart);
}

void TourManager::start(const QString& id)
{
    const Tour t = Tours::find(id);
    if (t.id.isEmpty()) return;
    m_asking.clear();
    TourState::markShown(id);
    m_startPage = m_host.page ? m_host.page() : QString();
    m_runner.start(t);
}

void TourManager::stop()
{
    m_asking.clear();
    if (m_runner.running()) m_runner.finish();
    m_overlay->end();
}

void TourManager::enterStep()
{
    const TourStep* s = m_runner.step();
    if (!s) return;
    unreveal();
    if (!s->page.isEmpty() && m_host.page && m_host.showPage && m_host.page() != s->page) m_host.showPage(s->page);
    if (const auto it = m_host.actions.constFind(s->action); !s->action.isEmpty() && it != m_host.actions.cend()) (*it)();
    // Panels the user switched off: shown for this step only
    if (m_host.reveal)
        for (const QString& part : s->target.split('|', Qt::SkipEmptyParts))
            if (!part.contains(':'))
                if (auto undo = m_host.reveal(part)) m_revealed << undo;
    showStep();
}

void TourManager::unreveal()
{
    const auto undo = std::exchange(m_revealed, {});
    for (auto it = undo.rbegin(); it != undo.rend(); ++it) (*it)();
}

void TourManager::showStep()
{
    const TourStep* s = m_runner.step();
    if (!s) return;
    QString offer;
    if (const auto it = m_host.offers.constFind(s->offer); !s->offer.isEmpty() && it != m_host.offers.cend())
        if (!it->needed || it->needed()) offer = T(it->text);
    m_overlay->showStep(T(s->text), m_runner.index(), m_runner.count(), m_runner.waiting(), offer);
}

void TourManager::finished(bool)
{
    unreveal();
    m_overlay->end();
    // Back where the tour began (it may have shown other pages)
    if (!m_startPage.isEmpty() && m_host.page && m_host.showPage && m_host.page() != m_startPage)
        m_host.showPage(m_startPage);
}

void TourManager::poke()
{
    m_runner.check();
    // A page switch from the tour itself may hide a target or show it: the overlay follows on its own timer
}

int TourManager::clipCount() const
{
    int n = 0;
    const Timeline& tl = m_project->timeline();
    for (const Track& t : tl.video) n += t.clips.size();
    for (const Track& t : tl.audio) n += t.clips.size();
    return n;
}

void TourManager::begin(const TourStep&)
{
    m_basePlayhead = m_host.playhead ? m_host.playhead() : 0;
    m_baseClips = clipCount();
}

bool TourManager::holds(const QString& c) const
{
    if (c == "media_imported") return !m_project->media().isEmpty();
    if (c == "clip_on_timeline") return clipCount() > 0;
    if (c == "clip_added") return clipCount() > m_baseClips; // e.g. split: one clip more than when the step began
    if (c == "playhead_moved") return m_host.playhead && m_host.playhead() != m_basePlayhead;
    if (c.startsWith("page:")) return m_host.page && m_host.page() == c.mid(5);
    if (const auto it = m_host.conditions.constFind(c); it != m_host.conditions.cend()) return (*it)();
    qWarning("Tour condition unknown: %s", qPrintable(c));
    return true; // an unknown condition never blocks a tour
}

QAction* TourManager::findAction(const QString& id) const
{
    for (const auto& e : InputBindings::instance().entries())
        if (e.id == id) return e.action;
    return nullptr;
}

namespace {
bool menuHolds(QMenu* menu, QAction* action)
{
    for (QAction* a : menu->actions()) {
        if (a == action) return true;
        if (a->menu() && menuHolds(a->menu(), action)) return true;
    }
    return false;
}
} // namespace

QVector<QRect> TourManager::resolve(const QString& target) const
{
    QVector<QRect> rects;
    for (const QString& part : target.split('|', Qt::SkipEmptyParts)) {
        if (part.startsWith("area:")) {
            const auto it = m_host.areas.constFind(part.mid(5));
            if (it != m_host.areas.cend()) {
                const QRect r = (*it)();
                if (r.isValid()) rects << r;
            }
            continue;
        }
        if (part.startsWith("menu:")) {
            QAction* action = findAction(part.mid(5));
            QMenuBar* bar = m_window->menuBar();
            if (!action || !bar || !bar->isVisible()) continue;
            for (QAction* top : bar->actions())
                if (top->menu() && menuHolds(top->menu(), action)) {
                    const QRect r = bar->actionGeometry(top);
                    if (r.isValid()) rects << QRect(bar->mapTo(m_window, r.topLeft()), r.size());
                    break;
                }
            continue;
        }
        // A widget: its "tourTarget" property, else its objectName; the first one that shows
        const QList<QWidget*> all = m_window->findChildren<QWidget*>();
        const auto pick = [&](bool byProperty) -> QWidget* {
            for (QWidget* w : all) {
                if (w == m_overlay || m_overlay->isAncestorOf(w) || !w->isVisible()) continue;
                if (byProperty ? w->property("tourTarget").toString() == part : w->objectName() == part) return w;
            }
            return nullptr;
        };
        QWidget* w = pick(true);
        if (!w) w = pick(false);
        if (!w || w->width() <= 0 || w->height() <= 0) continue;
        QRect r(w->mapTo(m_window, QPoint(0, 0)), w->size());
        r = r.intersected(m_window->rect());
        if (r.isValid()) rects << r;
    }
    return rects;
}
