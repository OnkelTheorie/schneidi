#include "app/AiLog.h"

#include "app/Theme.h"
#include "core/I18n.h"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {

// Work outside the app per command: what the indicator says while it runs (after "AI"), and the result ("%1" = the
// count, the loudness or the file written)
struct Job {
    const char* tool;
    const char* doing;
    const char* done;
};

const Job* jobOf(const QString& tool)
{
    static const Job jobs[] = {
        {"probe", N_("prüft eine Datei"), N_("Datei geprüft")},
        {"silence", N_("sucht Stille"), N_("Pausen gefunden: %1")},
        {"scenes", N_("sucht Szenenwechsel"), N_("Szenenwechsel gefunden: %1")},
        {"peaks", N_("sucht laute Stellen"), N_("Laute Stellen gefunden: %1")},
        {"highlights", N_("sucht Highlights"), N_("Highlights gefunden: %1")},
        {"frames", N_("sieht sich Bilder an"), N_("Bilder angesehen: %1")},
        {"transcribe", N_("transkribiert"), N_("Untertitel erkannt: %1")},
        {"loudness", N_("misst die Lautheit"), N_("Lautheit: %1 LUFS")},
        {"scopes", N_("misst das Bild"), N_("Bild gemessen")},
        {"sync", N_("gleicht Aufnahmen am Ton ab"), N_("Aufnahmen abgeglichen: %1")},
        {"render", N_("rendert"), N_("Video geschrieben: %1")},
        {"export_timeline", N_("exportiert die Timeline"), N_("Timeline exportiert: %1")},
        {"extensions", N_("lädt eine Erweiterung"), N_("Erweiterung fertig")},
    };
    for (const Job& j : jobs)
        if (tool == QLatin1String(j.tool)) return &j;
    return nullptr;
}

} // namespace

AiLog::AiLog(QObject* parent) : QObject(parent) {}

void AiLog::setLimit(int n)
{
    m_limit = std::max(0, n);
    trim();
    emit changed();
}

const AiLog::Entry* AiLog::entry(int id) const
{
    for (const Entry& e : m_entries)
        if (e.id == id) return &e;
    return nullptr;
}

const AiLog::Entry* AiLog::runningJob() const
{
    for (const Entry& e : m_entries)
        if (e.state == Entry::State::Running) return &e;
    return nullptr;
}

AiLog::Entry* AiLog::runningEntry(int job)
{
    for (Entry& e : m_entries)
        if (e.state == Entry::State::Running && e.job == job) return &e;
    return nullptr;
}

int AiLog::add(Entry e)
{
    e.id = m_next++;
    if (!e.time.isValid()) e.time = QTime::currentTime();
    m_entries.prepend(e);
    trim();
    emit changed();
    return e.id;
}

void AiLog::activity(const LiveActivity& a)
{
    using S = LiveActivity::State;
    Entry* e = runningEntry(a.id);
    if (a.state == S::Started) {
        if (e) { // the same id again: the earlier one never ended (another session)
            e->state = Entry::State::Failed;
            e->detail = T("abgebrochen");
        }
        Entry n;
        n.state = Entry::State::Running;
        n.job = a.id;
        n.tool = a.tool;
        n.step = a.target.isEmpty() ? a.tool : QString("%1 · %2").arg(a.tool, targetName(a.target));
        add(n);
        return;
    }
    if (!e) return; // its start was not seen (log cleared meanwhile)
    if (a.state == S::Progress) {
        e->percent = a.percent;
    } else if (a.state == S::Finished) {
        e->state = Entry::State::Done;
        e->detail = jobResult(a.tool, a.summary);
    } else {
        e->state = Entry::State::Failed;
        e->detail = a.error == "interrupted" ? T("abgebrochen") : T("fehlgeschlagen: %1").arg(a.error);
    }
    if (a.state != S::Progress) trim();
    emit changed();
}

void AiLog::interruptRunning()
{
    bool any = false;
    for (Entry& e : m_entries)
        if (e.state == Entry::State::Running) {
            e.state = Entry::State::Failed;
            e.detail = T("abgebrochen");
            any = true;
        }
    if (!any) return;
    trim();
    emit changed();
}

void AiLog::clear()
{
    // Running work stays (it reports its end)
    m_entries.removeIf([](const Entry& e) { return e.state != Entry::State::Running; });
    emit changed();
}

void AiLog::trim()
{
    int kept = 0;
    m_entries.removeIf([&](const Entry& e) { return e.state != Entry::State::Running && ++kept > m_limit; });
}

