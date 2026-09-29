#include "app/MainWindow.h"

#include "app/InputBindings.h"
#include "app/KeyBindingsDialog.h"
#include "app/ClipSpeedDialog.h"
#include "app/NormalizeDialog.h"
#include "app/ProjectSettingsDialog.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Timecode.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"
#include "engine/Engine.h"
#include "engine/MediaCache.h"
#include "engine/Profiles.h"
#include "engine/ProxyManager.h"
#include "ui/DeliverPanel.h"
#include "ui/EffectsLibrary.h"
#include "ui/Inspector.h"
#include "ui/Mixer.h"
#include "ui/MediaPool.h"
#include "ui/Viewer.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QMouseEvent>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QMenuBar>
#include <QSplitter>
#include <QDesktopServices>
#include <QUrl>
#include <QUndoStack>
#include <QMenu>
#include <QProgressDialog>
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

MainWindow::MainWindow(Engine* engine, QWidget* parent) : QMainWindow(parent), m_engine(engine)
{
    setWindowTitle("schneidi");
    resize(1600, 950);

    m_project = new Project(this);
    m_selection = new Selection(this);
    m_editor = new Editor(m_project, m_selection, this);

    buildLayout();
    buildActions();
    qApp->installEventFilter(this); // Maus-Seitentasten -> Aktionen (keybindings.json, Abschnitt "mouse")

    // Modell -> Engine
    // Gebündelt: beim Ziehen (Fade-Griff, Lautstärkelinie, Inspector …) ändert jede Mausbewegung das Modell;
    // die Vorschau jedes Mal sofort neu zu bauen blockierte die Timeline (Griff hing hinterher).
    // Mixer-Änderungen sind billig (live gesetzt) und gehen weiter sofort durch.
    m_engineTimer.setSingleShot(true);
    m_engineTimer.setInterval(30);
    connect(&m_engineTimer, &QTimer::timeout, this, [this] { m_engine->updateTimeline(m_project->timeline()); });
    connect(m_project, &Project::timelineChanged, this, [this] {
        if (m_engine->mixerOnlyPending()) m_engine->updateTimeline(m_project->timeline());
        else if (!m_engineTimer.isActive()) m_engineTimer.start();
    });
    m_engine->updateTimeline(m_project->timeline());
    // Proxy fertig/gelöscht oder "Proxy-Medien verwenden" umgeschaltet -> Vorschau neu aufbauen
    ProxyManager* proxies = m_engine->proxies();
    auto proxiesChanged = [this] { m_engine->updateTimeline(m_project->timeline()); };
    connect(proxies, &ProxyManager::proxyChanged, this, [this, proxiesChanged](const QString& path) {
        if (TimelineOps::usesMediaOnVideo(m_project->timeline(), path)) proxiesChanged();
    });
    connect(proxies, &ProxyManager::enabledChanged, this, proxiesChanged);

    // Engine -> Playhead (nur im Timeline-Modus; im Quellmodus bleibt der Playhead stehen)
    TimelineView* tv = m_timeline->view();
    connect(m_engine, &Engine::positionChanged, this, [this, tv](int f) {
        if (m_engine->mode() == Engine::Mode::Timeline) tv->setPlayhead(f);
    });
    connect(tv, &TimelineView::seekRequested, this, [this](int f) {
        if (m_engine->mode() != Engine::Mode::Timeline) m_engine->showTimeline(f);
        else m_engine->seek(f);
    });
    // Keyframes: Inspector zeigt Werte am Playhead, Pfeile ◀ ▶ springen wie ein Klick ins Lineal
    connect(m_engine, &Engine::positionChanged, this, [this](int f) {
        if (m_engine->mode() == Engine::Mode::Timeline) m_inspector->setPlayhead(f);
    });
    connect(m_inspector, &Inspector::seekRequested, tv, &TimelineView::seekRequested);
    connect(m_mediaPool, &MediaPool::sourceRequested, this, &MainWindow::showSource);
    connect(tv, &TimelineView::dropRequested, this, &MainWindow::onDrop);
    // Rechtsklick auf Clips: die passenden Aktionen (Tastenkürzel wie im Menü)
    connect(tv, &TimelineView::clipMenuRequested, this, [this](const QPoint& pos) {
        QMenu menu(this);
        const bool audio = !m_editor->selectedAudioClips().isEmpty();
        for (const char* id : {"clip_speed", "clip_speed_reset", "normalize_audio", "", "toggle_enabled", "link_clips", "",
                               "delete", "ripple_delete"}) {
            if (!*id) {
                menu.addSeparator();
                continue;
            }
            QAction* a = InputBindings::instance().action(id);
            if (!a) continue;
            if (QString(id) == "normalize_audio" && !audio) continue;
            menu.addAction(a);
        }
        // Clipfarbe/Flags wie DaVinci: gelten für den Media-Pool-Clip (Titel haben keinen)
        QStringList paths;
        for (int cid : m_editor->selection()->ids())
            if (const Clip* c = TimelineOps::findClip(m_project->timeline(), cid); c && !c->isTitle()
                && !paths.contains(c->mediaPath))
                paths << c->mediaPath;
        if (!paths.isEmpty()) {
            menu.addSeparator();
            MediaPool::addClipColorMenu(&menu, m_project, paths);
            MediaPool::addFlagsMenu(&menu, m_project, paths);
        }
        menu.exec(pos);
    });
    // Quellbereich aus dem Viewer in die Timeline gezogen: dort überschreiben (wie DaVinci)
    connect(tv, &TimelineView::rangeDropRequested, this, [this](const QString& path, int in, int out, int frame, int track) {
        if (m_project->frameRateLocked()) {
            m_editor->placeSourceRange(path, in, out, frame, track);
            return;
        }
        QTimer::singleShot(0, this, [this, path, in, out, frame, track] { // Rückfrage erst nach dem Drop (s. onDrop)
            offerClipFormat({path}, true);
            m_editor->placeSourceRange(path, in, out, frame, track);
        });
    });
    // Viewer: Scrubber (Länge, In/Out) und Titel je nach Quelle/Timeline
    connect(m_engine, &Engine::modeChanged, this, &MainWindow::updateViewer);
    connect(m_project, &Project::timelineChanged, this, &MainWindow::updateViewer);
    connect(m_project, &Project::mediaMarksChanged, this, &MainWindow::updateViewer);
    connect(m_project, &Project::mediaChanged, this, &MainWindow::updateViewer);
    connect(m_engine, &Engine::positionChanged, this, [this](int f) {
        if (sourceActive()) m_sourcePos[m_engine->sourcePath()] = f;
    });
    updateViewer();
    // Effects Library: Doppelklick = wie Strg+T bzw. "Titel einfügen", nur mit der gewählten Art
    connect(m_effects, &EffectsLibrary::transitionRequested, this, [this, tv](TrackKind kind, const TransitionStyle& style) {
        m_editor->addTransitions(tv->playhead(), style, kind);
    });
    connect(m_effects, &EffectsLibrary::titleRequested, this, [this, tv] { m_editor->addTitle(tv->playhead()); });
    // Filter: auf die ausgewählten Videoclips, ohne Auswahl auf den obersten Clip am Playhead
    connect(m_effects, &EffectsLibrary::effectRequested, this, [this, tv](const QString& id) {
        m_editor->addEffect(m_editor->effectTargets(tv->playhead()), id);
    });
    tv->setProbe([this](const QString& path) { return probeCached(path); });
    m_mediaCache = new MediaCache(this);
    tv->setMediaCache(m_mediaCache);
    // Projekteinstellungen -> Engine (Profil neu), Cache, Inspector; kommt vor timelineChanged
    connect(m_project, &Project::formatChanged, this, &MainWindow::onFormatChanged);

    connect(m_project, &Project::modifiedChanged, this, &MainWindow::updateTitle);
    updateTitle();
    // Automatische Sicherung jede Minute (nur bei Änderungen), getrennt von der Projektdatei
    m_autosaveTimer = new QTimer(this);
    connect(m_autosaveTimer, &QTimer::timeout, this, &MainWindow::autosave);
    m_autosaveTimer->start(60 * 1000);

}

