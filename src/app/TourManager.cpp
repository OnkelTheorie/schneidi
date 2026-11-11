// SPDX-License-Identifier: AGPL-3.0-only
#include "app/TourManager.h"

#include "app/InputBindings.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "ui/TourOverlay.h"

#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QRegularExpression>
#include <QScrollArea>
#include <QTimer>
#include <utility>

TourManager::TourManager(QMainWindow* window, Project* project, Host host)
    : QObject(window), m_window(window), m_project(project), m_host(std::move(host)), m_overlay(new TourOverlay(window))
{
    // Before a step's condition is first tested: the clips the user deleted are back (the step then waits as usual)
    m_runner.setConditions(
        [this](const TourStep& s) {
            prepare(s);
            begin(s);
        },
        [this](const QString& c) { return holds(c); });
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
    m_overlay->setTargets([this]() -> QVector<QRect> {
        const TourStep* s = m_runner.step();
        return s ? resolve(s->target) : QVector<QRect>{};
    });
    m_overlay->setKeepFree(
        [this]() -> QVector<QRect> {
            const TourStep* s = m_runner.step();
            return s && !s->keep.isEmpty() ? resolve(s->keep) : QVector<QRect>{};
        },
        [this]() -> QVector<QRect> { return m_host.avoid ? m_host.avoid() : QVector<QRect>{}; });
    m_overlay->setHome([this] { return m_host.home ? m_host.home() : QRect(); });
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
    QVector<TourOverlay::Choice> choices;
    if (firstStart)
        choices.push_back({"tour_never", T("Keine Rundgänge anbieten"),
                           [this] {
                               TourState::answer(m_asking, TourState::Answer::Never);
                               if (m_offerAction) m_offerAction->setChecked(false);
                               m_asking.clear();
                               m_overlay->end();
                           },
                           false, true});
    const auto later = [this] {
        TourState::answer(m_asking, TourState::Answer::Later);
        m_asking.clear();
        m_overlay->end();
    };
    choices.push_back({"tour_later", T("Später"), later, false, false, true});
    choices.push_back({"tour_show", T("Zeig's mir"),
                       [this] {
                           const QString id = m_asking;
                           TourState::answer(id, TourState::Answer::Show);
                           m_asking.clear();
                           start(id);
                       },
                       true});
    m_overlay->showQuestion(T(tour.title), text, T("Du findest alle Rundgänge jederzeit unter Hilfe › Rundgänge."), choices);
}

void TourManager::start(const QString& id)
{
    const Tour t = Tours::find(id);
    if (t.id.isEmpty()) return;
    if (const QString why = m_host.blocked ? m_host.blocked() : QString(); !why.isEmpty()) {
        m_asking.clear();
        m_overlay->end();
        m_window->statusBar()->showMessage(why, 6000);
        return;
    }
    if (m_runner.running()) m_runner.finish(); // the one running first ends (and gives the user's project back)
    m_asking.clear();
    TourState::markShown(id);
    if (!m_host.unsaved || !m_host.unsaved()) {
        launch(t);
        return;
    }
    // The tour runs in a practice project: the user's unsaved changes stay (in memory, back after the tour), saving
    // first is offered
    const auto cancel = [this] { m_overlay->end(); };
    m_overlay->showQuestion(
        T(t.title),
        T("Der Rundgang läuft in einem Übungsprojekt. Dein Projekt hat ungespeicherte Änderungen: Es kommt nach dem "
          "Rundgang genau so zurück, wie es jetzt ist."),
        {},
        {{"tour_cancel", T("Abbrechen"), cancel, false, true, true},
         {"tour_save", T("Erst speichern"),
          [this, t] {
              if (m_host.save && m_host.save()) launch(t);
          }},
         {"tour_keep", T("Rundgang starten"), [this, t] { launch(t); }, true}});
}

