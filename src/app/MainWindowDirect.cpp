// SPDX-License-Identifier: AGPL-3.0-only
// MainWindow: the Direct page "Regie" (0.25) - built lazily on its first showing. The page asks, this file runs its
// DirectBackend: suggestions in the background (progress and ✕ in BackgroundJobs), a click on a suggestion plays it in
// the viewer up to its end (previewFile: the project stays unmodified), Übernehmen = one undo step.
#include "app/MainWindow.h"

#include "app/DirectBackend.h"
#include "app/InputBindings.h"
#include "app/TemplateDialogs.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "ui/BackgroundJobs.h"
#include "ui/direct/DirectMaterial.h"
#include "ui/direct/DirectPage.h"
#include "ui/direct/DirectRecipes.h"
#include "ui/direct/DirectSuggestions.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <cmath>

namespace {
constexpr const char* kJobId = "direct";
}

void MainWindow::setDirectBackend(std::unique_ptr<DirectBackend> backend)
{
    if (m_directCancel) { // a run of the old backend: its reply would come from something gone
        auto cancel = std::move(m_directCancel);
        m_directCancel = {};
        cancel();
    }
    m_directBackend = std::move(backend);
}

DirectBackend* MainWindow::directBackend()
{
    if (!m_directBackend) {
        // FAKE until package R1 hands in the backend of the `direct` command (main.cpp)
        m_directBackend = std::make_unique<FakeDirectBackend>(
            [this](const QString& command, const QJsonObject& args, const QString& undoText) {
                return m_userRunner ? m_userRunner(command, args, undoText) : QJsonObject{};
            },
            [this](const QString& file) -> double {
                const double fps = m_project->format().rate.fps();
                if (file == "@current") return TimelineOps::endFrame(m_project->timeline()) / fps;
                if (const MediaInfo* m = m_project->mediaInfo(file)) return m->length / fps;
                if (const auto cached = m_probeCache.find(file)) return cached->info.length / fps;
                return Engine::probe(m_project->format(), file).length / fps;
            });
    }
    return m_directBackend.get();
}

void MainWindow::ensureDirectPage()
{
    if (m_directPage) return;
    m_directPage = new DirectPage;
    m_pages->addWidget(m_directPage);
    watchSizes(m_directPage->root(), "directRoot");
    watchSizes(m_directPage->columns(), "directColumns");
    watchSizes(m_directPage->center(), "directCenter");
    m_directPage->setFps(m_project->format().rate.fps());
    connect(m_project, &Project::formatChanged, m_directPage, [this] { m_directPage->setFps(m_project->format().rate.fps()); });
    // Pictures of the suggestions: the frame at their start (worker thread, Engine::thumbnail is static)
    m_directPage->suggestions()->setThumbnailer([format = m_project->format()](const QString& file, double s, const QSize& size) {
        return Engine::thumbnail(format, file, int(std::lround(s * format.rate.fps())), size);
    });
    connect(m_directPage, &DirectPage::suggestRequested, this, &MainWindow::directSuggest);
    connect(m_directPage, &DirectPage::applyRequested, this, &MainWindow::directApply);
    connect(m_directPage, &DirectPage::openEditRequested, this, [this] { showPage(Page::Edit); });
    // Export: the Deliver page (its platform presets; `check` before the render runs there)
    connect(m_directPage, &DirectPage::exportRequested, this, [this] { showPage(Page::Deliver); });
    connect(m_directPage->suggestions(), &DirectSuggestions::previewRequested, this, &MainWindow::directPreview);
    // The material changed: the suggestions belong to the old one
    connect(m_directPage->material(), &DirectMaterial::changed, this, [this] {
        if (m_directPage->suggestions()->hasSession()) m_directPage->suggestions()->clear();
        m_directPage->updateResult();
    });
    DirectRecipes* recipes = m_directPage->recipes();
    connect(recipes, &DirectRecipes::templateChooseRequested, this, [this, recipes] {
        const ProjectFormat format = m_project->format();
        TemplateBrowserDialog dlg(this, [format](const QString& video, const QSize& size) {
            return Engine::thumbnail(format, video, 0, size);
        });
        if (dlg.exec() != QDialog::Accepted) return;
        if (dlg.choice() == TemplateBrowserDialog::Choice::NewContent || dlg.choice() == TemplateBrowserDialog::Choice::Edit)
            recipes->setTemplate(dlg.content().id, dlg.content().name);
    });
    connect(recipes, &DirectRecipes::scriptFileRequested, this, [this, recipes] {
        const QString file = QFileDialog::getOpenFileName(this, T("Skript wählen"), {}, T("Text (*.txt *.md);;Alle Dateien (*)"));
        if (!file.isEmpty()) recipes->setScriptFile(file);
    });
    // A previewed suggestion stops at its end
    connect(m_engine, &Engine::positionChanged, m_directPage, [this](int f) {
        if (m_directStopAt < 0) return;
        if (m_engine->timelinePositions() || m_engine->sourcePath() != m_directStopFile) {
            m_directStopAt = -1; // the user went on with something else
            return;
        }
        if (f >= m_directStopAt) {
            m_directStopAt = -1;
            m_engine->pause();
        }
    });
    // Delete on a suggestion or a material clip takes it out of that list, never timeline clips
    InputBindings::instance().registerPanelAction(m_directPage->suggestions()->removeAction(), "direct_remove_suggestion",
                                                  T("Regie"), "direct_suggestions", QKeySequence(Qt::Key_Delete),
                                                  QKeySequence(Qt::Key_Backspace));
    InputBindings::instance().registerPanelAction(m_directPage->material()->removeAction(), "direct_remove_material",
                                                  T("Regie"), "direct_material", QKeySequence(Qt::Key_Delete),
                                                  QKeySequence(Qt::Key_Backspace));
    InputBindings::instance().shieldWindowKeys(m_directPage,
                                               {"delete", "delete_alt", "ripple_delete", "ripple_delete_alt", "cut"});
}