void MainWindow::importFiles(const QStringList& paths, bool placeOnTimeline, bool adoptFormat)
{
    m_mediaPool->importFiles(paths);
    if (!placeOnTimeline) return;
    // Testhilfe (--demo): Dateien hintereinander auf V1/A1 legen; Projektformat ohne Nachfrage vom ersten Clip
    if (adoptFormat) offerClipFormat(paths, false);
    m_editor->addMediaAt(paths, TimelineOps::endFrame(m_project->timeline()));
}

MediaInfo MainWindow::probeCached(const QString& path)
{
    auto it = m_probeCache.find(path);
    if (it == m_probeCache.end()) it = m_probeCache.insert(path, m_engine->probe(path));
    return *it;
}

void MainWindow::onDrop(const QStringList& paths, int frame, int track)
{
    if (paths == QStringList{MediaPool::TitleItem}) {
        m_editor->addTitle(frame, track);
        return;
    }
    // Direkt aus dem Dateimanager: erst in den Media Pool, dann auf die Timeline (wie DaVinci)
    QStringList unknown;
    for (const QString& p : paths)
        if (!m_project->mediaInfo(p)) unknown << p;
    if (!unknown.isEmpty()) m_mediaPool->importFiles(unknown);
    if (m_project->frameRateLocked()) {
        m_editor->addMediaAt(paths, frame, track);
        return;
    }
    // Leere Timeline: evtl. Rückfrage zum Projektformat – erst nach dem Drop (keine modale Abfrage mitten
    // im Drag & Drop, die Quelle, z. B. der Dateimanager, wartet sonst)
    QTimer::singleShot(0, this, [this, paths, frame, track] {
        offerClipFormat(paths, true);
        m_editor->addMediaAt(paths, frame, track);
    });
}

void MainWindow::offerClipFormat(const QStringList& paths, bool ask)
{
    if (m_project->frameRateLocked()) return; // nur in leerer Timeline
    // Der erste Clip mit Bild entscheidet (Standbilder und Ton haben kein eigenes Format)
    for (const QString& path : paths) {
        const MediaInfo* info = m_project->mediaInfo(path);
        if (!info || !info->hasVideo || info->isImage) continue;
        const ClipFormat cf = detectClipFormat(path);
        if (!cf.ok) return;
        const ProjectFormat cur = m_project->format();
        ProjectFormat want;
        want.width = cf.width;
        want.height = cf.height;
        want.rate = cf.suggested;
        if (want == cur) return;
        if (ask) {
            // Wie DaVinci "Change project frame rate?", hier samt Auflösung
            QString clipRate = cf.rate.label();
            if (cf.variable)
                clipRate = T("variabel, etwa %1").arg(QString::number(cf.averageFps, 'f', 2).replace('.', I18n::language() == "de" ? "," : "."));
            QMessageBox box(QMessageBox::Question, T("Projekteinstellungen"),
                            T("Das Format des Clips passt nicht zu den Projekteinstellungen. Projekt an den Clip anpassen?"),
                            QMessageBox::NoButton, this);
            box.setInformativeText(T("Clip: %1, %2 fps\nProjekt: %3, %4 fps\n\nNeu: %5, %6 fps")
                                       .arg(resolutionLabel(cf.width, cf.height), clipRate,
                                            resolutionLabel(cur.width, cur.height), cur.rate.label(),
                                            resolutionLabel(want.width, want.height), want.rate.label()));
            QPushButton* change = box.addButton(T("Ändern"), QMessageBox::AcceptRole);
            box.addButton(T("Nicht ändern"), QMessageBox::RejectRole);
            box.setDefaultButton(change);
            box.exec();
            if (box.clickedButton() != change) return;
        }
        m_project->setFormat(want);
        return;
    }
}

void MainWindow::showSource(const QString& path)
{
    const MediaInfo* m = m_project->mediaInfo(path);
    if (!m) return;
    m_engine->pause();
    m_engine->showSource(path, m_sourcePos.value(path, m->markIn >= 0 ? m->markIn : 0));
}

bool MainWindow::sourceActive() const
{
    return m_engine->mode() == Engine::Mode::Source && m_project->mediaInfo(m_engine->sourcePath());
}

QString MainWindow::editSource() const
{
    if (m_project->mediaInfo(m_engine->sourcePath())) return m_engine->sourcePath();
    const QStringList selected = m_mediaPool->selectedMedia(); // noch nichts in der Quellansicht
    return selected.isEmpty() ? QString() : selected.first();
}

void MainWindow::sourceEdit(Editor::SourceEditMode mode)
{
    const QString path = editSource();
    if (path.isEmpty()) return;
    offerClipFormat({path}, true); // erster Clip in leerer Timeline: Projektformat anpassen?
    TimelineView* tv = m_timeline->view();
    const int srcPos = sourceActive() ? m_engine->position() : m_sourcePos.value(path, 0);
    const int end = m_editor->sourceEdit(mode, path, srcPos, tv->playhead());
    if (end < 0) {
        QApplication::beep(); // kein Clip unter dem Playhead bzw. zu wenig Material
        return;
    }
    // Playhead ans Ende des neuen Clips (wie DaVinci); die Quellansicht bleibt offen
    if (m_engine->mode() == Engine::Mode::Timeline) {
        m_engine->pause();
        m_engine->seek(end);
    } else {
        tv->setPlayhead(end);
    }
}

void MainWindow::updateViewer()
{
    if (sourceActive()) {
        const MediaInfo* m = m_project->mediaInfo(m_engine->sourcePath());
        m_viewer->setSource(m->path, m->name);
        m_viewer->setRange(m->length, m->markIn, m->markOut);
        return;
    }
    const Timeline& tl = m_project->timeline();
    m_viewer->setSource({}, {});
    m_viewer->setRange(TimelineOps::endFrame(tl), tl.markIn, tl.markOut);
}

void MainWindow::setProjectFormat(const ProjectFormat& format)
{
    m_project->setFormat(format);
}

