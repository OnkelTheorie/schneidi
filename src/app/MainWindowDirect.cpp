// SPDX-License-Identifier: AGPL-3.0-only
// MainWindow: the Direct page "Regie" (0.25) - built lazily on its first showing. The page asks, this file runs its
// DirectBackend: suggestions and Übernehmen in the background (progress and ✕ in BackgroundJobs), a click on a
// suggestion plays it in the viewer up to its end (previewFile: the project stays unmodified), Übernehmen = one undo
// step. A run never outlives its material or project: directReset ends it. The own wish line goes to the AI chat
// (with the page's context as a note); the model's `direct` comes back through LiveHost::liveDirection.
#include "app/MainWindow.h"

#include "app/AiChat.h"
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
#include "ui/direct/DirectThumbs.h"
#include "ui/direct/DirectWish.h"

#include <QDir>
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
#include <memory>

namespace {
constexpr const char* kJobId = "direct";
constexpr const char* kApplyJobId = "direct_apply";
} // namespace

void MainWindow::setDirectBackend(std::unique_ptr<DirectBackend> backend)
{
    directReset(false); // a run of the old backend: its reply would come from something gone
    m_directBackend = std::move(backend);
    if (m_directPage) m_directPage->recipes()->setAvailable(m_directBackend->recipes());
}

void MainWindow::liveDirection(const QJsonObject& session, bool ownChat)
{
    if (ownChat && m_directWishVoid) return; // the page was reset during the request: a late answer to old material
    ensureDirectPage();
    showDirectSession(session);
}

void MainWindow::liveDirectApplied(const QJsonObject& session, const QJsonObject& result, bool ownChat)
{
    if (ownChat && m_directWishVoid) return;
    // Shown like the page's own Übernehmen: its session, the short's finish for the apply's result timeline (for a
    // short its vertical copy); without one nothing to finish
    ensureDirectPage();
    showDirectSession(session);
    const bool newProject = result.value("new_project").toBool();
    m_directApplied = {{"session", session}, {"result", result}};
    m_directAppliedByAi = true;
    m_directPage->setFinishOffered(!newProject && !result.value("timeline").toString().isEmpty()
                                   && directBackend()->canFinish(session));
    if (newProject) {
        m_directPage->setMadeProject(result.value("project").toString());
        return;
    }
    statusBar()->showMessage(T("Regie: Timeline von der KI gebaut (Rückgängig nimmt sie zurück)"), 6000);
}

void MainWindow::directAiApplyUndone()
{
    if (!m_directAppliedByAi) return;
    m_directAppliedByAi = false;
    m_directApplied = {};
    if (m_directPage) m_directPage->setFinishOffered(false);
}

