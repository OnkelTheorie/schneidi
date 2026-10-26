// MainWindow: timelines from and to other editors (File › Import/Export Timeline, like DaVinci) – FCP7 XML,
// OpenTimelineIO, EDL. The formats are in core/Interchange.
#include "app/MainWindow.h"
#include "app/ContactSheetDialog.h"
#include "app/TimelineExportDialog.h"

#include "core/ContactSheet.h"
#include "core/I18n.h"
#include "core/Interchange.h"
#include "core/Project.h"
#include "core/Timecode.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "engine/Snapshot.h"
#include "ui/MediaPool.h"

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QApplication>
#include <QDesktopServices>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QStatusBar>
#include <QUndoStack>
#include <QUrl>

namespace {

QString startDir(const QString& projectPath)
{
    return QSettings().value("interchange/dir", projectPath.isEmpty() ? QString() : QFileInfo(projectPath).absolutePath()).toString();
}

// At most `n` lines of a list, "…" for the rest
QString shortList(const QStringList& lines, int n = 8)
{
    QStringList shown = lines.mid(0, n);
    if (lines.size() > n) shown << "…";
    return "• " + shown.join("\n• ");
}

// What was not carried over: a message that does not block (the timeline is already there)
void showNotes(QWidget* parent, const QString& title, const QString& text)
{
    auto* box = new QMessageBox(QMessageBox::Information, title, text, QMessageBox::Ok, parent);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->open();
}

} // namespace

void MainWindow::importTimelineDialog()
{
    const QString path = QFileDialog::getOpenFileName(
        this, T("Timeline importieren"), startDir(m_projectPath),
        T("Timelines (*.xml *.otio *.edl);;Final Cut Pro 7 XML (*.xml);;OpenTimelineIO (*.otio);;EDL (*.edl);;Alle Dateien (*)"));
    if (path.isEmpty()) return;
    QSettings().setValue("interchange/dir", QFileInfo(path).absolutePath());
    importTimelineFile(path);
}

bool MainWindow::importTimelineFile(const QString& path)
{
    const QString title = T("Timeline importieren");
    const auto format = Interchange::formatOf(path);
    QFile file(path);
    if (!format || !file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, title, T("%1 kann nicht gelesen werden (erwartet: .xml, .otio oder .edl).").arg(QFileInfo(path).fileName()));
        return false;
    }
    Interchange::Sequence seq;
    try {
        seq = Interchange::read(file.readAll(), *format, m_project->format().rate); // EDL: the project's frame rate
    } catch (const Interchange::Error& e) {
        QMessageBox::warning(this, title, T("Die Datei konnte nicht gelesen werden:\n%1").arg(e.message));
        return false;
    }
    // Media: as written in the file, or by name next to the file; else the user shows the folder (like DaVinci's
    // "Relink media" when a timeline comes from another computer)
    QStringList folders{QFileInfo(path).absolutePath()};
    QHash<QString, QString> local;
    const QStringList media = Interchange::mediaFiles(seq);
    const auto resolve = [&] {
        for (const QString& m : media)
            if (!local.contains(m))
                if (const QString f = Interchange::findMedia(m, folders); !f.isEmpty()) local.insert(m, f);
    };
    resolve();
    if (local.size() < media.size()) {
        QStringList missing;
        for (const QString& m : media)
            if (!local.contains(m)) missing << QFileInfo(QString(m).replace('\\', '/')).fileName();
        const auto answer = QMessageBox::question(
            this, title,
            T("%1 von %2 Mediendateien wurden nicht gefunden:\n%3\n\nOrdner auswählen, in dem sie liegen?")
                .arg(missing.size()).arg(media.size()).arg(shortList(missing, 5)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer == QMessageBox::Yes) {
            const QString dir = QFileDialog::getExistingDirectory(this, T("Ordner mit den Mediendateien"), folders.first());
            if (!dir.isEmpty()) {
                folders << dir;
                resolve();
            }
        }
    }
    m_engine->pause();
    QUndoStack* undo = m_project->undoStack();
    undo->beginMacro(title);
    // An empty project takes the file's format (like opening the timeline in DaVinci); EDL has none
    if (!m_project->frameRateLocked() && *format != Interchange::Format::Edl) {
        ProjectFormat f = m_project->format();
        f.rate = seq.format.rate;
        if (seq.hasSize) {
            f.width = seq.format.width;
            f.height = seq.format.height;
        }
        if (f != m_project->format()) setProjectFormat(f);
    }
    QStringList files;
    for (const QString& f : local)
        if (!m_project->mediaInfo(f) && !files.contains(f)) files << f;
    if (!files.isEmpty()) m_mediaPool->importFiles(files);
    const Interchange::Imported imp = Interchange::addToProject(*m_project, seq, local, {}, &Engine::startTimecode);
    undo->endMacro();
    const QString done = T("Timeline „%1“ importiert (%2 Clips).").arg(imp.name, QString::number(imp.clips));
    if (imp.warnings.isEmpty()) statusBar()->showMessage(done, 5000);
    else showNotes(this, title, done + "\n\n" + T("Hinweise:") + "\n" + shortList(imp.warnings));
    return true;
}