void MainWindow::grabStill()
{
    m_engine->pause();
    const QImage img = m_engine->grabStill(m_project->timeline());
    if (img.isNull()) {
        QMessageBox::warning(this, T("Standbild exportieren"), T("Das Bild konnte nicht erzeugt werden."));
        return;
    }
    // Vorschlag: Quellclip bzw. Projektname + Timecode (Doppelpunkte gehen nicht in Dateinamen)
    const QString base = m_engine->mode() == Engine::Mode::Source
        ? QFileInfo(m_engine->sourcePath()).completeBaseName()
        : (m_projectPath.isEmpty() ? T("Unbenannt") : QFileInfo(m_projectPath).completeBaseName());
    const QString tc = Timecode::format(m_engine->position(), m_project->fps()).replace(':', '-');
    QSettings settings;
    const QString dir = settings.value("still/dir", QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).toString();
    QString path = QFileDialog::getSaveFileName(this, T("Standbild exportieren"), QDir(dir).filePath(base + "_" + tc + ".png"),
                                                T("Bilder (*.png *.jpg *.tif)"));
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path += ".png";
    settings.setValue("still/dir", QFileInfo(path).absolutePath());
    if (!img.save(path, nullptr, 95))
        QMessageBox::warning(this, T("Standbild exportieren"), T("Datei konnte nicht gespeichert werden:\n%1").arg(path));
}

void MainWindow::projectSettingsDialog()
{
    ProjectSettingsDialog dlg(m_project->format(), m_project->frameRateLocked(), this);
    if (dlg.exec() == QDialog::Accepted) m_project->setFormat(dlg.format());
}

void MainWindow::clipSpeedDialog()
{
    // Vorgabe vom ersten ausgewählten Medienclip (Titel haben keine Geschwindigkeit)
    const Clip* first = nullptr;
    for (int id : m_editor->selection()->ids())
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id); c && !c->isTitle()
            && (!first || c->start < first->start))
            first = c;
    if (!first) return;
    const Editor::Retime cur{first->speed, first->reverse, first->freeze, first->keepPitch};
    ClipSpeedDialog dlg(cur, first->length(), m_project->format().rate.fps(), this);
    if (dlg.exec() == QDialog::Accepted)
        m_editor->setClipSpeed(m_editor->selection()->ids().values().toVector(), dlg.retime(), dlg.ripple());
}

