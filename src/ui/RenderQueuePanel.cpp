#include "ui/RenderQueuePanel.h"

#include "app/InputBindings.h"
#include "app/Theme.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Timecode.h"
#include "engine/RenderQueue.h"

#include <QAction>
#include <QDir>
#include <QDrag>
#include <QDropEvent>
#include <QElapsedTimer>
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

#include <algorithm>
#include <functional>

namespace {

// Single-line label that adapts to the card width (too long = "…", full text in the tooltip)
class ElidedLabel : public QLabel {
public:
    explicit ElidedLabel(Qt::TextElideMode mode = Qt::ElideRight) : m_mode(mode) {}
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
                   fontMetrics().elidedText(m_text, m_mode, width()));
    }

private:
    QString m_text;
    Qt::TextElideMode m_mode;
};

// One card per job
class JobCard : public QWidget {
public:
    JobCard()
    {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(8, 6, 8, 6);
        lay->setSpacing(2);
        auto* top = new QHBoxLayout;
        title = new ElidedLabel(Qt::ElideMiddle); // file name: keep the extension
        title->setStyleSheet(QString("color: %1; font-weight: 600;").arg(Theme::text.name()));
        status = new QLabel;
        status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        top->addWidget(title, 1);
        top->addWidget(status);
        lay->addLayout(top);
        // Path in the middle (keeps folder start and file name); the rest at the end, so no word is cut in half
        for (ElidedLabel** l : {&summary, &range, &path, &message}) {
            *l = new ElidedLabel(l == &path ? Qt::ElideMiddle : Qt::ElideRight);
            (*l)->setStyleSheet(QString("color: %1; font-size: 8pt;").arg(Theme::textDim.name()));
            lay->addWidget(*l);
        }
        message->setStyleSheet(QString("color: %1; font-size: 8pt;").arg(Theme::warning.name()));
        progress = new QProgressBar;
        progress->setRange(0, 100);
        progress->setTextVisible(false);
        progress->setFixedHeight(4);
        progress->setStyleSheet(QString("QProgressBar { background: %2; border: none; }"
                                        "QProgressBar::chunk { background: %1; }")
                                    .arg(Theme::primary.name(), Theme::well.name()));
        lay->addWidget(progress);
        setAttribute(Qt::WA_TransparentForMouseEvents); // selection/context menu are handled by the list
    }
    ElidedLabel *title, *summary, *range, *path, *message;
    QLabel* status;
    QProgressBar* progress;
};

QString statusColor(RenderStatus s)
{
    switch (s) {
    case RenderStatus::Done: return Theme::meterLow.name();
    case RenderStatus::Failed: return Theme::warning.name();
    case RenderStatus::Rendering: return Theme::primary.name();
    default: return Theme::textDim.name();
    }
}

constexpr int kIdRole = Qt::UserRole + 1;

// The job list: dragging jobs reorders the queue. The list does not move its items itself, the panel changes the
// project's queue and rebuilds the cards in the new order.
class JobList : public QListWidget {
public:
    std::function<void(const QVector<int>& ids, int beforeId)> moveRequested;

    // Job in front of which a drop at pos lands (upper/lower half of a card), 0 = at the end
    int dropTarget(const QPoint& pos) const
    {
        QListWidgetItem* item = itemAt(pos);
        if (!item) { // between two cards (spacing) or below the last one: the next card below pos
            for (int i = 0; i < count(); ++i)
                if (visualItemRect(this->item(i)).top() > pos.y()) return this->item(i)->data(kIdRole).toInt();
            return 0;
        }
        const int row = this->row(item) + (pos.y() > visualItemRect(item).center().y() ? 1 : 0);
        return row < count() ? this->item(row)->data(kIdRole).toInt() : 0;
    }

protected:
    void startDrag(Qt::DropActions) override
    {
        QModelIndexList indexes;
        for (const QModelIndex& index : selectedIndexes())
            if (index.flags() & Qt::ItemIsDragEnabled) indexes << index;
        if (indexes.isEmpty()) return;
        std::sort(indexes.begin(), indexes.end(), [](const QModelIndex& a, const QModelIndex& b) { return a.row() < b.row(); });
        auto* drag = new QDrag(this);
        drag->setMimeData(model()->mimeData(indexes));
        // The cards are widgets the item delegate does not paint: the first one as the picture
        if (QWidget* card = itemWidget(item(indexes.first().row()))) {
            const QPixmap picture = card->grab();
            drag->setPixmap(picture);
            drag->setHotSpot(card->mapFromGlobal(QCursor::pos()));
        }
        // Copy, not move: the view would remove the rows itself afterwards (the panel rebuilds them)
        drag->exec(Qt::MoveAction | Qt::CopyAction, Qt::MoveAction);
    }
    void dropEvent(QDropEvent* e) override
    {
        if (e->source() != this) {
            e->ignore();
            return;
        }
        QVector<int> ids;
        for (QListWidgetItem* item : selectedItems())
            if (item->flags() & Qt::ItemIsDragEnabled) ids << item->data(kIdRole).toInt();
        const int before = dropTarget(e->position().toPoint());
        e->setDropAction(Qt::CopyAction);
        e->accept();
        stopAutoScroll();
        setState(NoState);
        // After the drag has ended: the move rebuilds the list
        QMetaObject::invokeMethod(this, [this, ids, before] { if (moveRequested) moveRequested(ids, before); },
                                  Qt::QueuedConnection);
    }
};

} // namespace