void MainWindow::exportTimelineDialog()
{
    const QString title = T("Timeline exportieren");
    TimelineExportDialog dialog(m_project->sequenceName(m_project->currentSequence()), startDir(m_projectPath), this);
    if (dialog.exec() != QDialog::Accepted) return;
    const QString path = dialog.path();
    const Interchange::Format format = dialog.format();
    if (QFileInfo::exists(path)
        && QMessageBox::question(this, title, T("%1 gibt es schon. Ersetzen?").arg(QFileInfo(path).fileName()))
               != QMessageBox::Yes)
        return;
    Interchange::Sequence seq = Interchange::fromTimeline(*m_project);
    Interchange::applyStartTimecodes(seq, &Engine::startTimecode);
    const QByteArray data = Interchange::write(seq, format);
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size() || !out.commit()) {
        QMessageBox::warning(this, title, T("Datei konnte nicht gespeichert werden:\n%1").arg(path));
        return;
    }
    const QString done = T("Timeline exportiert: %1").arg(QFileInfo(path).fileName());
    if (seq.warnings.isEmpty()) statusBar()->showMessage(done, 5000);
    else showNotes(this, title, done + "\n\n" + T("Nicht übernommen:") + "\n" + shortList(seq.warnings));
}

void MainWindow::contactSheetDialog()
{
    const QString title = T("Kontaktbogen speichern");
    const Timeline& tl = m_project->timeline();
    const int end = TimelineOps::endFrame(tl);
    if (end <= 0) {
        QMessageBox::information(this, title, T("Die Timeline ist leer."));
        return;
    }
    const bool hasInOut = tl.markIn >= 0 && tl.markOut > tl.markIn;
    ContactSheetDialog dialog(m_project->sequenceName(m_project->currentSequence()), startDir(m_projectPath), hasInOut,
                              this);
    if (dialog.exec() != QDialog::Accepted) return;
    const int from = dialog.inOut() ? tl.markIn : 0;
    const int to = dialog.inOut() ? std::min(end, tl.markOut + 1) : end;
    const QVector<int> frames = ContactSheet::spread(from, to, std::min(dialog.count(), to - from));
    const ProjectFormat& f = m_project->format();

    QProgressDialog progress(T("Bilder werden geholt…"), T("Abbrechen"), 0, int(frames.size()), this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    int failed = -1;
    // The picture as exported (compound clips, effects, grades)
    const QImage sheet = Snapshot::contactSheet(f, m_project->renderTimeline(), frames, dialog.tileWidth(),
                                                dialog.columns(), [&progress](int done) {
                                                    progress.setValue(done);
                                                    QApplication::processEvents();
                                                    return !progress.wasCanceled();
                                                }, &failed);
    if (sheet.isNull()) {
        if (failed >= 0)
            QMessageBox::warning(this, title, T("Bild %1 konnte nicht geholt werden.").arg(Timecode::format(failed, m_project->fps())));
        return;
    }
    const QString path = dialog.path();
    if (!sheet.save(path, nullptr, 90)) {
        QMessageBox::warning(this, title, T("Datei konnte nicht gespeichert werden:\n%1").arg(path));
        return;
    }
    QMessageBox box(QMessageBox::Information, title, T("Kontaktbogen gespeichert: %1").arg(QFileInfo(path).fileName()),
                    QMessageBox::Close, this);
    QPushButton* open = box.addButton(T("Öffnen"), QMessageBox::ActionRole);
    QPushButton* folder = box.addButton(T("Im Ordner zeigen"), QMessageBox::ActionRole);
    box.exec();
    if (box.clickedButton() == open) QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    else if (box.clickedButton() == folder) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}