QString AiLog::jobText(const QString& tool)
{
    if (const Job* j = jobOf(tool)) return T(j->doing);
    return T("arbeitet (%1)").arg(tool);
}

QString AiLog::jobResult(const QString& tool, const QJsonObject& summary)
{
    const Job* j = jobOf(tool);
    if (!j) return T("fertig");
    const QString done = T(j->done);
    if (!done.contains("%1")) return done;
    if (summary.contains("count")) return done.arg(summary.value("count").toInt());
    if (summary.contains("lufs"))
        return done.arg(QString::number(summary.value("lufs").toDouble(), 'f', 1).replace('.', I18n::decimalPoint()));
    if (summary.contains("file")) return done.arg(QFileInfo(summary.value("file").toString()).fileName());
    return T("fertig");
}

QString AiLog::targetName(const QString& target)
{
    if (target.trimmed() == QLatin1String("@app")) return T("offenes Projekt");
    return QFileInfo(target).fileName();
}

// ---------- View ----------

AiLogView::AiLogView(AiLog* log, QWidget* parent) : QFrame(parent), m_log(log)
{
    setObjectName("AiLogView");
    setFrameShape(QFrame::StyledPanel);
    setStyleSheet(QString("#AiLogView { background: %1; border: 1px solid %2; }").arg(Theme::panel.name(), Theme::border.name()));
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 6);
    lay->setSpacing(4);

    auto* top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 4, 0);
    auto* title = new QLabel(T("KI-Protokoll"));
    title->setObjectName("PanelTitle");
    top->addWidget(title, 1);
    auto* clear = new QToolButton;
    clear->setObjectName("ai_log_clear");
    clear->setText(T("Leeren"));
    connect(clear, &QToolButton::clicked, log, &AiLog::clear);
    top->addWidget(clear);
    lay->addLayout(top);

    m_list = new QTreeWidget;
    m_list->setObjectName("ai_log_list");
    m_list->setColumnCount(3);
    m_list->setHeaderLabels({T("Zeit"), T("Schritt"), T("Ergebnis")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setAllColumnsShowFocus(true);
    m_list->header()->setStretchLastSection(true);
    m_list->setColumnWidth(0, 64);
    m_list->setColumnWidth(1, 210);
    m_list->setStyleSheet(QString("QTreeWidget { background: %1; border: none; }").arg(Theme::panel.name()));
    connect(m_list, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        emit activated(item->data(0, Qt::UserRole).toInt());
    });
    lay->addWidget(m_list, 1);
    m_empty = new QLabel;
    m_empty->setWordWrap(true);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setStyleSheet(QString("color: %1; padding: 16px;").arg(Theme::textDim.name()));
    lay->addWidget(m_empty, 1);
    auto* hint = new QLabel(T("Klick auf einen Eintrag: seine Clips auswählen und hinspringen."));
    hint->setStyleSheet(QString("color: %1; padding: 0 8px;").arg(Theme::textDim.name()));
    lay->addWidget(hint);

    resize(620, 340);
    connect(log, &AiLog::changed, this, &AiLogView::rebuild);
    rebuild();
}

void AiLogView::rebuild()
{
    m_list->clear();
    for (const AiLog::Entry& e : m_log->entries()) {
        QString detail = e.detail;
        if (e.state == AiLog::Entry::State::Running)
            detail = e.percent >= 0 ? T("läuft … %1 %").arg(e.percent) : T("läuft …");
        auto* item = new QTreeWidgetItem(m_list, {e.time.toString("HH:mm:ss"), e.step, detail});
        item->setData(0, Qt::UserRole, e.id);
        for (int c = 0; c < 3; ++c) item->setToolTip(c, c == 2 ? detail : e.step);
        if (e.state == AiLog::Entry::State::Failed) item->setForeground(2, Theme::warning);
        if (e.state == AiLog::Entry::State::Running) item->setForeground(2, Theme::primary);
        if (e.clips.isEmpty() && e.frame < 0) item->setForeground(1, Theme::textDim); // nothing to jump to
    }
    const bool empty = m_log->entries().isEmpty();
    m_list->setVisible(!empty);
    m_empty->setVisible(empty);
    m_empty->setText(m_log->limit() == 0 ? T("Das Protokoll ist aus (Einstellungen › KI).")
                                          : T("Noch nichts. Hier steht, was eine verbundene KI tut: Schnitte, "
                                              "Analysen, Render."));
}