void MainWindow::normalizeAudioDialog()
{
    const QVector<int> ids = m_editor->selectedAudioClips();
    if (ids.isEmpty()) {
        QApplication::beep(); // keine Audioclips ausgewählt
        return;
    }
    NormalizeDialog dlg(int(ids.size()), this);
    if (dlg.exec() != QDialog::Accepted) return;
    // Spitzenpegel messen (dekodiert den Ton der Clips; Originale, nie Proxies)
    QProgressDialog progress(T("Audiopegel werden gemessen…"), T("Abbrechen"), 0, 1000, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    QHash<int, double> peaks;
    const Timeline tl = m_project->timeline(); // Kopie: Zeiger bleiben gültig
    for (int i = 0; i < ids.size(); ++i) {
        const Clip* c = TimelineOps::findClip(tl, ids[i]);
        if (!c) continue;
        const auto peak = AudioAnalysis::clipPeakDb(m_project->format(), *c, [&](double f) {
            progress.setValue(int((i + f) / ids.size() * 1000));
            QApplication::processEvents();
            return !progress.wasCanceled();
        });
        if (progress.wasCanceled()) return;
        if (peak) peaks[c->id] = *peak;
    }
    progress.setValue(1000);
    m_editor->normalizeAudio(peaks, dlg.targetDb(), dlg.relative());
}

void MainWindow::onFormatChanged()
{
    const ProjectFormat& f = m_project->format();
    const FrameRate oldRate = m_engine->format().rate;
    m_engine->setFormat(f); // Timeline baut das folgende timelineChanged() neu auf
    m_mediaCache->setFormat(f);
    m_inspector->setFrameSize(f.size());
    m_probeCache.clear();
    if (f.rate == oldRate) return;
    // Längen im Media Pool zählen in Projekt-Frames -> neu einlesen (nur in leerer Timeline möglich,
    // Undo/Redo eingeschlossen). Offline-Medien umrechnen.
    QVector<MediaInfo> media = m_project->media();
    for (MediaInfo& m : media) {
        const MediaInfo fresh = QFileInfo::exists(m.path) ? m_engine->probe(m.path) : MediaInfo{};
        m.length = fresh.length > 0 ? fresh.length : int(std::lround(m.length * f.rate.fps() / oldRate.fps()));
        auto scaleMark = [&](int mark) {
            return mark < 0 ? -1 : std::min(m.length - 1, int(std::lround(mark * f.rate.fps() / oldRate.fps())));
        };
        m.markIn = scaleMark(m.markIn);
        m.markOut = scaleMark(m.markOut);
    }
    m_project->replaceMedia(media);
}

void MainWindow::buildLayout()
{
    m_mediaPool = new MediaPool(m_project, m_engine);
    m_effects = new EffectsLibrary;
    m_viewer = new Viewer(m_engine);
    m_inspector = new Inspector(m_editor);
    m_inspector->setFrameSize(m_project->format().size());
    m_timeline = new TimelinePanel(m_editor);
    m_deliver = new DeliverPanel(m_project);
    m_mixer = new Mixer(m_project, m_engine);
    m_mixer->hide();

    // Seiten; die gemeinsamen Panels (Pool, Viewer, Timeline) wandern beim Umschalten mit
    m_editTop = new QSplitter(Qt::Horizontal);
    m_editLeft = new QSplitter(Qt::Vertical);
    m_editLeft->addWidget(m_effects);
    m_editMain = new QSplitter(Qt::Vertical);
    m_editMain->addWidget(m_editTop);
    m_editBottom = new QSplitter(Qt::Horizontal); // Mixer rechts neben der Timeline wie in DaVinci
    m_editBottom->addWidget(m_mixer);
    m_editBottom->setStretchFactor(0, 1);
    m_editMain->addWidget(m_editBottom);
    m_mediaPage = new QSplitter(Qt::Horizontal);
    m_deliverPage = new QSplitter(Qt::Horizontal);
    m_deliverRight = new QSplitter(Qt::Vertical);
    m_deliverPage->addWidget(m_deliver);
    m_deliverPage->addWidget(m_deliverRight);

    m_pages = new QStackedWidget;
    m_pages->addWidget(m_mediaPage);
    m_pages->addWidget(m_editMain);
    m_pages->addWidget(m_deliverPage);

    auto* root = new QWidget;
    root->setObjectName("Root");
    auto* lay = new QVBoxLayout(root);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(buildTopBar());
    lay->addWidget(m_pages, 1);
    lay->addWidget(buildPageBar());
    setCentralWidget(root);

    showPage(Page::Edit);
}

QWidget* MainWindow::buildTopBar()
{
    // Obere Leiste wie in DaVinci: Panels links/rechts ein- und ausblenden, Projektname mittig
    // Sichtbarkeit wird gespeichert (panels/<key>); Testläufe ändern die Einstellung nicht
    auto makeToggle = [this](const QString& text, const QString& key, bool def, QWidget* panel) {
        auto* b = new QToolButton;
        b->setText(text);
        b->setCheckable(true);
        b->setChecked(QSettings().value("panels/" + key, def).toBool());
        // Noch nicht eingehängte Panels nur verstecken (show() ohne Eltern öffnete ein eigenes Fenster)
        if (panel->parentWidget() || !b->isChecked()) panel->setVisible(b->isChecked());
        connect(b, &QToolButton::toggled, this, [this, key, panel](bool on) {
            panel->setVisible(on);
            updateLeftColumn();
            if (!m_autosaveDisabled) QSettings().setValue("panels/" + key, on);
        });
        return b;
    };
    m_poolToggle = makeToggle("▦ Media Pool", "mediaPool", true, m_mediaPool);
    m_effectsToggle = makeToggle("✦ Effects", "effects", false, m_effects);
    m_inspectorToggle = makeToggle("☰ Inspector", "inspector", true, m_inspector);
    m_mixerToggle = makeToggle("▥ Mixer", "mixer", false, m_mixer);

    auto* title = m_titleLabel = new QLabel;
    title->setStyleSheet("font-weight: 600;");
    title->setAlignment(Qt::AlignCenter);

    auto* bar = new QWidget;
    bar->setObjectName("TopBar");
    bar->setStyleSheet("QWidget#TopBar { background: #2a2a30; border-bottom: 1px solid #141417; }");
    auto* lay = new QHBoxLayout(bar);
    lay->setContentsMargins(6, 2, 6, 2);
    lay->addWidget(m_poolToggle);
    lay->addWidget(m_effectsToggle);
    lay->addStretch(1);
    lay->addWidget(title);
    lay->addStretch(1);
    lay->addWidget(m_mixerToggle);
    lay->addWidget(m_inspectorToggle);
    return bar;
}

QWidget* MainWindow::buildPageBar()
{
    // Seitenleiste unten wie in DaVinci (Media | Edit | Deliver)
    auto* bar = new QWidget;
    bar->setObjectName("PageBar");
    bar->setStyleSheet(
        "QWidget#PageBar { background: #1f1f23; border-top: 1px solid #141417; }"
        "QToolButton { color: #8c8c94; padding: 4px 18px; border-radius: 0; }"
        "QToolButton:checked { color: #e87a3a; background: transparent; border-bottom: 2px solid #e87a3a; }"
        "QToolButton:hover { color: #d2d2d6; }");
    auto* lay = new QHBoxLayout(bar);
    lay->setContentsMargins(10, 0, 10, 0);
    lay->setSpacing(4);

    auto* brand = new QLabel("schneidi");
    brand->setStyleSheet("color: #8c8c94; font-weight: 600;");
    lay->addWidget(brand);
    lay->addStretch(1);

    m_pageButtons = new QButtonGroup(this);
    const struct { const char* icon; const char* name; Page page; } pages[] = {
        {"▤", "Media", Page::Media},
        {"✂", "Edit", Page::Edit},
        {"⇪", "Deliver", Page::Deliver},
    };
    for (const auto& pg : pages) {
        auto* b = new QToolButton;
        b->setText(QString("%1\n%2").arg(pg.icon, pg.name));
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        b->setCheckable(true);
        b->setChecked(pg.page == m_page);
        const Page page = pg.page;
        connect(b, &QToolButton::clicked, this, [this, page] { showPage(page); });
        m_pageButtons->addButton(b, int(pg.page));
        lay->addWidget(b);
    }
    lay->addStretch(1);
    lay->addSpacing(brand->sizeHint().width());
    return bar;
}

void MainWindow::showPage(Page page)
{
    m_page = page;
    switch (page) {
    case Page::Media:
        m_mediaPage->insertWidget(0, m_mediaPool);
        m_mediaPage->insertWidget(1, m_viewer);
        m_mediaPage->setSizes({900, 700});
        m_mediaPool->setVisible(true);
        m_pages->setCurrentWidget(m_mediaPage);
        break;
    case Page::Edit:
        m_editLeft->insertWidget(0, m_mediaPool);
        m_editTop->insertWidget(0, m_editLeft);
        m_editTop->insertWidget(1, m_viewer);
        m_editTop->insertWidget(2, m_inspector);
        m_editBottom->insertWidget(0, m_timeline);
        m_editBottom->setStretchFactor(0, 1);
        m_editBottom->setSizes({1300, 300});
        m_editTop->setStretchFactor(1, 1);
        m_editTop->setSizes({360, 880, 360});
        m_editMain->setSizes({480, 420});
        m_mediaPool->setVisible(m_poolToggle->isChecked());
        updateLeftColumn();
        m_pages->setCurrentWidget(m_editMain);
        break;
    case Page::Deliver:
        m_deliverRight->insertWidget(0, m_viewer);
        m_deliverRight->insertWidget(1, m_timeline);
        m_deliverRight->setSizes({480, 380});
        m_deliverPage->setStretchFactor(1, 1);
        m_deliverPage->setSizes({340, 1260});
        m_pages->setCurrentWidget(m_deliverPage);
        break;
    }
    if (auto* b = m_pageButtons->button(int(page))) b->setChecked(true);
    const bool edit = page == Page::Edit;
    m_poolToggle->setEnabled(edit);
    m_effectsToggle->setEnabled(edit);
    m_inspectorToggle->setEnabled(edit);
    m_mixerToggle->setEnabled(edit);
}

void MainWindow::updateLeftColumn()
{
    if (m_page == Page::Edit) m_editLeft->setVisible(m_poolToggle->isChecked() || m_effectsToggle->isChecked());
}

QAction* MainWindow::makeAction(QMenu* menu, const QString& id, const QString& text, const QKeySequence& key,
                               const std::function<void()>& fn)
{
    auto* a = new QAction(text, this);
    InputBindings::instance().registerAction(a, id, menu->title().remove('&'), key);
    a->setShortcutContext(Qt::WindowShortcut);
    connect(a, &QAction::triggered, this, fn);
    menu->addAction(a);
    return a;
}

void MainWindow::buildActions()
{
    TimelineView* tv = m_timeline->view();
    QUndoStack* undo = m_project->undoStack();

    // Belegung nach DaVinci Resolve (Defaults), überschreibbar in keybindings.json
    QMenu* file = menuBar()->addMenu(T("&Datei"));
    makeAction(file, "project_new", T("Neues Projekt"), QKeySequence("Ctrl+N"), [this] { newProject(); });
    makeAction(file, "project_open", T("Projekt öffnen…"), QKeySequence("Ctrl+O"), [this] { openProjectDialog(); });
    m_recentMenu = file->addMenu(T("Zuletzt geöffnet"));
    rebuildRecentMenu();
    makeAction(file, "project_save", T("Projekt speichern"), QKeySequence("Ctrl+S"), [this] { save(); });
    makeAction(file, "project_save_as", T("Projekt speichern unter…"), QKeySequence("Ctrl+Shift+S"), [this] { saveAs(); });
    file->addSeparator();
    makeAction(file, "import", T("Medien importieren…"), QKeySequence("Ctrl+I"), [this] { m_mediaPool->importDialog(); });
    makeAction(file, "new_bin", T("Neuer Bin"), QKeySequence("Ctrl+Shift+N"), [this] { m_mediaPool->newBin(); });
    file->addSeparator();
    makeAction(file, "project_settings", T("Projekteinstellungen…"), QKeySequence("Shift+9"), [this] { projectSettingsDialog(); });
    file->addSeparator();
    makeAction(file, "quit", T("Beenden"), QKeySequence("Ctrl+Q"), [this] { close(); });

    QMenu* edit = menuBar()->addMenu(T("&Bearbeiten"));
    QAction* undoAct = makeAction(edit, "undo", T("Rückgängig"), QKeySequence("Ctrl+Z"), [undo] { undo->undo(); });
    QAction* redoAct = makeAction(edit, "redo", T("Wiederholen"), QKeySequence("Ctrl+Shift+Z"), [undo] { undo->redo(); });
    connect(undo, &QUndoStack::canUndoChanged, undoAct, &QAction::setEnabled);
    connect(undo, &QUndoStack::canRedoChanged, redoAct, &QAction::setEnabled);
    undoAct->setEnabled(false);
    redoAct->setEnabled(false);
    edit->addSeparator();
    makeAction(edit, "cut", T("Ausschneiden"), QKeySequence("Ctrl+X"), [this] { m_editor->cutSelection(); });
    makeAction(edit, "copy", T("Kopieren"), QKeySequence("Ctrl+C"), [this] { m_editor->copySelection(); });
    makeAction(edit, "paste", T("Einfügen am Playhead"), QKeySequence("Ctrl+V"), [this, tv] { m_editor->paste(tv->playhead()); });
    edit->addSeparator();
    makeAction(edit, "delete", T("Löschen (Lücke bleibt)"), QKeySequence(Qt::Key_Backspace), [this] { m_editor->deleteSelection(); });
    makeAction(edit, "delete_alt", T("Löschen (Entf)"), QKeySequence(Qt::Key_Delete), [this] { m_editor->deleteSelection(); });
    makeAction(edit, "ripple_delete", T("Löschen mit Ripple"), QKeySequence("Shift+Del"), [this] { m_editor->rippleDeleteSelection(); });
    makeAction(edit, "ripple_delete_alt", T("Löschen mit Ripple (Rücktaste)"), QKeySequence("Shift+Backspace"),
               [this] { m_editor->rippleDeleteSelection(); });
    edit->addSeparator();
    // Bearbeitungen aus dem Quell-Viewer wie DaVinci (Edit → Insert/Overwrite/Replace/Place on Top …)
    using SE = Editor::SourceEditMode;
    makeAction(edit, "insert_clip", T("Clip einfügen"), QKeySequence(Qt::Key_F9), [this] { sourceEdit(SE::Insert); });
    makeAction(edit, "overwrite_clip", T("Clip überschreiben"), QKeySequence(Qt::Key_F10), [this] { sourceEdit(SE::Overwrite); });
    makeAction(edit, "replace_clip", T("Clip ersetzen"), QKeySequence(Qt::Key_F11), [this] { sourceEdit(SE::Replace); });
    makeAction(edit, "place_on_top", T("Oben platzieren"), QKeySequence(Qt::Key_F12), [this] { sourceEdit(SE::PlaceOnTop); });
    makeAction(edit, "ripple_overwrite", T("Ripple-Überschreiben"), QKeySequence("Shift+F10"),
               [this] { sourceEdit(SE::RippleOverwrite); });
    makeAction(edit, "append_at_end", T("Am Ende der Timeline anhängen"), QKeySequence("Shift+F12"),
               [this] { sourceEdit(SE::AppendAtEnd); });
    edit->addSeparator();
    makeAction(edit, "select_all", T("Alles auswählen"), QKeySequence("Ctrl+A"), [this] { m_editor->selectAll(); });
    makeAction(edit, "deselect", T("Auswahl aufheben"), QKeySequence("Ctrl+Shift+A"), [this] { m_selection->clear(); });
    edit->addSeparator();
    makeAction(edit, "edit_keybindings", T("Tastenbelegung…"), QKeySequence("Ctrl+Alt+K"), [this] {
        KeyBindingsDialog dlg(this);
        dlg.exec();
    });

    QMenu* timeline = menuBar()->addMenu("&Timeline");
    makeAction(timeline, "tool_select", T("Auswahl-Werkzeug"), QKeySequence("A"), [tv] { tv->setTool(TimelineView::Tool::Select); });
    makeAction(timeline, "tool_trim", T("Trim-Modus"), QKeySequence("T"), [tv] { tv->setTool(TimelineView::Tool::Trim); });
    makeAction(timeline, "tool_blade", T("Klingen-Werkzeug"), QKeySequence("B"), [tv] { tv->setTool(TimelineView::Tool::Blade); });
    makeAction(timeline, "snapping", T("Snapping an/aus"), QKeySequence("N"), [tv] { tv->setSnapping(!tv->snapping()); });
    timeline->addSeparator();
    makeAction(timeline, "split", T("Clip am Playhead teilen"), QKeySequence("Ctrl+B"),
               [this, tv] { m_editor->splitAtPlayhead(tv->playhead()); });
    makeAction(timeline, "split_alt", T("Clip teilen (alternativ)"), QKeySequence("Ctrl+\\"),
               [this, tv] { m_editor->splitAtPlayhead(tv->playhead()); });
    makeAction(timeline, "add_transition", T("Übergang hinzufügen (Cross Dissolve)"), QKeySequence("Ctrl+T"),
               [this, tv] { m_editor->addTransitions(tv->playhead()); });
    makeAction(timeline, "add_title", T("Titel einfügen"), QKeySequence(), [this, tv] { m_editor->addTitle(tv->playhead()); });
    makeAction(timeline, "trim_start", T("Anfang bis Playhead trimmen"), QKeySequence("Shift+["),
               [this, tv] { m_editor->trimToPlayhead(TimelineOps::Edge::Start, tv->playhead()); });
    makeAction(timeline, "trim_end", T("Ende bis Playhead trimmen"), QKeySequence("Shift+]"),
               [this, tv] { m_editor->trimToPlayhead(TimelineOps::Edge::End, tv->playhead()); });
    // Trim-Modus: , und . slippen die Auswahl (wie DaVinci), sonst verschieben
    auto nudge = [this, tv](int frames) {
        if (tv->tool() == TimelineView::Tool::Trim) m_editor->slipSelection(frames);
        else m_editor->nudgeSelection(frames);
    };
    makeAction(timeline, "nudge_left", T("1 Frame nach links schieben"), QKeySequence(","), [nudge] { nudge(-1); });
    makeAction(timeline, "nudge_right", T("1 Frame nach rechts schieben"), QKeySequence("."), [nudge] { nudge(1); });
    makeAction(timeline, "nudge_left_multi", T("5 Frames nach links schieben"), QKeySequence("Shift+,"),
               [nudge] { nudge(-5); });
    makeAction(timeline, "nudge_right_multi", T("5 Frames nach rechts schieben"), QKeySequence("Shift+."),
               [nudge] { nudge(5); });
    makeAction(timeline, "toggle_enabled", T("Clip aktivieren/deaktivieren"), QKeySequence("D"),
               [this] { m_editor->toggleSelectionEnabled(); });
    makeAction(timeline, "clip_speed", T("Clip-Geschwindigkeit ändern…"), QKeySequence("Ctrl+R"), [this] { clipSpeedDialog(); });
    makeAction(timeline, "clip_speed_reset", T("Geschwindigkeit zurücksetzen"), QKeySequence("Ctrl+Alt+R"), [this] {
        m_editor->setClipSpeed(m_editor->selection()->ids().values().toVector(), {}, true);
    });
    makeAction(timeline, "normalize_audio", T("Audiopegel normalisieren…"), QKeySequence(), [this] { normalizeAudioDialog(); });
    makeAction(timeline, "link_clips", T("Clips verknüpfen/trennen"), QKeySequence("Ctrl+Alt+L"),
               [this] { m_editor->toggleLinkSelection(); });
    timeline->addSeparator();
    makeAction(timeline, "add_marker", T("Marker setzen/entfernen"), QKeySequence("M"),
               [this, tv] { m_editor->toggleMarker(tv->playhead()); });
    // In/Out wie DaVinci: zeigt der Viewer die Quelle, gelten sie für den Quellclip (pro Media-Pool-Clip gemerkt)
    auto markIn = [this, tv](int frame) {
        if (sourceActive()) m_editor->setSourceMarkIn(m_engine->sourcePath(), frame < 0 ? -1 : m_engine->position());
        else m_editor->setMarkIn(frame < 0 ? -1 : tv->playhead());
    };
    auto markOut = [this, tv](int frame) {
        if (sourceActive()) m_editor->setSourceMarkOut(m_engine->sourcePath(), frame < 0 ? -1 : m_engine->position());
        else m_editor->setMarkOut(frame < 0 ? -1 : tv->playhead());
    };
    auto gotoMark = [this](bool in) {
        if (sourceActive()) {
            const MediaInfo* m = m_project->mediaInfo(m_engine->sourcePath());
            const int f = in ? m->markIn : m->markOut;
            if (f < 0) return;
            m_engine->pause();
            m_engine->seek(f);
            return;
        }
        jumpToFrame(in ? m_project->timeline().markIn : m_project->timeline().markOut);
    };
    makeAction(timeline, "mark_in", T("In-Punkt setzen"), QKeySequence("I"), [markIn] { markIn(0); });
    makeAction(timeline, "mark_out", T("Out-Punkt setzen"), QKeySequence("O"), [markOut] { markOut(0); });
    makeAction(timeline, "clear_in", T("In-Punkt entfernen"), QKeySequence("Alt+I"), [markIn] { markIn(-1); });
    makeAction(timeline, "clear_out", T("Out-Punkt entfernen"), QKeySequence("Alt+O"), [markOut] { markOut(-1); });
    makeAction(timeline, "clear_in_out", T("In und Out entfernen"), QKeySequence("Alt+X"), [this] {
        if (sourceActive()) m_editor->clearSourceMarks(m_engine->sourcePath());
        else m_editor->clearMarks();
    });
    makeAction(timeline, "goto_in", T("Zum In-Punkt"), QKeySequence("Shift+I"), [gotoMark] { gotoMark(true); });
    makeAction(timeline, "goto_out", T("Zum Out-Punkt"), QKeySequence("Shift+O"), [gotoMark] { gotoMark(false); });
    makeAction(timeline, "marker_prev", T("Vorheriger Marker"), QKeySequence("Shift+Up"), [this] { jumpToMarker(-1); });
    makeAction(timeline, "marker_next", T("Nächster Marker"), QKeySequence("Shift+Down"), [this] { jumpToMarker(1); });
    auto* linked = makeAction(timeline, "linked_selection", T("Verknüpfte Auswahl"), QKeySequence("Ctrl+Shift+L"), [] {});
    linked->setCheckable(true);
    linked->setToolTip(T("Verknüpfte Auswahl (Strg+Shift+L): an = Video und Ton eines Clips bewegen sich zusammen, "
                         "aus = einzeln auswählen, verschieben und trimmen"));
    linked->setChecked(QSettings().value("edit/linkedSelection", true).toBool());
    m_editor->setLinkedSelection(linked->isChecked());
    connect(linked, &QAction::toggled, this, [this](bool on) {
        m_editor->setLinkedSelection(on);
        if (!m_autosaveDisabled) QSettings().setValue("edit/linkedSelection", on);
    });
    m_timeline->addToolAction(linked, TimelinePanel::Icon::Link);
    auto* splitTracks = makeAction(timeline, "split_on_tracks", T("Teilen auf ganzer Spur der Auswahl"), QKeySequence(), [] {});
    splitTracks->setCheckable(true);
    splitTracks->setToolTip(T("An: Strg+B/Maustaste teilt auch den Nachbarclip auf der Spur des ausgewählten Clips. "
                            "Aus: nur ausgewählte Clips."));
    splitTracks->setChecked(QSettings().value("edit/splitOnSelectedTracks", true).toBool());
    m_editor->setSplitOnSelectedTracks(splitTracks->isChecked());
    connect(splitTracks, &QAction::toggled, this, [this](bool on) {
        m_editor->setSplitOnSelectedTracks(on);
        QSettings().setValue("edit/splitOnSelectedTracks", on);
    });
    timeline->addSeparator();
    makeAction(timeline, "zoom_in", T("Hineinzoomen"), QKeySequence("Ctrl+="), [tv] { tv->zoomBy(1.5); });
    makeAction(timeline, "zoom_out", T("Herauszoomen"), QKeySequence("Ctrl+-"), [tv] { tv->zoomBy(1 / 1.5); });
    makeAction(timeline, "zoom_fit", T("Ganze Timeline zeigen"), QKeySequence("Shift+Z"), [tv] { tv->zoomToFit(); });

    QMenu* play = menuBar()->addMenu(T("&Wiedergabe"));
    makeAction(play, "play_pause", "Play/Pause", QKeySequence(Qt::Key_Space), [this] { m_engine->togglePlay(); });
    makeAction(play, "shuttle_back", T("Rückwärts (J)"), QKeySequence("J"), [this] { shuttle(-1); });
    makeAction(play, "stop", T("Stopp (K)"), QKeySequence("K"), [this] { m_engine->pause(); });
    makeAction(play, "shuttle_fwd", T("Vorwärts (L)"), QKeySequence("L"), [this] { shuttle(1); });
    play->addSeparator();
    makeAction(play, "frame_prev", T("1 Frame zurück"), QKeySequence(Qt::Key_Left), [this] { stepFrames(-1); });
    makeAction(play, "frame_next", T("1 Frame vor"), QKeySequence(Qt::Key_Right), [this] { stepFrames(1); });
    makeAction(play, "second_prev", T("1 Sekunde zurück"), QKeySequence("Shift+Left"), [this] { stepFrames(-m_project->fps()); });
    makeAction(play, "second_next", T("1 Sekunde vor"), QKeySequence("Shift+Right"), [this] { stepFrames(m_project->fps()); });
    makeAction(play, "edit_prev", T("Vorheriger Schnittpunkt"), QKeySequence(Qt::Key_Up), [this] { jumpToEdit(-1); });
    makeAction(play, "edit_next", T("Nächster Schnittpunkt"), QKeySequence(Qt::Key_Down), [this] { jumpToEdit(1); });
    makeAction(play, "go_start", T("Zum Anfang"), QKeySequence(Qt::Key_Home), [this] { m_engine->pause(); m_engine->seek(0); });
    makeAction(play, "go_end", T("Zum Ende"), QKeySequence(Qt::Key_End), [this] {
        m_engine->pause();
        m_engine->seek(TimelineOps::endFrame(m_project->timeline()));
    });
    makeAction(play, "show_timeline", T("Timeline im Viewer zeigen"), QKeySequence("Q"),
              [this, tv] { m_engine->showTimeline(tv->playhead()); });
    // Wie DaVinci „Grab Still“ (Strg+Alt+G), hier direkt als PNG speichern
    auto* grab = makeAction(play, "grab_still", T("Standbild exportieren…"), QKeySequence("Ctrl+Alt+G"), [this] { grabStill(); });
    m_viewer->addAction(grab);
    m_viewer->setContextMenuPolicy(Qt::ActionsContextMenu);
    play->addSeparator();
    // Wie DaVinci (Playback → Use Proxy Media if Available); Export nutzt immer die Originale
    auto* useProxy = makeAction(play, "use_proxy", T("Proxy-Medien verwenden, falls vorhanden"), QKeySequence(), [] {});
    useProxy->setCheckable(true);
    useProxy->setChecked(m_engine->proxies()->enabled());
    connect(useProxy, &QAction::toggled, m_engine->proxies(), &ProxyManager::setEnabled);

    QMenu* workspace = menuBar()->addMenu(T("&Arbeitsbereich"));
    makeAction(workspace, "page_media", T("Media-Seite"), QKeySequence("Shift+2"), [this] { showPage(Page::Media); });
    makeAction(workspace, "page_edit", T("Edit-Seite"), QKeySequence("Shift+4"), [this] { showPage(Page::Edit); });
    makeAction(workspace, "page_deliver", T("Deliver-Seite"), QKeySequence("Shift+8"), [this] { showPage(Page::Deliver); });
    makeAction(workspace, "toggle_effects", T("Effects Library ein/aus"), QKeySequence(),
               [this] { if (m_effectsToggle->isEnabled()) m_effectsToggle->toggle(); });
    makeAction(workspace, "toggle_mixer", T("Mixer ein/aus"), QKeySequence(),
               [this] { if (m_mixerToggle->isEnabled()) m_mixerToggle->toggle(); });
    workspace->addSeparator();
    // Sprache wie in DaVinci (Preferences → User → UI Settings): wirkt nach dem Neustart
    QMenu* langMenu = workspace->addMenu(T("Sprache"));
    auto* langGroup = new QActionGroup(langMenu);
    for (const auto& [code, name] : {std::pair{"de", "Deutsch"}, std::pair{"en", "English"}}) {
        QAction* a = langMenu->addAction(name);
        a->setCheckable(true);
        a->setChecked(I18n::language() == code);
        langGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, lang = QString(code)] {
            if (lang == I18n::language()) return;
            I18n::setLanguage(lang);
            QMessageBox::information(this, "schneidi", lang == "en" ? "The language changes after restarting schneidi."
                                                                    : "Die Sprache ändert sich nach dem Neustart von schneidi.");
        });
    }

    InputBindings::instance().saveIfIncomplete();
}

