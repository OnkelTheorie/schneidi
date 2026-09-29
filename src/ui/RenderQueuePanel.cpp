#include "ui/RenderQueuePanel.h"

#include "app/Theme.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Timecode.h"
#include "engine/RenderQueue.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

// Einzeilige Beschriftung, die sich der Kartenbreite anpasst (zu lang = „…“, voller Text im Tooltip)
class ElidedLabel : public QLabel {
public:
    void setFullText(const QString& text)
    {
        m_text = text;
        setToolTip(text);
        updateGeometry();
        update();
    }
    QSize minimumSizeHint() const override { return {0, fontMetrics().height()}; }
    QSize sizeHint() const override { return {fontMetrics().horizontalAdvance(m_text), fontMetrics().height()}; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setPen(palette().color(foregroundRole()));
        p.setFont(font());
        p.drawText(rect(), int(alignment() | Qt::AlignVCenter),
                   fontMetrics().elidedText(m_text, Qt::ElideMiddle, width()));
    }

private:
    QString m_text;
};

// Eine Karte je Auftrag
class JobCard : public QWidget {
public:
    JobCard()
    {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(8, 6, 8, 6);
        lay->setSpacing(2);
        auto* top = new QHBoxLayout;
        title = new ElidedLabel;
        title->setStyleSheet(QString("color: %1; font-weight: 600;").arg(Theme::text.name()));
        status = new QLabel;
        status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        top->addWidget(title, 1);
        top->addWidget(status);
        lay->addLayout(top);
        for (ElidedLabel** l : {&summary, &range, &path, &message}) {
            *l = new ElidedLabel;
            (*l)->setStyleSheet(QString("color: %1; font-size: 8pt;").arg(Theme::textDim.name()));
            lay->addWidget(*l);
        }
        message->setStyleSheet("color: #e8414a; font-size: 8pt;");
        progress = new QProgressBar;
        progress->setRange(0, 100);
        progress->setTextVisible(false);
        progress->setFixedHeight(4);
        progress->setStyleSheet(QString("QProgressBar { background: #141417; border: none; }"
                                        "QProgressBar::chunk { background: %1; }")
                                    .arg(Theme::accent.name()));
        lay->addWidget(progress);
        setAttribute(Qt::WA_TransparentForMouseEvents); // Auswahl/Kontextmenü macht die Liste
    }
    ElidedLabel *title, *summary, *range, *path, *message;
    QLabel* status;
    QProgressBar* progress;
};

QString statusColor(RenderStatus s)
{
    switch (s) {
    case RenderStatus::Done: return "#3cc05a";
    case RenderStatus::Failed: return "#e8414a";
    case RenderStatus::Rendering: return Theme::accent.name();
    default: return Theme::textDim.name();
    }
}

constexpr int kIdRole = Qt::UserRole + 1;

} // namespace

RenderQueuePanel::RenderQueuePanel(Project* project, RenderQueue* queue, QWidget* parent)
    : QWidget(parent), m_project(project), m_queue(queue)
{
    setObjectName("Panel");
    setMinimumWidth(280);
    m_title = new QLabel;
    m_title->setObjectName("PanelTitle");

    m_list = new QListWidget;
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setSpacing(2);
    m_list->setStyleSheet(QString("QListWidget { background: %1; }"
                                  "QListWidget::item { background: %2; border: 1px solid %3; }"
                                  "QListWidget::item:selected { background: #3a3a42; border: 1px solid %4; }")
                              .arg(Theme::panel.name(), Theme::panelHeader.name(), Theme::border.name(),
                                   Theme::accent.name()));
    m_empty = new QLabel(T("Keine Aufträge.\nLinks „Zur Render-Warteschlange hinzufügen“."));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));

    m_renderBtn = new QPushButton;
    m_renderBtn->setMinimumHeight(30);
    m_renderBtn->setStyleSheet("QPushButton { background: #e87a3a; color: black; font-weight: 600; border-radius: 3px; }"
                               "QPushButton:disabled { background: #5a4030; }");

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 12);
    lay->setSpacing(6);
    lay->addWidget(m_title);
    lay->addWidget(m_list, 1);
    lay->addWidget(m_empty, 1);
    auto* bottom = new QVBoxLayout;
    bottom->setContentsMargins(12, 0, 12, 0);
    bottom->addWidget(m_renderBtn);
    lay->addLayout(bottom);

    m_list->installEventFilter(this);
    m_list->viewport()->installEventFilter(this);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(m_renderBtn, &QPushButton::clicked, this, &RenderQueuePanel::renderClicked);
    connect(m_list, &QListWidget::itemSelectionChanged, this, &RenderQueuePanel::updateButton);
    connect(m_list, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* item) { emit loadJobRequested(item->data(kIdRole).toInt()); });
    connect(m_list, &QWidget::customContextMenuRequested, this, &RenderQueuePanel::contextMenu);
    connect(m_project, &Project::renderQueueChanged, this, &RenderQueuePanel::rebuild);
    connect(m_project, &Project::sequencesChanged, this, &RenderQueuePanel::rebuild); // Timeline-Namen
    connect(m_queue, &RenderQueue::runningChanged, this, &RenderQueuePanel::updateButton);
    connect(m_queue, &RenderQueue::progress, this, [this](int id, int percent) {
        for (int i = 0; i < m_list->count(); ++i)
            if (m_list->item(i)->data(kIdRole).toInt() == id)
                if (auto* card = static_cast<JobCard*>(m_list->itemWidget(m_list->item(i)))) {
                    card->progress->setValue(percent);
                    card->status->setText(T("Rendert %1 %").arg(percent));
                }
    });
    rebuild();
}