RenderQueuePanel::RenderQueuePanel(Project* project, RenderQueue* queue, QWidget* parent)
    : QWidget(parent), m_project(project), m_queue(queue)
{
    setObjectName("Panel");
    setMinimumWidth(280);
    m_title = new QLabel;
    m_title->setObjectName("PanelTitle");

    auto* list = new JobList;
    list->moveRequested = [this](const QVector<int>& ids, int beforeId) { moveJobs(ids, beforeId); };
    m_list = list;
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setDragDropMode(QAbstractItemView::InternalMove);
    m_list->setDefaultDropAction(Qt::MoveAction);
    m_list->setDropIndicatorShown(true);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setSpacing(2);
    m_list->setStyleSheet(QString("QListWidget { background: %1; }"
                                  "QListWidget::item { background: %2; border: 1px solid %3; }"
                                  "QListWidget::item:selected { background: %5; border: 1px solid %4; }")
                              .arg(Theme::panel.name(), Theme::panelHeader.name(), Theme::border.name(),
                                   Theme::primary.name(), Theme::control.name()));
    m_empty = new QLabel(T("Keine Aufträge.\nLinks „Zur Render-Warteschlange hinzufügen“."));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));

    m_renderBtn = new QPushButton;
    m_renderBtn->setMinimumHeight(30);
    m_renderBtn->setStyleSheet(Theme::primaryButtonStyle());

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
    m_renderBtn->installEventFilter(this);
    m_list->viewport()->installEventFilter(this);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(m_renderBtn, &QPushButton::clicked, this, &RenderQueuePanel::renderClicked);
    connect(m_list, &QListWidget::itemSelectionChanged, this, &RenderQueuePanel::updateButton);
    connect(m_list, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* item) { jobDoubleClicked(item->data(kIdRole).toInt()); });
    m_deleteAction = new QAction(T("Aufträge löschen"), this);
    m_deleteAction->setShortcuts({QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)}); // InputBindings may change them
    m_deleteAction->setShortcutContext(Qt::WidgetShortcut);
    connect(m_deleteAction, &QAction::triggered, this, [this] { removeJobs(selectedJobs()); });
    connect(m_list, &QWidget::customContextMenuRequested, this, &RenderQueuePanel::contextMenu);
    connect(m_project, &Project::renderQueueChanged, this, &RenderQueuePanel::rebuild);
    connect(m_project, &Project::sequencesChanged, this, &RenderQueuePanel::rebuild); // timeline name
    connect(m_queue, &RenderQueue::runningChanged, this, &RenderQueuePanel::updateButton);
    connect(m_queue, &RenderQueue::progress, this, [this](int id, int percent) {
        for (int i = 0; i < m_list->count(); ++i)
            if (m_list->item(i)->data(kIdRole).toInt() == id)
                if (auto* card = static_cast<JobCard*>(m_list->itemWidget(m_list->item(i)))) {
                    card->progress->setValue(percent);
                    card->status->setText(T("Rendert %1 %").arg(percent));
                }
        updateButton(); // after "preparing"
    });
    // Preparing runs in the background (RenderQueue::isPreparing): the button stops it like a running render
    connect(m_queue, &RenderQueue::preparing, this, [this](int id) {
        for (int i = 0; i < m_list->count(); ++i)
            if (m_list->item(i)->data(kIdRole).toInt() == id)
                if (auto* card = static_cast<JobCard*>(m_list->itemWidget(m_list->item(i))))
                    card->status->setText(T("Wird vorbereitet …"));
        updateButton();
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
        // The job being rendered stays where it is; the others can be dragged
        item->setFlags((item->flags() | Qt::ItemIsDragEnabled) & ~Qt::ItemIsDropEnabled);
        if (m_queue->isRunning() && m_queue->currentJob() == j.id) item->setFlags(item->flags() & ~Qt::ItemIsDragEnabled);
        auto* card = new JobCard;
        const QFileInfo fi(j.path);
        card->title->setFullText(T("Job %1").arg(j.id) + QStringLiteral(" · ") + fi.fileName());
        QString tip = j.preset.isEmpty() ? T("Eigene Einstellungen") : j.preset;
        if (j.status == RenderStatus::Done) tip += '\n' + T("Doppelklick: im Dateimanager zeigen");
        item->setToolTip(tip); // the list shows it (the card lets the mouse through)
        const bool current = j.status == RenderStatus::Rendering && m_queue->currentJob() == j.id;
        card->status->setText(current && m_queue->isPreparing() ? T("Wird vorbereitet …")
                              : current                         ? T("Rendert %1 %").arg(m_queue->currentProgress())
                              : j.status == RenderStatus::Failed ? T("Fehler")
                                                                 : renderStatusText(j));
        card->message->setFullText(j.message);
        // Failed: the error; done: a note (e.g. the hardware encoder fell back to software)
        card->message->setVisible((j.status == RenderStatus::Failed || j.status == RenderStatus::Done) && !j.message.isEmpty());
        card->status->setStyleSheet(QString("color: %1; font-size: 8pt;").arg(statusColor(j.status)));
        QStringList tip;
        if (!j.message.isEmpty()) tip << j.message;
        if (j.status == RenderStatus::Done && !j.encoderUsed.isEmpty()) tip << T("Video-Encoder: %1").arg(j.encoderUsed);
        card->status->setToolTip(tip.join('\n'));
        card->status->setMaximumWidth(140);
        // Which timeline (like DaVinci in the job card); a deleted timeline stays as a hint
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
    QElapsedTimer busy;
    busy.start();
    m_queue->start(selectedJobs());
    m_ignoreClicksBefore = m_clickTime + quint64(busy.elapsed());
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

void RenderQueuePanel::moveJobs(const QVector<int>& ids, int beforeId)
{
    const int running = m_queue->isRunning() ? m_queue->currentJob() : 0;
    m_project->setRenderQueue(moveRenderJobs(m_project->renderQueue(), ids, beforeId, running));
}

void RenderQueuePanel::jobDoubleClicked(int id)
{
    for (const RenderJob& j : m_project->renderQueue())
        if (j.id == id) {
            // Finished: where the file is (what one wants next); otherwise take the settings over
            const QString file = firstOutputFile(j.path, j.settings);
            if (j.status == RenderStatus::Done && QFileInfo::exists(file)) emit revealFileRequested(file);
            else emit loadJobRequested(id);
            return;
        }
}

void RenderQueuePanel::keyPressEvent(QKeyEvent* e)
{
    if (InputBindings::matches(m_deleteAction, e)) {
        m_deleteAction->trigger();
        return;
    }
    QWidget::keyPressEvent(e);
}

bool RenderQueuePanel::eventFilter(QObject* obj, QEvent* e)
{
    // The panel's keys (Del deletes jobs) win over the window's while the list has the focus: the timeline's Del
    // must not delete clips (registerPanelAction)
    // keep cards at list width
    if (obj == m_list->viewport() && e->type() == QEvent::Resize)
        for (int i = 0; i < m_list->count(); ++i) {
            QListWidgetItem* item = m_list->item(i);
            item->setSizeHint(QSize(m_list->viewport()->width() - 4, item->sizeHint().height()));
        }
    if (obj == m_renderBtn && (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonRelease ||
                               e->type() == QEvent::MouseButtonDblClick)) {
        const quint64 time = static_cast<QInputEvent*>(e)->timestamp();
        if (time < m_ignoreClicksBefore) return true; // clicked while the start blocked the window
        if (e->type() == QEvent::MouseButtonRelease) m_clickTime = time; // the release triggers clicked()
    }
    if ((obj == m_list || obj == m_renderBtn) && (e->type() == QEvent::ShortcutOverride || e->type() == QEvent::KeyPress)
        && InputBindings::matches(m_deleteAction, static_cast<QKeyEvent*>(e))) {
        e->accept(); // ShortcutOverride: no window shortcut, it comes as a key press instead
        if (e->type() == QEvent::KeyPress) m_deleteAction->trigger();
        return e->type() == QEvent::KeyPress;
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
    // only useful if a selected job is finished/failed/canceled (not already waiting or running)
    const QVector<RenderJob> queue = m_project->renderQueue();
    reset->setEnabled(std::any_of(queue.begin(), queue.end(), [&](const RenderJob& j) {
        return ids.contains(j.id) && j.status != RenderStatus::Queued && j.status != RenderStatus::Rendering;
    }));
    // The rendered file of a finished job (image sequences: the first frame in their folder)
    QString file;
    for (const RenderJob& j : queue)
        if (ids.size() == 1 && j.id == ids.first() && j.status == RenderStatus::Done) file = firstOutputFile(j.path, j.settings);
    const bool exists = !file.isEmpty() && QFileInfo::exists(file);
    menu.addSeparator();
    QAction* reveal = menu.addAction(T("Im Dateimanager zeigen"), this, [this, file] { emit revealFileRequested(file); });
    reveal->setObjectName("revealJob");
    reveal->setEnabled(exists);
    QAction* open = menu.addAction(T("Mit Standardprogramm öffnen"), this, [this, file] { emit openFileRequested(file); });
    open->setObjectName("openJob");
    open->setEnabled(exists);
    menu.addSeparator();
    QAction* del = menu.addAction(T("Löschen"), this, [this, ids] { removeJobs(ids); });
    del->setShortcuts(m_deleteAction->shortcuts()); // shows the bound key (and works while the menu is open)
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