// ---- Projektdatei ---------------------------------------------------------------

namespace {
const QString kFileFilter = T("schneidi-Projekt (*.%1)").arg(ProjectFile::Extension);
}

QString MainWindow::autosavePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dir).filePath(QString("autosave.%1").arg(ProjectFile::Extension));
}

void MainWindow::updateTitle()
{
    const QString name = m_projectPath.isEmpty() ? T("Unbenannt") : QFileInfo(m_projectPath).completeBaseName();
    const QString shown = name + (m_project->isModified() ? " *" : "");
    setWindowTitle(shown + " – schneidi");
    if (m_titleLabel) m_titleLabel->setText(shown);
}

void MainWindow::setProjectPath(const QString& path)
{
    m_projectPath = path;
    if (!path.isEmpty() && !m_autosaveDisabled) addRecent(path); // Testlauf: Liste des Nutzers nicht anfassen
    updateTitle();
}

bool MainWindow::maybeSave()
{
    if (!m_project->isModified() || m_autosaveDisabled) return true; // Testlauf: nie nachfragen
    const auto answer = QMessageBox::question(this, "schneidi", T("Änderungen am Projekt speichern?"),
                                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                              QMessageBox::Save);
    if (answer == QMessageBox::Save) return save();
    return answer == QMessageBox::Discard;
}

