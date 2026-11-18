// SPDX-License-Identifier: AGPL-3.0-only
#include "core/Tours.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <algorithm>

// Tour file (assets/tours/<id>.json; tools/i18n_check.py collects "title" and every step's "text"):
// {
//   "id": "first_steps", "title": "Erste Schritte", "order": 1, "offerOn": "start", "group": "", "practice": "pool",
//   "steps": [
//     {"target": "mediaPool", "page": "edit", "text": "Hier liegen deine Clips."},
//     {"target": "mediaPool|timeline", "wait": "clip_on_timeline", "offer": "clips", "text": "Zieh einen Clip ..."},
//     {"target": "comp_canvas", "page": "comp", "action": "comp_nodes", "text": "..."},
//     {"target": "area:clip_v1", "needs": "clip", "action": "playhead_mid_clip", "text": "..."}
//   ]
// }

namespace Tours {

Tour parse(const QByteArray& json, QString* error)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    Tour t;
    if (!doc.isObject()) {
        if (error) *error = pe.errorString();
        return t;
    }
    const QJsonObject o = doc.object();
    for (const QJsonValue& v : o.value("steps").toArray()) {
        const QJsonObject s = v.toObject();
        TourStep step;
        step.target = s.value("target").toString();
        step.text = s.value("text").toString();
        step.page = s.value("page").toString();
        step.wait = s.value("wait").toString();
        step.offer = s.value("offer").toString();
        step.action = s.value("action").toString();
        step.keep = s.value("keep").toString();
        step.tool = s.value("tool").toString();
        step.needs = s.value("needs").toString();
        if (step.text.isEmpty()) {
            if (error) *error = QStringLiteral("step %1 without text").arg(t.steps.size() + 1);
            return {};
        }
        t.steps << step;
    }
    t.title = o.value("title").toString();
    t.order = o.value("order").toInt();
    t.offerOn = o.value("offerOn").toString();
    t.group = o.value("group").toString();
    t.practice = o.value("practice").toString(t.practice);
    if (t.steps.isEmpty() || t.title.isEmpty()) {
        if (error) *error = QStringLiteral("no title or no steps");
        return {};
    }
    t.id = o.value("id").toString();
    if (t.id.isEmpty() && error) *error = QStringLiteral("no id");
    return t;
}

QVector<Tour> builtIn()
{
    QVector<Tour> tours;
    for (const QString& name : QDir(":/tours").entryList({"*.json"}, QDir::Files, QDir::Name)) {
        QFile f(":/tours/" + name);
        if (!f.open(QIODevice::ReadOnly)) continue;
        QString err;
        Tour t = parse(f.readAll(), &err);
        if (t.id.isEmpty()) qWarning("Tour %s unreadable: %s", qPrintable(name), qPrintable(err));
        else tours << t;
    }
    std::stable_sort(tours.begin(), tours.end(), [](const Tour& a, const Tour& b) { return a.order < b.order; });
    return tours;
}

Tour find(const QString& id)
{
    for (const Tour& t : builtIn())
        if (t.id == id) return t;
    return {};
}

} // namespace Tours

namespace TourState {

namespace {
bool g_suppressed = false;
QString key(const QString& id) { return "tours/state/" + id; }
} // namespace

void setSuppressed(bool on) { g_suppressed = on; }
bool suppressed() { return g_suppressed; }

bool offerTours() { return QSettings().value("tours/offer", true).toBool(); }
void setOfferTours(bool on) { QSettings().setValue("tours/offer", on); }

QString state(const QString& id) { return QSettings().value(key(id)).toString(); }

bool shouldAsk(const QString& id) { return !g_suppressed && offerTours() && state(id).isEmpty(); }

void answer(const QString& id, Answer a)
{
    if (g_suppressed) return; // a test run never writes the user's answers
    QSettings s;
    s.setValue(key(id), a == Answer::Show ? "shown" : "later");
    if (a == Answer::Never) s.setValue("tours/offer", false);
}

void markShown(const QString& id)
{
    if (!g_suppressed) QSettings().setValue(key(id), "shown");
}

void resetAll()
{
    QSettings s;
    s.remove("tours/state");
    s.setValue("tours/offer", true);
}

} // namespace TourState

TourRunner::TourRunner(QObject* parent) : QObject(parent) {}

void TourRunner::setConditions(std::function<void(const TourStep&)> begin, std::function<bool(const QString&)> holds)
{
    m_begin = std::move(begin);
    m_holds = std::move(holds);
}

const TourStep* TourRunner::step() const
{
    return m_index >= 0 && m_index < m_tour.steps.size() ? &m_tour.steps[m_index] : nullptr;
}

void TourRunner::start(const Tour& tour)
{
    if (running()) finish();
    m_tour = tour;
    if (m_tour.steps.isEmpty()) return;
    enter(0);
}

void TourRunner::enter(int index)
{
    m_index = index;
    const TourStep& s = m_tour.steps[index];
    if (m_begin) m_begin(s);
    // A condition that already holds (the clip is on the timeline already) does not wait
    const bool wait = !s.wait.isEmpty() && !(m_holds && m_holds(s.wait));
    const bool changed = wait != m_waiting;
    m_waiting = wait;
    emit stepChanged(index);
    if (changed) emit waitingChanged(wait);
}

void TourRunner::next()
{
    if (!running()) return;
    if (m_index + 1 >= m_tour.steps.size()) {
        m_index = -1;
        m_waiting = false;
        emit finished(true);
        return;
    }
    enter(m_index + 1);
}

void TourRunner::back()
{
    if (running() && m_index > 0) enter(m_index - 1);
}

void TourRunner::finish()
{
    if (!running()) return;
    m_index = -1;
    m_waiting = false;
    emit finished(false);
}

void TourRunner::check()
{
    if (!running() || !m_waiting) return;
    if (m_holds && m_holds(m_tour.steps[m_index].wait)) next(); // done what the step asked: on to the next one
}