void TourManager::launch(const Tour& t)
{
    m_startPage = m_host.page ? m_host.page() : QString();
    if (m_host.enter) m_host.enter(t);
    // One bubble size for the whole tour (its buttons stay where they are from step to step)
    QStringList texts, offers;
    for (const TourStep& s : t.steps) {
        texts << stepText(s);
        if (const auto it = m_host.offers.constFind(s.offer); !s.offer.isEmpty() && it != m_host.offers.cend())
            offers << T(it->text);
    }
    m_overlay->measureSteps(texts, offers);
    m_hadClip = false;
    m_lostShown = false;
    m_notes.clear();
    emit runningChanged(true);
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
    if (!s->page.isEmpty() && m_host.page && m_host.showPage && m_host.page() != s->page) m_host.showPage(s->page);
    if (!s->tool.isEmpty() && m_host.setTool) m_host.setTool(s->tool);
    if (const auto it = m_host.actions.constFind(s->action); !s->action.isEmpty() && it != m_host.actions.cend()) {
        m_busy = true; // what the action changes is no step of the user's (it never ends a wait)
        (*it)();
        m_busy = false;
    }
    if (m_host.takeNote)
        if (const QString note = m_host.takeNote(); !note.isEmpty()) m_notes << note;
    // Targets inside a scroll area (the Color page's controls on a low screen) scrolled into view: a panel's own
    // scrolling, not the timeline's; once more after the new page's layout
    const auto reveal = [this, target = s->target] {
        for (const QString& part : target.split('|', Qt::SkipEmptyParts)) {
            QWidget* w = widgetFor(part);
            for (QWidget* p = w ? w->parentWidget() : nullptr; p; p = p->parentWidget())
                if (auto* area = qobject_cast<QScrollArea*>(p)) {
                    area->ensureWidgetVisible(w, 0, 0);
                    break;
                }
        }
    };
    reveal();
    QTimer::singleShot(0, this, reveal);
    begin(*s); // what the page switch or the action did (the playhead into a clip) is not the user's doing
    m_lostShown = m_hadClip && m_host.hasClip && !m_host.hasClip();
    showStep();
}

void TourManager::showStep()
{
    const TourStep* s = m_runner.step();
    if (!s) return;
    QString offer;
    if (const auto it = m_host.offers.constFind(s->offer); !s->offer.isEmpty() && it != m_host.offers.cend())
        if (!it->needed || it->needed()) offer = T(it->text);
    m_overlay->showStep(shownText(), m_runner.index(), m_runner.count(), m_runner.waiting(), offer);
}

QString TourManager::shownText() const
{
    const TourStep* s = m_runner.step();
    if (!s) return {};
    QString out = stepText(*s);
    QStringList notes = m_lostShown ? QStringList{N_("Hoppla, alle Clips sind weg. {key:undo} holt sie zurück – sonst hole "
                                                     "ich sie beim nächsten Schritt zurück.")}
                                    : m_notes;
    for (const QString& n : std::as_const(notes)) out += "<br><br><i>" + richText(n) + "</i>";
    return out;
}

void TourManager::prepare(const TourStep& step)
{
    m_notes.clear();
    if (!m_host.hasClip || !m_host.restoreClip) return;
    if (m_host.hasClip()) {
        m_hadClip = true;
        return;
    }
    // Deleted (all of them: select all, then Delete) at an earlier step, or never placed (the drag step skipped)
    if (!m_hadClip && step.needs != QLatin1String("clip")) return;
    m_busy = true;
    const Host::Restored r = m_host.restoreClip();
    m_busy = false;
    if (r == Host::Restored::Undone) m_notes << N_("Die Timeline war leer: Ich habe die Clips mit {key:undo} zurückgeholt.");
    if (r == Host::Restored::Placed) m_notes << N_("Ich habe dir einen Clip in die Timeline gelegt.");
    if (r != Host::Restored::No) m_hadClip = true;
}

void TourManager::updateLost()
{
    if (!m_runner.running() || !m_host.hasClip) return;
    const bool has = m_host.hasClip();
    if (has) m_hadClip = true;
    const bool lost = m_hadClip && !has;
    if (lost == m_lostShown) return;
    m_lostShown = lost;
    if (lost) m_notes.clear(); // "brought back" would be wrong now
    m_overlay->setStepText(shownText()); // the text only: the buttons and the keyboard focus stay
}

void TourManager::finished(bool)
{
    m_overlay->end();
    if (m_host.leave) {
        m_host.leave(); // the user's project, page and layout back
    } else if (!m_startPage.isEmpty() && m_host.page && m_host.showPage && m_host.page() != m_startPage) {
        m_host.showPage(m_startPage); // back where the tour began (it may have shown other pages)
    }
    emit runningChanged(false);
}