void RenderQueuePanel::rebuild()
{
    const QVector<int> selected = selectedJobs();
    m_list->clear();
    const auto& jobs = m_project->renderQueue();
    const int fps = m_project->fps();
    for (int n = 0; n < jobs.size(); ++n) {
        const RenderJob& j = jobs[n];
        auto* item = new QListWidgetItem(m_list);
        item->setData(kIdRole, j.id);
        auto* card = new JobCard;
        const QFileInfo fi(j.path);
        card->title->setFullText(T("Job %1").arg(j.id) + QStringLiteral(" · ") + fi.fileName());
        card->setToolTip(j.preset.isEmpty() ? T("Eigene Einstellungen") : j.preset);
        card->status->setText(j.status == RenderStatus::Rendering && m_queue->currentJob() == j.id
                                  ? T("Rendert %1 %").arg(m_queue->currentProgress())
                              : j.status == RenderStatus::Failed ? T("Fehler")
                                                                 : renderStatusText(j));
        card->message->setFullText(j.message);
        card->message->setVisible(j.status == RenderStatus::Failed && !j.message.isEmpty());
        card->status->setStyleSheet(QString("color: %1; font-size: 8pt;").arg(statusColor(j.status)));
        card->status->setToolTip(j.message);
        card->status->setMaximumWidth(140);
        // Welche Timeline (wie DaVinci in der Job-Karte); gelöschte Timeline bleibt als Hinweis stehen
        const QString timeline = !j.sequence ? QString()
                                 : m_project->sequence(j.sequence) ? m_project->sequenceName(j.sequence)
                                                                   : T("(Timeline gelöscht)");
        card->summary->setFullText(timeline.isEmpty() ? j.settings.summary(j.size)
                                                      : timeline + QStringLiteral(" · ") + j.settings.summary(j.size));
        card->range->setFullText(j.inOut ? T("In/Out: %1 – %2")
                                           .arg(Timecode::format(j.from, fps),
                                                j.to >= 0 ? Timecode::format(j.to, fps) : T("Ende"))
                                     : T("Ganze Timeline"));
        card->path->setFullText(QDir::toNativeSeparators(j.path));
        card->progress->setValue(j.status == RenderStatus::Done ? 100
                                 : j.status == RenderStatus::Rendering && m_queue->currentJob() == j.id
                                     ? m_queue->currentProgress()
                                     : 0);
        card->progress->setVisible(j.status == RenderStatus::Rendering || j.status == RenderStatus::Done);
        item->setSizeHint(QSize(m_list->viewport()->width() - 4, card->sizeHint().height()));
        m_list->setItemWidget(item, card);
        item->setSelected(selected.contains(j.id));
    }
    m_title->setText(jobs.isEmpty() ? T("Render-Warteschlange") : T("Render-Warteschlange (%1)").arg(jobs.size()));
    m_list->setVisible(!jobs.isEmpty());
    m_empty->setVisible(jobs.isEmpty());
    updateButton();
}