void MainWindow::newProject()
{
    if (!maybeSave()) return;
    m_engine->pause();
    m_selection->clear();
    m_engine->setFormat(ProjectFormat{}); // Medienlängen passen schon (leer) -> nicht neu einlesen
    m_project->reset();
    m_sourcePos.clear();
    m_engine->showTimeline(0);
    setProjectPath({});
    removeAutosave();
}

void MainWindow::openProjectDialog()
{
    if (!maybeSave()) return;
    const QString dir = m_projectPath.isEmpty() ? QDir::homePath() : QFileInfo(m_projectPath).absolutePath();
    const QString path = QFileDialog::getOpenFileName(this, T("Projekt öffnen"), dir, kFileFilter);
    if (!path.isEmpty()) openProject(path);
}

bool MainWindow::openProject(const QString& path)
{
    ProjectData data;
    QString error;
    if (!ProjectFile::load(path, &data, &error)) {
        QMessageBox::warning(this, T("Projekt öffnen"), T("%1 konnte nicht geöffnet werden:\n%2").arg(path, error));
        return false;
    }
    if (!applyLoaded(std::move(data), path)) return false;
    removeAutosave();
    return true;
}

bool MainWindow::applyLoaded(ProjectData data, const QString& path)
{
    // Fehlende Medien wie DaVinci "Media Offline": Ordner durchsuchen lassen oder offline lassen
    for (QStringList missing = ProjectFile::missingMedia(data); !missing.isEmpty();
         missing = ProjectFile::missingMedia(data)) {
        QMessageBox box(QMessageBox::Warning, T("Medien fehlen"),
                        T("%1 Datei(en) nicht gefunden (verschoben oder umbenannt?):").arg(missing.size()),
                        QMessageBox::NoButton, this);
        QStringList names;
        for (const QString& p : missing.mid(0, 15)) names << p;
        if (missing.size() > 15) names << "…";
        box.setInformativeText(names.join('\n'));
        QPushButton* search = box.addButton(T("Ordner durchsuchen…"), QMessageBox::AcceptRole);
        box.addButton(T("Offline lassen"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() != search) break;
        const QString dir = QFileDialog::getExistingDirectory(this, T("Ordner mit den Medien wählen"),
                                                              QFileInfo(path).absolutePath());
        if (dir.isEmpty()) break;
        if (ProjectFile::relink(&data, dir) == 0)
            QMessageBox::information(this, T("Medien fehlen"), T("In diesem Ordner wurden keine der Dateien gefunden."));
    }

    m_engine->pause();
    m_selection->clear();
    m_probeCache.clear();
    m_engine->setFormat(data.format); // Medienlängen in der Datei zählen schon in dieser Framerate
    m_project->load(data);
    m_sourcePos.clear();
    m_engine->showTimeline(data.playhead);
    m_timeline->view()->setPlayhead(data.playhead);
    setProjectPath(path);
    return true;
}

bool MainWindow::save()
{
    return m_projectPath.isEmpty() ? saveAs() : saveTo(m_projectPath);
}

bool MainWindow::saveAs()
{
    QString path = QFileDialog::getSaveFileName(
        this, T("Projekt speichern unter"),
        m_projectPath.isEmpty() ? QDir::home().filePath(T("Unbenannt.%1").arg(ProjectFile::Extension)) : m_projectPath,
        kFileFilter);
    if (path.isEmpty()) return false;
    if (QFileInfo(path).suffix() != ProjectFile::Extension) path += QString(".%1").arg(ProjectFile::Extension);
    return saveTo(path);
}

bool MainWindow::saveTo(const QString& path)
{
    ProjectData data = m_project->data();
    data.playhead = m_timeline->view()->playhead();
    QString error;
    if (!ProjectFile::save(data, path, &error)) {
        QMessageBox::warning(this, T("Projekt speichern"), T("Speichern fehlgeschlagen:\n%1").arg(error));
        return false;
    }
    m_project->markSaved();
    setProjectPath(path);
    removeAutosave();
    return true;
}

void MainWindow::autosave()
{
    if (!m_project->isModified()) return;
    ProjectData data = m_project->data();
    data.playhead = m_timeline->view()->playhead();
    QDir().mkpath(QFileInfo(autosavePath()).absolutePath());
    if (ProjectFile::save(data, autosavePath(), nullptr))
        QSettings().setValue("autosave/projectPath", m_projectPath); // wohin die Sicherung gehört
}

void MainWindow::disableAutosave()
{
    m_autosaveTimer->stop();
    m_autosaveDisabled = true;
    m_mediaPool->setSaveSettings(false); // Testläufe: Media-Pool-Ansicht nicht speichern
}

void MainWindow::removeAutosave()
{
    if (m_autosaveDisabled) return; // Testlauf: echte Sicherung des Nutzers nicht anfassen

    QFile::remove(autosavePath());
    QSettings().remove("autosave/projectPath");
}

void MainWindow::offerAutosaveRestore()
{
    if (!QFileInfo::exists(autosavePath())) return;
    const QString original = QSettings().value("autosave/projectPath").toString();
    const QString name = original.isEmpty() ? T("Unbenannt") : QFileInfo(original).fileName();
    const auto answer = QMessageBox::question(
        this, T("Wiederherstellen"),
        T("schneidi wurde nicht normal beendet.\nNicht gespeicherte Änderungen von „%1“ wiederherstellen?")
            .arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer != QMessageBox::Yes) {
        removeAutosave();
        return;
    }
    ProjectData data;
    if (!ProjectFile::load(autosavePath(), &data, nullptr)) return;
    applyLoaded(std::move(data), original);
    m_project->markModified(); // wiederhergestellt, aber noch nicht in die Projektdatei gespeichert
}

void MainWindow::addRecent(const QString& path)
{
    QSettings settings;
    QStringList recent = settings.value("recentProjects").toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    settings.setValue("recentProjects", recent.mid(0, 10));
    rebuildRecentMenu();
}

void MainWindow::rebuildRecentMenu()
{
    if (!m_recentMenu) return;
    m_recentMenu->clear();
    const QStringList recent = QSettings().value("recentProjects").toStringList();
    for (const QString& path : recent) {
        QAction* a = m_recentMenu->addAction(QFileInfo(path).fileName(), this, [this, path] {
            if (maybeSave()) openProject(path);
        });
        a->setToolTip(path);
        a->setEnabled(QFileInfo::exists(path));
    }
    m_recentMenu->setEnabled(!recent.isEmpty());
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (!maybeSave()) {
        event->ignore();
        return;
    }
    removeAutosave(); // normal beendet -> nichts wiederherzustellen
    event->accept();
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    const QEvent::Type t = event->type();
    if (t != QEvent::MouseButtonPress && t != QEvent::MouseButtonRelease && t != QEvent::MouseButtonDblClick)
        return QMainWindow::eventFilter(obj, event);
    // Nur im Hauptfenster, nicht in Dialogen (dort wird z. B. die Belegung gewählt)
    auto* w = qobject_cast<QWidget*>(obj);
    if (!w || w->window() != this || QApplication::activeModalWidget())
        return QMainWindow::eventFilter(obj, event);
    const auto* me = static_cast<QMouseEvent*>(event);
    const QString id = InputBindings::instance().mouseAction(me->button());
    if (id.isEmpty()) return QMainWindow::eventFilter(obj, event);
    if (t == QEvent::MouseButtonPress)
        if (QAction* a = InputBindings::instance().action(id); a && a->isEnabled()) a->trigger();
    return true; // Loslassen/Doppelklick der belegten Taste ebenfalls schlucken
}

void MainWindow::shuttle(int direction)
{
    // J/L mehrfach drücken = schneller, wie in DaVinci
    const double s = m_engine->speed();
    if (direction > 0) m_engine->setSpeed(s <= 0 ? 1.0 : std::min(s * 2, 16.0));
    else m_engine->setSpeed(s >= 0 ? -1.0 : std::max(s * 2, -16.0));
}

void MainWindow::stepFrames(int frames)
{
    m_engine->pause();
    m_engine->seek(m_engine->position() + frames);
}

void MainWindow::jumpToEdit(int direction)
{
    QVector<int> points;
    const Timeline& tl = m_project->timeline();
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const auto& t : tl.tracks(k))
            for (const auto& c : t.clips) points << c.start << c.end();
    jumpTo(points, direction);
}

void MainWindow::jumpToMarker(int direction)
{
    jumpTo(m_project->timeline().markers, direction);
}

void MainWindow::jumpTo(const QVector<int>& points, int direction)
{
    const int pos = m_timeline->view()->playhead();
    int best = direction > 0 ? INT_MAX : -1;
    for (int e : points) {
        if (direction > 0 && e > pos) best = std::min(best, e);
        if (direction < 0 && e < pos) best = std::max(best, e);
    }
    if (best == INT_MAX || best < 0) return;
    jumpToFrame(best);
}

void MainWindow::jumpToFrame(int frame)
{
    if (frame < 0) return;
    m_engine->pause();
    if (m_engine->mode() != Engine::Mode::Timeline) m_engine->showTimeline(frame);
    else m_engine->seek(frame);
}
