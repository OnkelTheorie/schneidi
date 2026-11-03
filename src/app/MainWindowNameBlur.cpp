// SPDX-License-Identifier: AGPL-3.0-only
// MainWindow: hide player names (core/NameBlur.h, engine/TextRegions.h) - the clip menu entry. A small dialog picks
// the game template and the look; the measuring runs in a thread behind a progress dialog; the masks become windows
// of the clips' grade in one undo step, to review (and delete false ones) on the Color page.
#include "app/MainWindow.h"

#include "core/Editor.h"
#include "core/GradeWindows.h"
#include "core/I18n.h"
#include "core/NameBlur.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/TextRegions.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressDialog>
#include <QSpinBox>
#include <QStatusBar>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <memory>

namespace {

struct Choice {
    NameBlur::Template tpl = NameBlur::Template::All;
    QString style = "blur";
    int strength = 60;
};

bool ask(QWidget* parent, Choice* out)
{
    QDialog d(parent);
    d.setWindowTitle(T("Namen verwischen"));
    auto* form = new QFormLayout(&d);
    auto* info = new QLabel(T("Findet Text im Bild (Killfeed, Chat, Namen über Köpfen) und verwischt ihn, solange er "
                              "zu sehen ist. Das Ergebnis sind Fenster auf der Farbseite: dort prüfen und falsche "
                              "löschen."));
    info->setWordWrap(true);
    form->addRow(info);
    auto* tpl = new QComboBox;
    tpl->addItem(T("Alle Texte"), "all");
    tpl->addItem("Counter-Strike 2", "cs2");
    tpl->addItem("Valorant", "valorant");
    tpl->setToolTip(T("Das Spiel bestimmt, welcher Text zählt (Killfeed, Chat, Namen) und welcher nie "
                      "(Munition, Leben, Zeit, Fadenkreuz). Alle Texte: jeder Text im Bild."));
    form->addRow(T("Vorlage"), tpl);
    auto* style = new QComboBox;
    style->addItem(T("Verwischen"), "blur");
    style->addItem(T("Verpixeln"), "pixelate");
    form->addRow(T("Art"), style);
    auto* strength = new QSpinBox;
    strength->setRange(1, 100);
    strength->setValue(60);
    form->addRow(T("Stärke"), strength);
    QObject::connect(style, &QComboBox::currentIndexChanged, strength,
                     [style, strength] { strength->setValue(style->currentData() == "blur" ? 60 : 100); });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    form->addRow(buttons);
    if (d.exec() != QDialog::Accepted) return false;
    NameBlur::templateFromId(tpl->currentData().toString(), &out->tpl);
    out->style = style->currentData().toString();
    out->strength = strength->value();
    return true;
}

} // namespace

void MainWindow::blurNames()
{
    QVector<Clip> clips;
    const Timeline& tl = m_project->timeline();
    // None selected = the whole timeline; the kill feed copies above reframed clips show the same names
    const QVector<int> ids = m_editor->withKeptCopies(m_editor->clipIdsOf(m_selection->ids()));
    for (const Track& t : tl.video)
        for (const Clip& c : t.clips)
            if ((ids.isEmpty() || ids.contains(c.id)) && !c.isTitle() && !c.isNested()) clips << c;
    if (clips.isEmpty()) {
        statusBar()->showMessage(T("Namen verwischen: keine Video-Clips (Titel, Compound- und Multicam-Clips nicht)"), 6000);
        return;
    }
    Choice choice;
    if (!ask(this, &choice)) return;
    const ProjectFormat format = m_project->timelineFormat();
    const int sequence = m_project->currentSequence();

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto progress = std::make_shared<std::atomic<int>>(0); // of the clip being measured, 0..1000
    auto done = std::make_shared<std::atomic<int>>(0);     // clips measured
    auto result = std::make_shared<QHash<int, QVector<NameBlur::Mask>>>();
    auto errors = std::make_shared<QStringList>();
    auto dropped = std::make_shared<int>(0);
    const NameBlur::Template tpl = choice.tpl;
    QThread* thread = QThread::create([=] {
        for (int i = 0; i < clips.size() && !cancel->load(); ++i) {
            const Clip& c = clips[i];
            progress->store(0);
            const TextRegions::Result r = TextRegions::analyze(format, c, cancel.get(), progress.get());
            done->store(i + 1);
            if (!r.error.isEmpty()) {
                *errors << r.error;
                continue;
            }
            NameBlur::Options o;
            o.maxMasks = GradeWindows::kMax - GradeWindows::list(c).size();
            o.samplesPerSecond = format.rate.fps() / TextRegions::sampleStep(format.rate);
            o.screen = TextRegions::screenOf(format, c);
            int d = 0;
            result->insert(c.id, NameBlur::plan(r.samples, tpl, c.length(), o, &d));
            *dropped += d;
        }
    });
    QProgressDialog dialog(T("Namen verwischen: Text wird gesucht …"), T("Abbrechen"), 0, 1000, this);
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setMinimumDuration(400);
    connect(&dialog, &QProgressDialog::canceled, this, [cancel] { cancel->store(true); });
    QEventLoop loop;
    connect(thread, &QThread::finished, &loop, &QEventLoop::quit);
    QMetaObject::Connection replaced = connect(m_project, &Project::loaded, &loop, [cancel, &loop] {
        cancel->store(true);
        loop.quit();
    });
    QTimer poll;
    poll.setInterval(100);
    connect(&poll, &QTimer::timeout, &dialog, [&] { 
        dialog.setValue(std::min<qint64>(999, (qint64(done->load()) * 1000 + progress->load()) / clips.size()));
    });
    thread->start();
    poll.start();
    loop.exec();
    disconnect(replaced);
    if (!thread->isFinished()) cancel->store(true);
    thread->wait();
    delete thread;
    poll.stop();
    dialog.reset();
    if (cancel->load()) return;
    if (m_project->currentSequence() != sequence) {
        statusBar()->showMessage(T("Namen verwischen abgebrochen: die Timeline hat sich während der Suche geändert"), 6000);
        return;
    }
    for (const Clip& c : clips) {
        const Clip* now = TimelineOps::findClip(m_project->timeline(), c.id);
        if (!now || now->mediaPath != c.mediaPath || now->in != c.in || now->out != c.out) {
            statusBar()->showMessage(T("Namen verwischen abgebrochen: die Timeline hat sich während der Suche geändert"), 6000);
            return;
        }
    }
    if (!errors->isEmpty()) QMessageBox::warning(this, T("Namen verwischen"), errors->join('\n'));
    int notPlaced = 0;
    const QHash<int, QVector<int>> added = m_editor->blurNames(*result, choice.style, choice.strength, &notPlaced);
    *dropped += notPlaced;
    int windows = 0;
    for (const QVector<int>& w : added) windows += w.size();
    QString text = windows ? T("Namen verwischen: %1 Fenster auf %2 Clips – auf der Farbseite prüfen, falsche löschen")
                                 .arg(windows)
                                 .arg(added.size())
                           : *dropped ? T("Namen verwischen: kein Fenster frei")
                                      : T("Namen verwischen: kein Text gefunden");
    if (*dropped) text += ' ' + T("(%1 Texte ohne freies Fenster: bleiben sichtbar)").arg(*dropped);
    statusBar()->showMessage(text, 10000);
}