void MainWindow::showDirectSession(const QJsonObject& session)
{
    ensureDirectPage();
    // A session from elsewhere (the chat) takes the place of the page's own runs: theirs end (late replies dropped),
    // a running Übernehmen/finish too (one that already runs on the UI thread still makes its step)
    ++m_directRun;
    ++m_directApplyRun;
    for (std::function<void()>* cancel : {&m_directCancel, &m_directApplyCancel})
        if (*cancel) {
            auto c = std::move(*cancel);
            *cancel = {};
            c();
        }
    if (m_backgroundJobs) {
        m_backgroundJobs->removeJob(kJobId);
        m_backgroundJobs->removeJob(kApplyJobId);
    }
    m_directPage->setBusy(false); // also the Suggest button anew
    m_directPage->setApplying(false);
    m_directPage->setFinishOffered(false);
    m_directPage->setMadeProject({});
    m_directApplied = {};
    QStringList files;
    for (const QJsonValue& v : session.value("material").toArray()) files << v.toObject().value("file").toString();
    {
        const QSignalBlocker b(m_directPage->material()); // its changed() would drop the session again
        if (session.value("timeline").toString().isEmpty()) {
            m_directPage->material()->setUseTimeline(false);
            if (m_directPage->material()->files() != files) {
                m_directPage->material()->clear();
                m_directPage->material()->addFiles(files);
            }
        } else {
            m_directPage->material()->setUseTimeline(true);
        }
    }
    m_directPage->recipes()->setRecipe(session.value("recipe").toString());
    m_directPage->material()->setMaterialState(session.value("material").toArray());
    m_directPage->suggestions()->setSession(session);
    m_directPage->updateResult();
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
            },
            [this] { return QString::number(m_project->currentSequence()); });
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
    // Pictures of the lists: the frame at a time of the file (own low-priority threads, Engine::thumbnail is static)
    m_directPage->thumbs()->setDecoder([format = m_project->format()](const QString& file, double s, const QSize& size) {
        return Engine::thumbnail(format, file, int(std::lround(s * format.rate.fps())), size);
    });
    // The Delete keys registered at the start (buildActions) act in the two lists
    m_directPage->suggestions()->setRemoveAction(m_directRemoveSuggestion);
    m_directPage->material()->setRemoveAction(m_directRemoveMaterial);
    // The window's keys that delete timeline clips do nothing in the page (also guarded by the page itself:
    // MainWindowActions' notOnDirect)
    InputBindings::instance().shieldWindowKeys(m_directPage,
                                               {"delete", "delete_alt", "ripple_delete", "ripple_delete_alt", "cut"});
    connect(m_directPage, &DirectPage::suggestRequested, this, &MainWindow::directSuggest);
    connect(m_directPage, &DirectPage::applyRequested, this, &MainWindow::directApply);
    connect(m_directPage, &DirectPage::finishRequested, this, &MainWindow::directFinish);
    // A template's new project: opened like File › Open (the question about unsaved changes first)
    connect(m_directPage, &DirectPage::openProjectRequested, this, [this](const QString& path) {
        if (path.isEmpty() || !maybeLeaveProject()) return;
        openProject(path);
    });
    m_directPage->recipes()->setAvailable(directBackend()->recipes());
    connect(m_directPage, &DirectPage::openEditRequested, this, [this] { showPage(Page::Edit); });
    // Export: the Deliver page (its platform presets; `check` before the render runs there)
    connect(m_directPage, &DirectPage::exportRequested, this, [this] { showPage(Page::Deliver); });
    connect(m_directPage->suggestions(), &DirectSuggestions::previewRequested, this, &MainWindow::directPreview);
    // The material changed: a run on the old one ends, its suggestions go
    connect(m_directPage->material(), &DirectMaterial::changed, this, [this] { directReset(false); });
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
    connect(recipes, &DirectRecipes::musicFileRequested, this, [this, recipes] {
        recipes->setMusicFile(QFileDialog::getOpenFileName(this, T("Musik wählen"), {},
                                                           T("Audio (*.wav *.mp3 *.flac *.ogg *.m4a *.opus);;Alle Dateien (*)")));
    });
    updateDirectTracks();
    connect(m_project, &Project::timelineChanged, m_directPage, [this] { updateDirectTracks(); });
    // The own wish: to the AI chat; ✕ stops its turn
    connect(m_directPage->wish(), &DirectWish::wishRequested, this, &MainWindow::directWish);
    connect(m_directPage->wish(), &DirectWish::stopRequested, this, [this] {
        if (m_directWishTurn && m_chat) m_chat->stop(); // its turnFinished ends the wish (directWishDone)
    });
    updateDirectWish();
    // A previewed suggestion stops at its end; anything else in the viewer or a pause by the user ends that
    connect(m_engine, &Engine::positionChanged, m_directPage, [this](int f) {
        if (m_directStopAt < 0) return;
        if (m_engine->timelinePositions() || m_engine->sourcePath() != m_directStopFile) {
            m_directStopAt = -1;
            return;
        }
        if (f >= m_directStopAt) {
            m_directStopAt = -1;
            m_engine->pause();
        }
    });
    // Paused by the user (the stop's own pause cleared it first): the next play goes on freely
    connect(m_engine, &Engine::speedChanged, m_directPage, [this](double speed) {
        if (speed == 0.0) m_directStopAt = -1;
    });
}

void MainWindow::updateDirectTracks()
{
    if (!m_directPage) return;
    QStringList names;
    const QVector<Track>& audio = m_project->timeline().audio;
    for (int i = 0; i < audio.size(); ++i)
        names << (audio[i].name.isEmpty() ? QString("A%1").arg(i + 1) : QString("A%1 %2").arg(i + 1).arg(audio[i].name));
    m_directPage->recipes()->setAudioTracks(names);
}