void MainWindow::directSuggest()
{
    if (!m_directPage || m_directPage->material()->isEmpty()) return;
    if (m_directCancel) { // a new request replaces the running one
        auto cancel = std::move(m_directCancel);
        m_directCancel = {};
        cancel();
    }
    const int run = ++m_directRun;
    const QString recipe = m_directPage->recipes()->recipe();
    m_directPage->setBusy(true, T("Analysiert das Material …"));
    BackgroundJobs::Job job;
    job.text = T("Regie: Analyse");
    job.fraction = 0;
    job.cancel = [this, run] {
        if (run != m_directRun || !m_directCancel) return;
        auto cancel = std::move(m_directCancel);
        m_directCancel = {};
        cancel();
    };
    if (m_backgroundJobs) m_backgroundJobs->setJob(kJobId, job);
    const auto progress = [this, run, job](double fraction, const QString& stage) mutable {
        if (run != m_directRun || !m_backgroundJobs) return;
        job.fraction = fraction;
        job.toolTip = stage;
        m_backgroundJobs->setJob(kJobId, job);
    };
    const auto done = [this, run](const QJsonObject& reply) {
        if (run != m_directRun) return; // an older run's late reply
        m_directCancel = {};
        m_directPage->setBusy(false);
        if (reply.value("ok").toBool()) {
            const QJsonObject session = reply.value("result").toObject().value("session").toObject();
            m_directPage->material()->setMaterialState(session.value("material").toArray());
            m_directPage->suggestions()->setSession(session);
            if (m_backgroundJobs) m_backgroundJobs->finishJob(kJobId, T("Regie: %1 Vorschläge").arg(session.value("suggestions").toArray().size()));
            return;
        }
        const QJsonObject error = reply.value("error").toObject();
        if (m_backgroundJobs) m_backgroundJobs->removeJob(kJobId);
        if (error.value("code").toString() == "CANCELED") {
            m_directPage->suggestions()->setError(T("Abgebrochen."));
            return;
        }
        m_directPage->suggestions()->setError(T("Keine Vorschläge: %1").arg(error.value("message").toString()));
    };
    m_directCancel = directBackend()
                         ->suggest(recipe, m_directPage->material()->materialJson(), m_directPage->recipes()->options(),
                                   progress, done, m_directPage)
                         .cancel;
}

void MainWindow::directApply()
{
    if (!m_directPage || !m_directPage->suggestions()->hasSession()) return;
    m_directStopAt = -1;
    const QJsonObject reply =
        directBackend()->apply(m_directPage->suggestions()->session(), m_directPage->suggestions()->choice());
    if (!reply.value("ok").toBool()) {
        QMessageBox::warning(this, T("Regie"),
                             T("Nicht übernommen: %1").arg(reply.value("error").toObject().value("message").toString()));
        return;
    }
    statusBar()->showMessage(T("Regie: neue Timeline gebaut (Rückgängig nimmt sie zurück)"), 5000);
}

void MainWindow::directPreview(const QString& file, double from, double to)
{
    if (!QFileInfo(file).isFile()) return; // a range of the timeline (pauses): not previewed this way yet
    const double fps = m_project->format().rate.fps();
    const int in = int(std::lround(from * fps)), out = int(std::lround(to * fps));
    // previewFile shows the file without touching the project (no marks set: the project stays unmodified); it
    // starts where m_sourcePos says
    m_sourcePos.insert(file, in);
    previewFile(file);
    m_engine->seek(in);
    m_directStopAt = std::max(in + 1, out);
    m_directStopFile = m_engine->sourcePath();
    m_engine->play();
}