void RenderQueuePanel::updateButton()
{
    if (m_queue->isRunning()) {
        m_renderBtn->setText(T("Rendern stoppen"));
        m_renderBtn->setEnabled(true);
        return;
    }
    const bool sel = !selectedJobs().isEmpty();
    m_renderBtn->setText(sel ? T("Auswahl rendern") : T("Alle rendern"));
    const auto& jobs = m_project->renderQueue();
    m_renderBtn->setEnabled(sel || std::any_of(jobs.cbegin(), jobs.cend(), [](const RenderJob& j) {
                                return j.status != RenderStatus::Done;
                            }));
}

QVector<int> RenderQueuePanel::selectedJobs() const
{
    QVector<int> ids;
    for (QListWidgetItem* item : m_list->selectedItems()) ids << item->data(kIdRole).toInt();
    return ids;
}

void RenderQueuePanel::renderClicked()
{
    if (m_queue->isRunning()) {
        m_queue->cancel();
        return;
    }
    m_queue->start(selectedJobs());
}

void RenderQueuePanel::removeJobs(const QVector<int>& ids)
{
    QVector<RenderJob> q = m_project->renderQueue();
    q.erase(std::remove_if(q.begin(), q.end(), [&](const RenderJob& j) {
                return ids.contains(j.id) && !(m_queue->isRunning() && m_queue->currentJob() == j.id);
            }),
            q.end());
    m_project->setRenderQueue(q);
}

void RenderQueuePanel::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
        removeJobs(selectedJobs());
        return;
    }
    QWidget::keyPressEvent(e);
}

bool RenderQueuePanel::eventFilter(QObject* obj, QEvent* e)
{
    // Entf in der Liste löscht Aufträge, nicht Clips (Tastenkürzel der Timeline nicht auslösen)
    // Karten auf Listenbreite halten
    if (obj == m_list->viewport() && e->type() == QEvent::Resize)
        for (int i = 0; i < m_list->count(); ++i) {
            QListWidgetItem* item = m_list->item(i);
            item->setSizeHint(QSize(m_list->viewport()->width() - 4, item->sizeHint().height()));
        }
    if (obj == m_list && e->type() == QEvent::ShortcutOverride) {
        const int key = static_cast<QKeyEvent*>(e)->key();
        if (key == Qt::Key_Delete || key == Qt::Key_Backspace) e->accept();
    }
    return QWidget::eventFilter(obj, e);
}

void RenderQueuePanel::contextMenu(const QPoint& pos)
{
    const QVector<int> ids = selectedJobs();
    const bool running = m_queue->isRunning();
    QMenu menu(this);
    QAction* render = menu.addAction(T("Rendern"), this, [this, ids] { m_queue->start(ids); });
    render->setEnabled(!ids.isEmpty() && !running);
    QAction* load = menu.addAction(T("Einstellungen laden"), this, [this, ids] { emit loadJobRequested(ids.first()); });
    load->setEnabled(ids.size() == 1);
    QAction* reset = menu.addAction(T("Wieder auf „Wartet“ setzen"), this, [this, ids] {
        QVector<RenderJob> q = m_project->renderQueue();
        for (RenderJob& j : q)
            if (ids.contains(j.id) && j.status != RenderStatus::Rendering) {
                j.status = RenderStatus::Queued;
                j.message.clear();
            }
        m_project->setRenderQueue(q);
    });
    reset->setEnabled(!ids.isEmpty());
    menu.addSeparator();
    QAction* del = menu.addAction(T("Löschen"), this, [this, ids] { removeJobs(ids); });
    del->setShortcut(QKeySequence::Delete);
    del->setEnabled(!ids.isEmpty());
    menu.addAction(T("Fertige Aufträge entfernen"), this, [this] {
        QVector<int> done;
        for (const RenderJob& j : m_project->renderQueue())
            if (j.status == RenderStatus::Done) done << j.id;
        removeJobs(done);
    });
    menu.addAction(T("Alle Aufträge löschen"), this, [this] {
        QVector<int> all;
        for (const RenderJob& j : m_project->renderQueue()) all << j.id;
        removeJobs(all);
    });
    menu.exec(m_list->viewport()->mapToGlobal(pos));
}
