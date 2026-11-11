// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/BackgroundJobs.h"

#include "app/Theme.h"
#include "core/I18n.h"
#include "ui/ElidedLabel.h"

#include <QHBoxLayout>
#include <QProgressBar>
#include <QTimer>
#include <QToolButton>
#include <algorithm>
#include <cmath>

BackgroundJobs::BackgroundJobs(QWidget* parent) : QWidget(parent)
{
    setObjectName("background_jobs");
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred); // the status bar's room stays with its message
    m_bar = new QProgressBar;
    m_bar->setObjectName("background_jobs_bar");
    m_bar->setTextVisible(false);
    m_bar->setFixedSize(90, 5);
    m_bar->setStyleSheet(QString("QProgressBar { background: %2; border: none; border-radius: 2px; }"
                                 "QProgressBar::chunk { background: %1; border-radius: 2px; }")
                             .arg(Theme::primary.name(), Theme::well.name()));
    m_text = new ElidedLabel(Qt::ElideMiddle); // the file name may lose its middle, count and percent stay
    m_text->setObjectName("background_jobs_text");
    m_text->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    m_text->setMaximumWidth(fontMetrics().averageCharWidth() * 44);
    m_text->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred); // as wide as the text, no gap before the ✕
    m_cancel = new QToolButton;
    m_cancel->setObjectName("background_jobs_cancel");
    m_cancel->setText("✕");
    m_cancel->setAutoRaise(true);
    m_cancel->setFocusPolicy(Qt::NoFocus);
    connect(m_cancel, &QToolButton::clicked, this, [this] {
        const Entry* e = shownEntry();
        if (!e || e->done || !e->job.cancel) return;
        const auto cancel = e->job.cancel; // the call may change m_jobs
        cancel();
    });

    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(4, 0, 2, 0);
    lay->setSpacing(6);
    lay->addWidget(m_bar, 0, Qt::AlignVCenter);
    lay->addWidget(m_text);
    lay->addWidget(m_cancel);
    hide();
}

int BackgroundJobs::indexOf(const QString& id) const
{
    for (int i = 0; i < m_jobs.size(); ++i)
        if (m_jobs[i].id == id) return i;
    return -1;
}

void BackgroundJobs::setJob(const QString& id, const Job& job)
{
    int i = indexOf(id);
    if (i < 0) {
        m_jobs.append({id, job, false, nullptr});
    } else {
        Entry& e = m_jobs[i];
        e.job = job;
        e.done = false;
        if (e.timer) e.timer->stop();
    }
    refresh();
}

void BackgroundJobs::finishJob(const QString& id, const QString& doneText)
{
    const int i = indexOf(id);
    if (i < 0) {
        m_jobs.append({id, {}, true, nullptr});
    }
    Entry& e = m_jobs[i < 0 ? int(m_jobs.size()) - 1 : i];
    e.done = true;
    e.job.text = doneText;
    e.job.fraction = 1.0;
    e.job.toolTip.clear();
    e.job.cancel = nullptr;
    if (!e.timer) {
        e.timer = new QTimer(this);
        e.timer->setSingleShot(true);
        connect(e.timer, &QTimer::timeout, this, [this, id] {
            const int k = indexOf(id);
            if (k >= 0 && m_jobs[k].done) removeJob(id);
        });
    }
    e.timer->start(m_holdMs);
    refresh();
}

void BackgroundJobs::removeJob(const QString& id)
{
    const int i = indexOf(id);
    if (i < 0) return;
    if (m_jobs[i].timer) m_jobs[i].timer->deleteLater();
    m_jobs.remove(i);
    refresh();
}

QStringList BackgroundJobs::jobIds() const
{
    QStringList out;
    for (const Entry& e : m_jobs) out << e.id;
    return out;
}

bool BackgroundJobs::hasJob(const QString& id) const { return indexOf(id) >= 0; }

QString BackgroundJobs::text() const { return m_text->fullText(); }

int BackgroundJobs::percent() const
{
    if (m_bar->maximum() == 0 || m_bar->isHidden()) return -1;
    return (m_bar->value() + 5) / 10;
}

const BackgroundJobs::Entry* BackgroundJobs::shownEntry() const
{
    // A running job before a notice before a done one: a done text is only news while nothing else runs
    const Entry* notice = nullptr;
    for (const Entry& e : m_jobs) {
        if (e.done) continue;
        if (!e.job.notice) return &e;
        if (!notice) notice = &e;
    }
    if (notice) return notice;
    return m_jobs.isEmpty() ? nullptr : &m_jobs.front();
}

void BackgroundJobs::refresh()
{
    if (m_jobs.isEmpty()) {
        hide();
        return;
    }
    const Entry* shown = shownEntry();
    QString line = shown->job.text;
    int running = 0;
    for (const Entry& e : m_jobs) running += e.done || e.job.notice ? 0 : 1;
    if (running > 1) line += "  " + T("(+%1)").arg(running - 1);
    m_text->setFullText(line);
    QStringList tips;
    for (const Entry& e : m_jobs) {
        QString tip = e.job.text;
        if (!e.job.toolTip.isEmpty()) tip += "\n" + e.job.toolTip;
        tips << tip;
    }
    setToolTip(tips.join("\n\n"));
    m_text->setToolTip(toolTip());
    if (shown->job.fraction < 0) {
        m_bar->setRange(0, 0); // busy
    } else {
        m_bar->setRange(0, 1000);
        m_bar->setValue(int(std::lround(std::clamp(shown->job.fraction, 0.0, 1.0) * 1000)));
    }
    m_bar->setVisible(!shown->done && !shown->job.notice);
    m_cancel->setVisible(!shown->done && bool(shown->job.cancel));
    m_cancel->setToolTip(shown->done ? QString() : shown->job.notice ? T("Meldung schließen") : T("Abbrechen"));
    m_text->setStyleSheet(QString("color: %1;").arg((shown->job.notice ? Theme::warning : Theme::textDim).name()));
    show();
}