void TourManager::poke()
{
    if (m_busy) return; // the tour's own change (a repair, a step's action): looked at once it is done
    m_runner.check();
    updateLost();
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

QVector<int> TourManager::timelineShape() const
{
    QVector<int> out;
    const Timeline& tl = m_project->timeline();
    for (const QVector<Track>* tracks : {&tl.video, &tl.audio})
        for (const Track& t : *tracks)
            for (const Clip& c : t.clips) out << c.id << c.start << c.length();
    return out;
}

void TourManager::begin(const TourStep&)
{
    m_basePlayhead = m_host.playhead ? m_host.playhead() : 0;
    m_baseClips = clipCount();
    m_baseTimeline = timelineShape();
}

bool TourManager::holds(const QString& c) const
{
    if (c == "media_imported") return !m_project->media().isEmpty();
    if (c == "clip_on_timeline") return clipCount() > 0;
    if (c == "clip_added") return clipCount() > m_baseClips; // e.g. split: one clip more than when the step began
    if (c == "clip_trimmed") return clipCount() == m_baseClips && timelineShape() != m_baseTimeline; // same clips, moved edges
    if (c == "playhead_moved") return m_host.playhead && m_host.playhead() != m_basePlayhead;
    if (c.startsWith("page:")) return m_host.page && m_host.page() == c.mid(5);
    if (const auto it = m_host.conditions.constFind(c); it != m_host.conditions.cend()) return (*it)();
    qWarning("Tour condition unknown: %s", qPrintable(c));
    return true; // an unknown condition never blocks a tour
}

QString TourManager::stepText(const TourStep& step) const
{
    // The dictionaries keep the placeholder as it is: only the key itself depends on the user (preset, own keys)
    return richText(step.text);
}

QString TourManager::richText(const QString& german) const
{
    static const QRegularExpression placeholder(QStringLiteral("\\{key:([A-Za-z0-9_]+)\\}"));
    const QString text = T(german);
    QString out;
    qsizetype at = 0;
    for (auto it = placeholder.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        out += text.mid(at, m.capturedStart() - at).toHtmlEscaped();
        const QString id = m.captured(1);
        const QKeySequence key = InputBindings::instance().current(id);
        QString shown = key.toString(QKeySequence::NativeText);
        if (shown.isEmpty()) // no key: the action's name (the tool, the menu entry)
            if (QAction* a = findAction(id)) shown = a->text().remove('&');
        out += "<b>" + (shown.isEmpty() ? id : shown).toHtmlEscaped() + "</b>";
        at = m.capturedEnd();
    }
    return out + text.mid(at).toHtmlEscaped();
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

QWidget* TourManager::widgetFor(const QString& part) const
{
    if (part.startsWith("area:") || part.startsWith("menu:")) return nullptr;
    // A widget: its "tourTarget" property, else its objectName; the first one that shows
    const QList<QWidget*> all = m_window->findChildren<QWidget*>();
    const auto pick = [&](bool byProperty) -> QWidget* {
        for (QWidget* w : all) {
            if (w == m_overlay || m_overlay->isAncestorOf(w) || w == m_overlay->bubble()
                || m_overlay->bubble()->isAncestorOf(w) || !w->isVisible())
                continue;
            if (byProperty ? w->property("tourTarget").toString() == part : w->objectName() == part) return w;
        }
        return nullptr;
    };
    QWidget* w = pick(true);
    return w ? w : pick(false);
}

QVector<QRect> TourManager::resolve(const QString& target) const
{
    QVector<QRect> rects;
    for (const Target& t : targets(target)) rects << t.rect;
    return rects;
}

QVector<TourManager::Target> TourManager::targets(const QString& target) const
{
    QVector<Target> out;
    const auto add = [&](const QString& part, QWidget* w, const QRect& local) {
        if (!w || !local.isValid()) return;
        // Only what shows of it: every parent cuts it (a scroll area's viewport, a splitter)
        QRect r = QRect(w->mapTo(m_window, local.topLeft()), local.size());
        for (QWidget* p = w->parentWidget(); p && p != m_window; p = p->parentWidget())
            r = r.intersected(QRect(p->mapTo(m_window, QPoint(0, 0)), p->size()));
        r = r.intersected(m_window->rect());
        if (r.isValid()) out.push_back({part, r, w});
    };
    for (const QString& part : target.split('|', Qt::SkipEmptyParts)) {
        if (part.startsWith("area:")) {
            const auto it = m_host.areas.constFind(part.mid(5));
            if (it != m_host.areas.cend()) {
                const Host::Area a = (*it)();
                if (a.widget && a.widget->isVisible()) add(part, a.widget, a.rect);
            }
            continue;
        }
        if (part.startsWith("menu:")) {
            QAction* action = findAction(part.mid(5));
            QMenuBar* bar = m_window->menuBar();
            if (!action || !bar || !bar->isVisible()) continue;
            for (QAction* top : bar->actions())
                if (top->menu() && menuHolds(top->menu(), action)) {
                    add(part, bar, bar->actionGeometry(top));
                    break;
                }
            continue;
        }
        QWidget* w = widgetFor(part);
        if (w && w->width() > 0 && w->height() > 0) add(part, w, w->rect());
    }
    return out;
}