void MainWindow::directReset(bool material)
{
    // A wish the AI still works on was about the old material/project: stopped, and nothing it does is shown
    if (m_directWishTurn && m_chat) {
        m_directWishVoid = true;
        m_chat->stop(); // its turnFinished ends the wish (directWishDone)
    }
    m_directAppliedByAi = false;
    ++m_directRun; // late replies of what runs now are dropped
    ++m_directApplyRun;
    for (std::function<void()>* cancel : {&m_directCancel, &m_directApplyCancel})
        if (*cancel) {
            auto c = std::move(*cancel);
            *cancel = {};
            c();
        }
    if (m_backgroundJobs) {
        m_backgroundJobs->removeJob(kJobId);
        m_backgroundJobs->removeJob(kApplyJobId);
    }
    m_directStopAt = -1;
    if (!m_directPage) return;
    m_directPage->setBusy(false);
    m_directPage->setApplying(false);
    m_directPage->setFinishOffered(false);
    m_directPage->setMadeProject({});
    m_directApplied = {};
    if (material) {
        const QSignalBlocker b(m_directPage->material()); // its changed() would come back here
        m_directPage->material()->clear();
    }
    m_directPage->suggestions()->clear();
    m_directPage->updateResult();
}

void MainWindow::directSuggest()
{
    if (!m_directPage || m_directPage->material()->isEmpty()) return;
    const int run = ++m_directRun; // first: the old run's reply (also a CANCELED from the cancel below) is dropped
    if (m_directCancel) {
        auto cancel = std::move(m_directCancel);
        m_directCancel = {};
        cancel();
    }
    const QString recipe = m_directPage->recipes()->recipe();
    m_directPage->suggestions()->setStale({});
    m_directPage->setBusy(true, T("Analysiert das Material …"));
    BackgroundJobs::Job job;
    job.text = T("Regie: Analyse");
    job.fraction = 0;
    job.cancel = [this, run] {
        if (run != m_directRun || !m_directCancel) return;
        auto cancel = std::move(m_directCancel);
        m_directCancel = {};
        cancel(); // its CANCELED reply ends the run below
    };
    if (m_backgroundJobs) m_backgroundJobs->setJob(kJobId, job);
    const auto progress = [this, run, job](double fraction, const QString& stage) mutable {
        if (run != m_directRun || !m_backgroundJobs) return;
        job.fraction = fraction;
        job.toolTip = stage;
        m_backgroundJobs->setJob(kJobId, job);
    };
    auto finished = std::make_shared<bool>(false);
    const auto done = [this, run, finished](const QJsonObject& reply) {
        *finished = true;
        if (run != m_directRun) return; // an older run's late reply
        m_directCancel = {};
        m_directPage->setBusy(false);
        if (reply.value("ok").toBool()) {
            const QJsonObject session = reply.value("result").toObject().value("session").toObject();
            m_directPage->setFinishOffered(false);
            m_directPage->setMadeProject({});
            m_directPage->material()->setMaterialState(session.value("material").toArray());
            m_directPage->suggestions()->setSession(session);
            if (m_backgroundJobs)
                m_backgroundJobs->finishJob(kJobId, T("Regie: %1 Vorschläge").arg(session.value("suggestions").toArray().size()));
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
    DirectBackend::JobHandle h = directBackend()->suggest(recipe, m_directPage->material()->materialJson(),
                                                          m_directPage->recipes()->options(), progress, done, m_directPage);
    if (!*finished && run == m_directRun) m_directCancel = std::move(h.cancel); // a backend may answer at once
}

void MainWindow::directApply()
{
    if (!m_directPage || !m_directPage->suggestions()->hasSession() || m_directPage->applying()) return;
    m_directStopAt = -1;
    m_engine->pause();
    const int run = ++m_directApplyRun;
    m_directPage->setApplying(true);
    BackgroundJobs::Job job;
    job.text = T("Regie: Timeline bauen");
    job.fraction = -1;
    job.cancel = [this, run] {
        if (run != m_directApplyRun || !m_directApplyCancel) return;
        auto cancel = std::move(m_directApplyCancel);
        m_directApplyCancel = {};
        cancel();
    };
    if (m_backgroundJobs) m_backgroundJobs->setJob(kApplyJobId, job);
    const auto progress = [this, run, job](double fraction, const QString& stage) mutable {
        if (run != m_directApplyRun || !m_backgroundJobs) return;
        job.fraction = fraction;
        job.toolTip = stage;
        m_backgroundJobs->setJob(kApplyJobId, job);
    };
    auto finished = std::make_shared<bool>(false);
    const auto done = [this, run, finished](const QJsonObject& reply) {
        *finished = true;
        if (run != m_directApplyRun) return;
        m_directApplyCancel = {};
        m_directPage->setApplying(false);
        if (reply.value("ok").toBool()) {
            const bool newProject = reply.value("result").toObject().value("new_project").toBool();
            if (m_backgroundJobs)
                m_backgroundJobs->finishJob(kApplyJobId, newProject ? T("Regie: Projekt gebaut") : T("Regie: Timeline gebaut"));
            // A short: its captions and effect sounds are offered as the next step
            const QJsonObject session = m_directPage->suggestions()->session();
            m_directApplied = {{"session", session}, {"result", reply.value("result")}};
            m_directAppliedByAi = false;
            m_directPage->setFinishOffered(directBackend()->canFinish(session));
            // A template filled into a new project file: nothing changed here (no undo step), the page offers it
            const QJsonObject result = reply.value("result").toObject();
            if (result.value("new_project").toBool()) {
                const QString path = result.value("project").toString();
                m_directPage->setMadeProject(path);
                statusBar()->showMessage(T("Regie: neues Projekt gespeichert: %1").arg(QDir::toNativeSeparators(path)), 8000);
                return;
            }
            statusBar()->showMessage(T("Regie: neue Timeline gebaut (Rückgängig nimmt sie zurück)"), 5000);
            return;
        }
        if (m_backgroundJobs) m_backgroundJobs->removeJob(kApplyJobId);
        const QJsonObject error = reply.value("error").toObject();
        const QString code = error.value("code").toString();
        if (code == "CANCELED") return;
        if (code == "SESSION_STALE") {
            m_directPage->suggestions()->setStale(T("Das Material hat sich geändert: Die Vorschläge passen nicht mehr."));
            return;
        }
        if (code == "SESSION_EXPIRED") {
            m_directPage->suggestions()->setStale(T("Die Vorschläge sind abgelaufen."));
            return;
        }
        QMessageBox::warning(this, T("Regie"), T("Nicht übernommen: %1").arg(error.value("message").toString()));
    };
    DirectBackend::JobHandle h = directBackend()->apply(m_directPage->suggestions()->session(),
                                                        m_directPage->suggestions()->choice(), progress, done, m_directPage);
    if (!*finished && run == m_directApplyRun) m_directApplyCancel = std::move(h.cancel);
}

void MainWindow::directFinish()
{
    if (!m_directPage || m_directApplied.isEmpty() || m_directPage->applying()) return;
    const int run = ++m_directApplyRun;
    m_directPage->setApplying(true, T("Untertitel und Effektklänge …"));
    BackgroundJobs::Job job;
    job.text = T("Regie: Untertitel und Effektklänge");
    job.fraction = -1;
    job.cancel = [this, run] {
        if (run != m_directApplyRun || !m_directApplyCancel) return;
        auto cancel = std::move(m_directApplyCancel);
        m_directApplyCancel = {};
        cancel();
    };
    if (m_backgroundJobs) m_backgroundJobs->setJob(kApplyJobId, job);
    const auto progress = [this, run, job](double fraction, const QString& stage) mutable {
        if (run != m_directApplyRun || !m_backgroundJobs) return;
        job.fraction = fraction;
        job.toolTip = stage;
        m_backgroundJobs->setJob(kApplyJobId, job);
    };
    auto finished = std::make_shared<bool>(false);
    const auto done = [this, run, finished](const QJsonObject& reply) {
        *finished = true;
        if (run != m_directApplyRun) return;
        m_directApplyCancel = {};
        m_directPage->setApplying(false);
        if (reply.value("ok").toBool()) {
            m_directPage->setFinishOffered(false); // done once
            if (m_backgroundJobs) m_backgroundJobs->finishJob(kApplyJobId, T("Regie: Untertitel und Effektklänge fertig"));
            const QJsonArray notes = reply.value("result").toObject().value("notes").toArray();
            statusBar()->showMessage(notes.isEmpty() ? T("Regie: Untertitel und Effektklänge eingefügt")
                                                     : T("Regie: %1").arg(notes.first().toString()),
                                     6000);
            return;
        }
        if (m_backgroundJobs) m_backgroundJobs->removeJob(kApplyJobId);
        const QJsonObject error = reply.value("error").toObject();
        const QString code = error.value("code").toString();
        // The short changed since: captions would land on the wrong times; offered again only after a new Übernehmen
        if (code == "FINISH_STALE" || code == "NOT_FOUND") {
            m_directPage->setFinishOffered(false);
            m_directApplied = {};
            statusBar()->showMessage(T("Regie: Der Short hat sich geändert, Untertitel und Effektklänge wurden nicht eingefügt."), 8000);
            return;
        }
        m_directPage->setFinishOffered(true);
        if (code == "CANCELED") return;
        QMessageBox::warning(this, T("Regie"), T("Nicht eingefügt: %1").arg(error.value("message").toString()));
    };
    DirectBackend::JobHandle h = directBackend()->finish(m_directApplied.value("session").toObject(),
                                                         m_directApplied.value("result").toObject(), progress, done,
                                                         m_directPage);
    if (!*finished && run == m_directApplyRun) m_directApplyCancel = std::move(h.cancel);
}

void MainWindow::directPreview(const QString& file, double from, double to)
{
    if (!QFileInfo(file).isFile()) return; // a range of the timeline (pauses): not previewed this way yet
    const double fps = m_project->format().rate.fps();
    const int in = int(std::lround(from * fps)), out = int(std::lround(to * fps));
    // previewFile shows the file without touching the project (no marks set: the project stays unmodified); it
    // starts where m_sourcePos says
    m_sourcePos.insert(file, in);
    previewFile(file); // (clears an older stop)
    m_engine->seek(in);
    m_engine->play();
    m_directStopAt = std::max(in + 1, out);
    m_directStopFile = m_engine->sourcePath();
}

void MainWindow::updateDirectWish()
{
    if (!m_directPage) return;
    m_directPage->wish()->setVisible(m_chat && chatReady());
}

void MainWindow::directWish(const QString& text)
{
    if (!m_directPage || !m_chat || text.trimmed().isEmpty() || m_directWishTurn) return;
    if (m_review.pending) {
        m_directPage->wish()->setNote(T("Erst die Änderungen der KI übernehmen oder verwerfen."), true);
        return;
    }
    if (m_chat->busy()) {
        m_directPage->wish()->setNote(T("Der KI-Chat ist gerade beschäftigt."), true);
        return;
    }
    const int first = int(m_chat->entries().size());
    m_directPage->setWishBusy(true);
    m_directWishTurn = true; // before the send: an answer at once (an error) ends it in chatTurnFinished
    m_directWishFirst = first;
    const bool plan = m_aiSettings.approval == AiSettings::Approval::Plan;
    if (sendChat(text.trimmed(), m_directPage->wishContext(plan))) return;
    // Not sent (no provider after all, the privacy question declined, a test run): the chat's error if it said one
    m_directWishTurn = false;
    m_directPage->setWishBusy(false);
    QString error;
    for (int i = first; i < m_chat->entries().size(); ++i)
        if (m_chat->entries()[i].kind == AiChat::Entry::Kind::Error) error = m_chat->entries()[i].text;
    m_directPage->wish()->setNote(error.isEmpty() ? T("Nicht an die KI geschickt.") : error, true);
}

void MainWindow::directWishDone()
{
    m_directWishTurn = false;
    if (!m_directPage) return;
    m_directPage->setWishBusy(false);
    // The turn's last answer of the model or `direct` call, and its last error: what came last wins
    QString answer, error;
    int lastAnswer = -1, lastError = -1;
    bool direct = false;
    const QVector<AiChat::Entry>& entries = m_chat->entries();
    for (int i = std::min<int>(m_directWishFirst, int(entries.size())); i < entries.size(); ++i) {
        const AiChat::Entry& e = entries[i];
        if (e.kind == AiChat::Entry::Kind::Assistant && !e.text.trimmed().isEmpty()) {
            answer = e.text.trimmed();
            lastAnswer = i;
        }
        if (e.kind == AiChat::Entry::Kind::Tool && e.tool == QLatin1String("direct") && e.state == AiChat::Entry::State::Done) {
            direct = true;
            lastAnswer = std::max(lastAnswer, i);
        }
        if (e.kind == AiChat::Entry::Kind::Error) {
            error = e.text;
            lastError = i;
        }
    }
    if (m_chat->wasStopped()) {
        m_directPage->wish()->setNote(m_directWishVoid ? QString() : T("Gestoppt."));
        return;
    }
    if (lastError > lastAnswer) {
        m_directPage->wish()->setNote(error, true);
        return;
    }
    constexpr int kMax = 300; // a short note, the whole answer is in the chat
    if (answer.size() > kMax) {
        int cut = kMax - 1;
        if (answer.at(cut - 1).isHighSurrogate()) --cut; // never half a character
        answer = answer.left(cut) + "…";
    }
    if (!direct && answer.isEmpty()) answer = T("Die KI hat nichts geändert.");
    m_directPage->wish()->setNote(answer);
}
