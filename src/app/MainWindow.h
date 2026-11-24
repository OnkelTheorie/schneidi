// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "engine/ProxyManager.h"
#include "app/AiSettings.h"
#include "app/DirectBackend.h"
#include "app/PageVisibility.h"
#include "core/Editor.h"
#include "core/LiveHost.h"
#include "core/PlaybackMeter.h"
#include "core/ProjectFile.h"
#include "core/Subtitles.h"
#include "core/Types.h"

#include <QElapsedTimer>
#include "engine/PreviewResolution.h"
#include "engine/ProbeCache.h"
#include "ui/SecondScreen.h"
#include "ui/timeline/PlayheadMove.h"
#include "ui/timeline/ViewState.h"

#include <QHash>
#include <QImage>
#include <QLockFile>
#include <QMainWindow>
#include <QMargins>
#include <QPointF>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <functional>
#include <memory>
#include <optional>

class Engine;
class JklKeys;
class Project;
class Selection;
class MediaPool;
class EffectsLibrary;
class MediaStorage;
class Viewer;
class Inspector;
class ColorPanel;
class ShapeOverlay;
class TranscriptPanel;
class ScopesPanel;
class Mixer;
class TimelinePanel;
class QMenu;
class QSplitter;
struct Tour;
class QStackedWidget;
class QButtonGroup;
class QToolButton;
class DeliverPanel;
struct StartTemplate;
struct ContentTemplate;

namespace Ai {
struct Endpoint;
class Keys;
} // namespace Ai

namespace Relink {
struct Root;
struct Progress;
} // namespace Relink

class MainWindow : public QMainWindow, public LiveHost {
    Q_OBJECT
public:
    explicit MainWindow(Engine* engine, QWidget* parent = nullptr);
    ~MainWindow() override;
    // placeOnTimeline (test helper --demo): one after another on V1/A1; adoptFormat = project format from the first clip
    void importFiles(const QStringList& paths, bool placeOnTimeline = false, bool adoptFormat = true);
    // Opens the graphics card's video decoder in the background (Video::Hardware::warmUp): there is video to show
    static void warmUpVideoDecoding();
    // Workspace › Brand Kit…: the dialog; the result is saved and applied to the open project (one undo step)
    void editBrandKit();
    // Timeline of another editor (FCP7 XML, OTIO, EDL) as a new timeline; false = not readable (message shown)
    bool importTimelineFile(const QString& path);

    // Playback › Generate Proxy Media and the timeline clip menu: the video files meant (core/ProxyChoice; "selected":
    // the timeline's selected clips, without any the Media Pool's selected files) and queueing the missing ones
    // (returns how many were queued)
    QStringList proxyFiles(int scope) const; // ProxyChoice::Scope
    int generateProxies(int scope);

    // Pages like in DaVinci (switchable at the bottom). New page: add here + in showPage().
    // Page::Direct comes last: the page buttons' ids are int(page) (the bar order is PageVisibility's)
    enum class Page { Media, Edit, Deliver, Color, Comp, Studio, Direct };
    // Every page in bar order, and the page of a stable id (nullopt = none)
    static const QVector<Page>& allPages();
    static std::optional<Page> pageForId(const QString& id);
    void showPage(Page page);
    // A voice-over take a crash left unfinished: into the Media Pool? (once, not modal; also at the start)
    void offerCrashedTake();
    Page currentPage() const { return m_page; }
    // Hiding pages (right-click on the page bar, Workspace › Visible Pages; PageVisibility.h): a hidden current page
    // switches to the nearest shown one; the last shown page cannot be hidden. Saved right away.
    void setPageHidden(Page page, bool hidden);
    bool isPageHidden(Page page) const;
    void showAllPages();
    // The menu: one checkbox per page + "Show all pages" (cleared first)
    void fillPagesMenu(QMenu* menu);
    static QString pageId(Page page); // stable id: settings, objectName pageButton_<id>
    static QString pageTitle(Page page); // its name in the UI language
    // The start page (Settings › General › Interface, PageVisibility::start): shown at the start and on New Project,
    // not when a project is opened; a hidden one gives the nearest shown page
    QString startPage() const { return m_pageVisibility.start(); }
    void setStartPage(const QString& id); // saved unless this is a test run
    void showStartPage();
    // Comp page (MainWindowComp.cpp): the clip's compositing graph; built on its first showing. openComp shows clip
    // `clipId` there; "Open in Comp" gives the first selected video clip an empty graph (clip -> output) if it has
    // none; "Comp from Selection" = Editor::compFrom, then opens it
    void openComp(int clipId);
    // The video clip the Comp page works on at timeline frame `frame`: a selected one under it, else the topmost shown
    // one there (0 = none, a gap)
    int compClipAt(int frame) const;
    // Comp and Color pages work on what the viewer shows: a selected clip the playhead is not in brings the playhead
    // to its start (nothing when it is already inside)
    void playheadInto(int clipId);
    void openCompForSelection();
    void compFromSelection();
    class CompPage* compPage() const { return m_compPage; } // nullptr until the page was shown
    // Studio page (0.23, MainWindowStudio.cpp): sound - tracks with waveforms and the volume pen, the big mixer; built
    // on its first showing
    class StudioPage* studioPage() const { return m_studioPage; } // nullptr until the page was shown
    // Direct page "Regie" (0.25, MainWindowDirect.cpp): material in, a recipe, suggestions to check, a timeline out;
    // built on its first showing. The work behind it is a DirectBackend (app/DirectBackend.h): main.cpp hands in the
    // one that runs the `direct` command; without one the page uses the fake (FakeDirectBackend).
    class DirectPage* directPage() const { return m_directPage; } // nullptr until the page was shown
    void setDirectBackend(std::unique_ptr<class DirectBackend> backend);
    class DirectBackend* directBackend();
    // In-app tours (MainWindowTours.cpp, app/TourManager.h): Help › Tours; offerTours = the one question at the end of
    // the start (main.cpp, never in test runs); pageName = a page's name in the tour files ("media", "edit", ...)
    void offerTours();
    // A larger interface for a screen with many pixels at 100 % (app/UiScale.h): a bar under the top bar, "Larger
    // (factor)" / "Leave it"; either answer is kept, a yes takes effect after a restart (main.cpp, never in test runs)
    void offerUiScale(double factor);
    class TourManager* tours() const { return m_tours; }
    // A tour asks or runs, or an AI drives the app: no unasked questions (the Claude offer) now
    bool busyForQuestions() const;
    static QString pageName(Page page);

    // Project file (.schneidi): only references to the original media + edit
    bool openProject(const QString& path);
    // File / Media Pool › Find Missing Media… (MainWindowRelink.cpp), also after opening a project with missing media:
    // looked for by itself near the old places (in the background, core/Relink.h), the clear matches relinked as one
    // undo step and said in a short note; the dialog ("Search Folder…" / "Leave Offline") only for the rest.
    // atOpen: after loading (the files of no timeline are asked about only then too)
    void findMissingMedia(bool atOpen = false);
    bool relinkSearching() const { return bool(m_relinkJob); } // tests
    QAction* findMissingAction() const { return m_findMissingAction; } // tests
    // A project that opened but not as written (ProjectFile::load warning): told once, without waiting for an answer
    void showLoadWarning(const QString& warning);
    // Video as project (`schneidi <video>`, File › Open Video as Project…): a new unsaved project whose format (size,
    // frame rate) comes from the first video, the files on its timeline one after another. Does not ask about the
    // project open before (callers do: maybeLeaveProject).
    void openMediaAsProject(const QStringList& files);
    // New project from a start template (core/StartTemplates.h; asks about unsaved changes first)
    void newProjectFromTemplate(const StartTemplate& t);
    // Content templates (core/TemplateLibrary.h, MainWindowTemplates.cpp). New: an untitled project from it (without
    // its template description); edit: the template's own project (saving changes the template). Both ask about
    // unsaved changes first; false = not readable (message shown) or cancelled
    bool newProjectFromContentTemplate(const ContentTemplate& t);
    bool editContentTemplate(const ContentTemplate& t);
    // File › New from Template…: the browser (content and start templates)
    void templateBrowser();
    // File › Save as Template…: dialog, then TemplateLibrary::save (preview = the viewer's picture at the playhead)
    void saveContentTemplate();
    // Timeline › Clip › Mark as Placeholder… (the first selected clip that can be one; the Inspector shows its rules)
    // and Remove Placeholder (all selected), one undo step each
    void markPlaceholder();
    void removePlaceholders();
    // "Check Fields": the placeholders of the open project and what is wrong with them
    void checkPlaceholders();
    bool offerAutosaveRestore(); // after a crash: offer the last automatic backup (true = restored)
    // Restore the backup without the question (Android: the system ends apps in the background, a "crash" there is
    // normal; env SCHNEIDI_SILENT_RESTORE=1/0 decides elsewhere, tests)
    static bool restoresWithoutAsking();
    void autosave(); // the automatic backup now (only when modified; Android: also when the app goes to the background)
    // File › Open Backup… after the file dialog: the backup as an untitled, modified project; the automatic backup
    // of the project left is removed (false = not readable, message shown)
    bool openBackup(const QString& path);
    void disableAutosave();      // test runs: do not write/delete a backup
    // The window's normal geometry on its screen before the first show (main.cpp, then maximized): most of the available
    // area (90 %, centred), so it never starts larger than the screen (1024x768, a scale factor) and is not small on a
    // big one. fitToScreen: a wanted geometry (a size given for a test run) shrunk/moved so that it and its frame
    // (`frame`: title bar and borders the window system adds, estimated) lie inside `available`; invalid, larger than
    // the screen or off it = 90 % of the room, centred
    void placeOnScreen(const QRect& wanted = {});
    // The screen a new window starts on: the one under the mouse cursor, else the primary screen (null = none)
    static class QScreen* startScreen();
    static QRect fitToScreen(const QRect& wanted, const QRect& available, const QMargins& frame = kFrameEstimate);
    static constexpr QMargins kFrameEstimate{4, 32, 4, 4};
    // Set project settings (one undo step); test helper --format
    void setProjectFormat(const ProjectFormat& format);
    // Restart (interface size): main() restarts schneidi with these arguments after quitting
    bool restartRequested() const { return m_restartRequested; }
    QStringList restartArguments() const;
    // Design or language changed (Settings, Workspace menu): applied at once - the window builds its widgets again
    // under a still of itself that then fades (rebuildUi, MainWindowRebuild.cpp). Queued (after the dialog that
    // changed it closed); nothing when neither changed.
    void applyLookChange();
    // The widgets, menus and actions made again in the saved design and language. Stays: the project with its undo
    // history, playhead, selection, every timeline's zoom/scroll, the page, panels and splitter sizes, playback, a
    // running render, background jobs, the Regie page's material and suggestions, the AI chat. false = not now (a
    // Regie job, a voice-over take or a modal dialog runs: tried again when it ended; `retry` false = not at all)
    bool rebuildUi(bool retry = true);
    int uiGeneration() const { return m_uiGeneration; } // counts rebuildUi (tests)
    // Interface size changed: a restart that comes back as it was - the project (unsaved changes too, from a file of
    // their own), its file name, page, playhead, zoom/scroll and selection (app/SessionResume.h). Asks only about a
    // running render. false = cancelled
    bool restartRestoring();
    // After such a restart (main.cpp, --resume <file>): the state back; false = not readable (nothing changed). The
    // files go only once the project is open and in this process's automatic backup.
    bool resumeSession(const QString& file);
    // The new process could not be started (main.cpp): the window back as it was, the user told
    void restartFailed();
    // At the start: resume files a restart left (its new process never opened them) offered; true = one opened
    bool offerLeftoverResume();

    // Live connection (cli/Live.h, started in main.cpp): what an AI session sees and controls (MainWindowLive.cpp)
    Project* liveProject() override { return m_project; }
    // During a tour the user's file (AIs wait meanwhile; the practice project has none)
    QString liveProjectPath() const override { return m_tourSaved.active ? m_tourSaved.path : m_projectPath; }
    Selection* liveSelection() override { return m_selection; }
    Editor* editor() const { return m_editor; } // tests
    int livePlayhead() const override;
    bool livePlaying() const override;
    void liveSeek(int frame) override;
    QString livePage() const override;
    bool liveShowPage(const QString& page) override;
    void livePlay(bool play) override;
    bool liveSave(const QString& path, QString* error) override;
    void liveGenerateProxies(const QStringList& files) override;
    QJsonObject liveProxyState(const QString& file) const override;
    QString liveProxyWriting() const override;
    QJsonObject liveRenderCache() const override;
    void liveRenderCacheControl(const QString& mode, bool pause) override;
    bool liveOpen(const QStringList& files, bool byUser, QString* error) override;
    bool liveAskTrust(const ProjectData& data, const QString& path) override;
    void bringToFront(); // window up and active (files handed over from another process)
    void liveConnectionChanged(bool connected, const QString& client) override;
    void liveChanged(const LiveChange& change) override;
    void liveActivity(const LiveActivity& activity) override;
    void liveCommandRunning(bool running, bool ownChat) override;
    void livePlan(const QString& state, const QJsonObject& plan, bool ownChat) override;
    void liveDirection(const QJsonObject& session, bool ownChat) override; // `direct` suggestions -> the Direct page
    void liveDirectApplied(const QJsonObject& session, const QJsonObject& result, bool ownChat) override; // ... applied
    bool livePlanOnly() const override;
    bool liveReviewOnly() const override;
    // AI log (click on the AI indicator, Workspace › AI Log): popup under the indicator
    void showAiLog();
    class AiLog* aiLog() const { return m_aiLog; }
    // "Connect with Claude" in Settings › AI (cli/Setup.h; main.cpp hands it in, the app does not link the CLI)
    void setClaudeSetup(const std::function<QJsonObject(bool remove)>& setup) { m_claudeSetup = setup; }
    // Buttons of the AI indicator in the top bar and Settings › AI: end the session and refuse new ones (reason for
    // the AI; empty = the user disconnected it) / allow again / how long a silent AI keeps the app (ms, 0 = always)
    void setLiveControls(const std::function<void(const QString& reason)>& disconnect, const std::function<void()>& allow,
                         const std::function<void(int ms)>& idleLimit);
    // Settings (File › Settings…, MainWindowSettings.cpp); test runs apply them without saving, except the Timeline
    // tab: its choices live only in the settings (TimelineOptions), so a test run leaves them as they are
    void settingsDialog(int tab = 0);
    // After Settings › Timeline changed: the timelines redraw what they show by those choices (no restart)
    void applyTimelineOptions();
    // Texts of the big step/nudge actions with the sizes chosen in Settings › Timeline
    void updateStepTexts();
    // What AIs on the live connection hear while the app cannot take their commands (Cli::Live::Server::setWaiting;
    // main.cpp hands it in): a tour runs on a practice project, nothing of an AI may land there
    void setLiveWaiting(const std::function<void(const QString& message)>& waiting) { m_liveWaiting = waiting; }
    const AiSettings& aiSettings() const { return m_aiSettings; }
    void setAiSettings(const AiSettings& s);
    // Settings › Proxy quality: stores it, offers to make the project's existing proxies again at the new size
    void setProxyQuality(ProxyManager::Quality quality);
    // AI chat (the "AI" panel, MainWindowChat.cpp): main.cpp hands in its tool server (`schneidi-cli mcp`; the app
    // does not link the command layer) with the environment of the live connection (Cli::Live::Server::chatEnvironment)
    void setAiChatTools(const QStringList& command, const QHash<QString, QString>& environment);
    void setAiChatOffline(bool on); // test runs: the chat never sends anything
    // Runs a command for the chat's plan card (the live connection's Cli::Live::Server::runForChat; main.cpp hands it
    // in): its reply {"ok":true,"result"} or {"ok":false,"error":{code,message}}
    void setChatRunner(const std::function<QJsonObject(const QString& command, const QJsonObject& args)>& run)
    {
        m_chatRunner = run;
    }
    // Runs an edit op of the command layer for one of the app's own menu entries (Clip › "Musik auf Länge…"; main.cpp
    // hands in Cli::Live::runForUser: the user's own undo step, not the AI's). Reply as for setChatRunner.
    using UserRunner = std::function<QJsonObject(const QString& command, const QJsonObject& args, const QString& undoText)>;
    void setUserRunner(const UserRunner& run) { m_userRunner = run; }
    // Clip › "Musik auf Länge…": the selected song's audio clip cut to the length of the timeline, the In/Out range or
    // a number (FitMusicDialog -> op fit_music, one undo step); errors in the UI language
    void fitMusicDialog();
    // An effect sound dropped from the Effects Library ("Klänge"): op 'sfx' with its moment at `frame` (one undo step,
    // the op's rules: own SFX track, spacing); a sound the rules leave out says why in the status bar
    void placeSound(const QByteArray& sound, int frame);
    // A compositing recipe from the Effects Library (double-click or dropped on a clip) onto video clips `ids`: its
    // required files asked for, one undo step, a failure in a message box (CompUi::applyRecipeAsking)
    bool applyRecipe(const QVector<int>& ids, const QString& recipe); // false = not applied
    class AiChat* aiChat() const { return m_chat; }
    class AiChatPanel* aiChatPanel() const { return m_chatPanel; }
    void showAiChat(bool on);
    // Update hint (app/UpdateCheck.h): automatically at most once a day a few seconds after the start (main.cpp, not
    // in test runs); manual = Help › Check for Updates… (also says "up to date" or why it failed)
    void startUpdateCheck();
    void checkForUpdates(bool manual);
    // Watch folders of the open project (MainWindowWatch.cpp): File › Watch Folders…, the watcher
    void watchFoldersDialog();
    class FolderWatch* folderWatch() const { return m_folderWatch; }
    // Large viewer (Workspace › Large Viewer, the viewer's button; MainWindowLayout.cpp): on every page the panels
    // beside the viewer and the timeline hide (Media Pool/Storage, Effects, Inspector, Transcript, Mixer, color
    // wheels, render settings, AI chat), the viewer gets their room; where the picture is then limited by the height,
    // the timeline below gives it height too, down to a third of the column. Off (same key, Esc)
    // brings back exactly what was there (which panels, splitter sizes); a page switch or a panel button ends it first.
    // Never touches the settings (panels/…, layout/…) or the timeline's zoom/scroll.
    void setViewerLarge(bool on);
    bool viewerLarge() const { return m_large.active; }
    // Full-screen viewer (Workspace › Full-Screen Viewer, Ctrl+F, the viewer's button): only the picture, black around
    // it, on the screen the window is on (ui/FullScreenViewer); the playback keys work there, Esc or the same key leave.
    // Playback goes on across the switch.
    void setViewerFullScreen(bool on);
    // The whole program full screen (Workspace › Full-Screen App, Ctrl+F11; not the viewer's full screen above): the
    // window without its frame and the desktop's bars, menus and panels as they are. Off (the same key or menu entry,
    // not Esc - Esc has its own jobs in the editor) brings back the state before: maximized or normal.
    void setAppFullScreen(bool on);
    class FullScreenViewer* fullScreenViewer() const; // nullptr while it is off
    // The back key (Android; ui/touch Touch::installBackKey, after popups and dialogs): one step back - the
    // full-screen viewer, then the large viewer, then another page back to Edit; on Edit with nothing left it asks
    // whether to quit (never quits by itself). Always true (the key is taken)
    bool backStep();
    // Layout forms (MainWindowPhone.cpp, ui/phone/PhoneShell): Desktop (pages, splitters), Tablet (the middle form:
    // top bar, tool rail, viewer above the timeline, one panel docked at the side) and Phone (viewer, timeline, a tool
    // bar, panels as sheets). Chosen by the window's size in logical px (wantedForm), switched live when it crosses a
    // threshold or the setting changes: the same widgets move, nothing is rebuilt
    enum class LayoutForm { Desktop, Tablet, Phone };
    // The form for a window of `logical` size; `current` = the one shown (a little hysteresis around the thresholds).
    // Setting ui/layoutForm (auto/phone/tablet/desktop; the older ui/phoneLayout on/off), SCHNEIDI_LAYOUT=phone|
    // tablet|desktop or SCHNEIDI_PHONE_LAYOUT=1/0 pin it
    static LayoutForm wantedForm(const QSize& logical, LayoutForm current = LayoutForm::Desktop);
    static bool phoneLayoutWanted(); // the start: wantedForm(the screen) == Phone
    // Set while a silent autosave restore starts, cleared 2 s after the window is up: still set = that start crashed
    static constexpr const char* kRestoreStartKey = "autosave/silentRestorePending";
    // "Automatisch" picks the form by the window's size only on Android; a desktop system stays Desktop at any size
    // (half a screen, a portrait monitor) unless the setting or SCHNEIDI_LAYOUT asks for another form.
    // SCHNEIDI_LAYOUT_BY_SIZE=1/0 and setFormBySize (tests) override
    static bool formBySize();
    static void setFormBySize(std::optional<bool> on);
    // The input is a finger (touch mode or a touchscreen): the shell's centred playhead, magnetic V1 and hold-to-move
    static bool touchInput();
    static constexpr int kPhoneEdge = 600;     // shorter side below: a phone (unless the window is wide)
    static constexpr int kDesktopWidth = 1100; // width from here: the desktop layout
    LayoutForm layoutForm() const;
    // Live: project, unsaved changes, selection, playhead, zoom and scroll stay (core rule); the desktop's splitter
    // sizes, open panels and page come back as they were when it returns
    void setLayoutForm(LayoutForm form);
    class QAbstractButton* addMediaButton() const { return m_addMediaButton; } // tests
    void enablePhoneLayout(); // = setLayoutForm(Phone) (tests call it on a window built without it)
    class PhoneShell* phoneShell() const { return m_phone; }
    // Preview on a second monitor (Workspace menu, Ctrl+Shift+F; ui/SecondScreen, MainWindowLayout.cpp): the key moves
    // it to the next monitor other than the one the main window is on, after the last one back into the window (two
    // monitors: on/off). Full screen there, letterboxed; it does not take the keyboard focus. The main window keeps
    // the viewer's area for source clips or closes it up (Settings: secondScreenMode). An unplugged monitor brings it
    // back into the window; plugging one in never moves it by itself. Timeline zoom/scroll stay untouched.
    void moveViewerToNextScreen();
    // The preview on `screen` (nullptr = back into the main window). Any screen, also the main window's own (tests)
    void showPreviewOn(class QScreen* screen);
    class PreviewOutput* previewOutput() const { return m_output; }
    void setSecondScreenMode(SecondScreenPlan::Mode mode);
    SecondScreenPlan::Mode secondScreenMode() const { return m_secondScreenMode; }
    const SecondScreenPlan& screenPlan() const { return m_screenPlan; }
    // Preview resolution (engine/PreviewResolution), main window and second monitor separately; stored, applies at
    // once (Workspace › Preview Resolution, the viewer's right-click, Settings)
    void setPreviewResolution(PreviewResolution::Target target, PreviewResolution::Choice choice);
    PreviewResolution::Choice previewResolution(PreviewResolution::Target target) const
    {
        return target == PreviewResolution::Target::Viewer ? m_viewerRes : m_monitorRes;
    }

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override; // the layout form for the new size (a moment later)
    void showEvent(QShowEvent* event) override;     // (the desktop's splitter sizes, once laid out)
    void changeEvent(QEvent* event) override; // window state: the full-screen action follows

private:
    void buildLayout();
    QWidget* buildTopBar();
    QWidget* buildPageBar();
    void showStatusText(const QString& text); // in the page bar: a status message, empty = the name and version
    void updatePageButtons(); // hidden pages' buttons go, the current page's button always stays
    void updateLeftColumn(); // show the left column only if Media Pool or Effects is on
    void updateRightColumn(); // the Inspector above the Mixer in their own column while the Mixer is on
    int rightColumnWidth() const; // what that column needs: the mixer's strips at natural width, the Inspector usable
    // Sets that column's splitter to `sizes`, its column at rightColumnWidth(): exact = always (defaults), else only
    // when it is much wider (a saved width)
    void fitRightColumn(QList<int> sizes, bool exact);
    // Panel sizes per page are remembered (settings layout/<splitter>); defaults when nothing is saved yet
    void watchSizes(QSplitter* s, const QString& name);
    void applySizes(QSplitter* s, const QList<int>& defaults);
    QList<int> sizesFor(QSplitter* s, const QList<int>& defaults); // what applySizes sets (saved, else defaults)
    // A panel just switched on that its splitter still holds at size 0 (sizes restored while it was hidden collapse
    // it): an equal share of the room, taken from the others
    static void uncollapse(QWidget* panel);
    void resetSizes();
    // Panel set of the Edit page (Workspace → Panels): which toggles are on
    void applyPanelSet(bool pool, bool effects, bool storage, bool transcript, bool mixer, bool inspector);
    void onDrop(const QStringList& paths, int frame, int track, PlaceParts parts = PlaceParts::Both);
    void addShape(int frame, int track, const QString& kindId);
    // Subtitles (SRT): every file becomes a new subtitle track; export of the visible track
    void importSubtitleFiles(const QStringList& paths);
    void importSubtitlesDialog();
    void exportSubtitlesDialog();
    // Timelines of other editors (FCP7 XML, OpenTimelineIO, EDL), MainWindowExchange.cpp
    void importTimelineDialog();
    void contactSheetDialog(); // File › Save Contact Sheet: stills of the timeline as one picture
    void exportTimelineDialog();
    void transcribeDialog();
    // Timeline › Subtitles › Re-split: the subtitle track of the selection (else the visible one) regrouped into cues
    // of this length (Subtitles::readable; stored word times kept, otherwise shared by word length), one undo step
    void resplitSubtitles(Subtitles::CueLength length);
    void speechDialog();
    MediaInfo probeCached(const QString& path);
    void probeLater(const QString& path); // full probe in the background into m_probeCache
    // Like DaVinci with the first clip in an empty timeline: adapt the project to the clip's resolution/frame rate
    // (ask = ask first, otherwise take over directly)
    void offerClipFormat(const QStringList& paths, bool ask);
    void onFormatChanged();
    void onTimelineFormatChanged();
    void projectSettingsDialog();
    void grabStill();
    // Green Screen pipette (Inspector): the viewer shows the clip alone without its key until a click picks the color
    // qualifier = the Color page's qualifier pipette (the picture before the qualifier, LUT and windows)
    void startKeyPick(int clipId, bool qualifier = false);
    void stopKeyPick();
    void updateRenderCacheBar(); // render cache bar above the timeline
    void updateRenderCacheJob(bool allDone); // its job at the bottom right (ui/BackgroundJobs)
    void clipSpeedDialog();
    void normalizeAudioDialog();
    // Import presets for audio streams and ducking (MainWindowAudio.cpp)
    void streamPresetsDialog();
    const Clip* streamPresetsClip() const; // selected clip whose stream setup can be remembered (nullptr = none)
    void rememberStreamSettings();
    void duckingDialog();
    // Effects of the selected clip as own preset (effects/Presets, Kdenlive format, core/Presets)
    void saveEffectPreset();
    const Clip* presetClip() const;
    void buildActions();
    QAction* makeAction(QMenu* menu, const QString& id, const QString& text, const QKeySequence& key,
                       const std::function<void()>& fn);
    // Source view (double-click in the Media Pool): open at the last shown position of the file
    void showSource(const QString& path);
    // Click in the Media Storage: the file in the source view without importing it (Editor::setPreviewSource; a pool
    // clip shows as with showSource). Its length comes from a probe in the background, the picture right away.
    void previewFile(const QString& path);
    bool sourceActive() const; // Viewer shows the source -> I/O apply to the source In/Out
    QString editSource() const; // source for F9/F10 …: source view, otherwise selection in the Media Pool
    void sourceEdit(Editor::SourceEditMode mode);
    void sourceEditPath(Editor::SourceEditMode mode, const QString& path); // a given file (Media Pool menu)
    void updateViewer();        // scrubber range, In/Out and title of the Viewer
    void jumpToEdit(int direction);
    void jumpToMarker(int direction);
    void jumpToFrame(int frame); // pauses, then jumps (AI/live and other programmatic jumps)
    // A jump key (Home/End, edit points, markers, In/Out): playback after it as the user chose (PlayheadMove)
    void keyJump(int frame);
    void jumpTo(const QVector<int>& points, int direction);
    void stepFrames(int frames);
    JklKeys* m_jkl = nullptr; // J/K/L with K held (buildActions)
    // Stop and go back to where playback last started (Space by choice, the action "stop_return"); paused: only back
    void stopAndReturn();
    int m_playStart = -1;          // position where playback last started (Engine::speedChanged from 0); -1 after
                                   // a project/timeline switch
    int m_playStartMode = -1;      // its Engine::Mode: back only within the same mode
    double m_lastSpeed = 0;

    void newProject(); // the default start template (or the user's choice, if they want to be asked)
    void openProjectDialog();
    void openMediaAsProjectDialog();
    // File › Save as Start Template…: the open project's format, tracks and import presets; returns the saved id (empty
    // = not saved), madeDefault = new projects start with it now
    QString saveStartTemplate(QWidget* parent, bool* madeDefault = nullptr);
    void rebuildTemplateMenu(); // File › New Project from Template
    bool save();
    bool saveAs();
    void openBackupDialog();
    bool saveTo(const QString& path);
    // The data to save at `path` made independent of the template it came from (m_templateDir); false = a copy
    // failed (asked nothing, said why). `written`: the copies made now (removed again when saving fails)
    bool copyTemplateFiles(ProjectData& data, const QString& path, QStringList* written);
    bool maybeSave(); // false = cancel
    // Before New/Open: if the render queue is running, ask and cancel (otherwise it writes its status
    // into the queue of the next project), then maybeSave(). false = cancel
    bool maybeLeaveProject();
    void startProject(const StartTemplate& t); // the new project (after maybeLeaveProject)
    void offerRestart(const QString& message, const QString& now, const QString& later);
    void offerScaleRestart(); // the interface size changed: restart now (restartRestoring) or later
    // ask = offer relinking missing media / list missing effects (not when reloading after an outside change)
    bool applyLoaded(ProjectData data, const QString& path, bool ask = true);
    // Find Missing Media (MainWindowRelink.cpp): one search in the background (a second request while it runs: none),
    // `then` with how many were relinked; the "Medien fehlen" dialog for what is still missing; the menu entries
    // enabled while files are missing
    void startRelinkSearch(const QVector<Relink::Root>& roots, bool near, const std::function<void(int)>& then);
    void askForMissingMedia();
    void updateFindMissingAction();
    // Project file changed outside (schneidi-cli, an AI, another window): reload; unsaved changes here ask first
    void checkProjectFile();
    void setProjectPath(const QString& path);
    void updateTitle();
    static QString appName(); // "schneidi 0.9.0" or "schneidi 0.9.0 → 0.10.0" (without the build state)
    void addRecent(const QString& path);
    void rebuildRecentMenu();
    void removeAutosave();
    // Every running instance has its own backup slot (QLockFile) so that two windows do not
    // overwrite or delete each other's backup; slot 1 = "autosave.schneidi" as before
    QString autosavePath();
    bool adoptOrphanedAutosave(); // takes over a free slot that holds a backup (true = this window's slot now)
    QString autosaveKey() const; // QSettings key: which project the backup belongs to
    void createCompoundClip(); // ask for the name like DaVinci
    // Comp page (MainWindowComp.cpp)
    void ensureCompPage();
    void ensureStudioPage();
    void ensureDirectPage();
    void directSuggest();                 // the page's recipe on its material (in the background, BackgroundJobs)
    void directApply();                   // the checked suggestions -> a timeline (one undo step)
    void directFinish();                  // after a short's apply: captions + effect sounds (one more undo step)
    void showDirectSession(const QJsonObject& session); // suggestions on the page (a run's or the chat's)
    void directPreview(const QString& file, double from, double to); // a suggestion in the viewer, stops at its end
    // Ends a running suggestion/apply job and empties the suggestions (the material changed, another project);
    // newProject also empties the material
    void directReset(bool material);
    void directClearPreviewStop() { m_directStopAt = -1; }
    void updateDirectTracks(); // the talk recipe's track list from the open timeline
    void setDirectSourceTimeline(const QJsonObject& session); // a talk session's timeline for the result strip
    // The own wish line: shown only with a chat provider set up (as the chat panel sees it, not its setup page)
    void updateDirectWish();
    void directWish(const QString& text); // to the AI chat with the page's context; its `direct` answers on the page
    void directWishDone();                // the chat's turn of a wish ended: the answer or error under the line
    void directAiApplyUndone();           // review: the AI's changes discarded - its apply is no more (no finish)
    void ensureColorPage(); // the Color page's widgets (color wheels, scopes), built on the first use
    void connectColorPage(); // its connections (MainWindow.cpp, beside the other panels')
    ColorPanel* colorPanel()
    {
        ensureColorPage();
        return m_colorPanel;
    }
    int selectedVideoClip() const; // first selected video clip (0 = none)
    // What the viewer plays: the project, on the Comp page with the graph cut at the shown node / its matte
    Timeline previewTimeline() const;
    void updatePreview(); // previewTimeline() to the engine now
    // Pipette of a comp node's color (key color): the picture going into the node, picked in the key pick window
    void startCompKeyPick(int clipId, int node, const QString& key);
    void openSelectedCompound(); // also a multicam clip (like DaVinci "Open in Timeline")

    // Multicam (MainWindowMulticam.cpp): viewer mode with the grid, keys 1-9 cut / Alt+1-9 switch (only there,
    // otherwise Alt+N stays the video target track), Media Pool dialog, timeline right-click
    void buildMulticamActions(QMenu* timeline);
    void buildMulticamViewerAction(QMenu* playback);
    void setupMulticam(); // connections, after buildActions()
    void setMulticamMode(bool on);
    void syncMulticam(); // the engine's grid follows the multicam clip at the playhead (MulticamView)
    void updateMulticamProxies(); // proxy state of the grid's angles -> MulticamView (hint + "Generate proxies")
    // Playback too slow for the originals: hint above the picture with "Generate proxies" (never unasked)
    void setupProxyHint();
    void updateProxyHint();
    // Background work at the bottom right of the status bar (ui/BackgroundJobs): proxies today
    void setupBackgroundJobs();
    bool offersRenderCache() const; // the hint offers "Render-Cache: Smart" instead of proxies
    // The viewer's preview resolution one step smaller (PreviewResolution::smaller) when that makes the engine's
    // picture noticeably smaller: not while zoomed to 100 %, the viewer is away or a second monitor wants it as large
    std::optional<PreviewResolution::Choice> smallerPreview() const;
    int previewEdgeFor(PreviewResolution::Choice viewerRes) const; // what updatePreviewEdge asks for with that choice
    QStringList playingVideoFiles() const; // video files of what plays (timeline incl. nested clips, or the source)
    bool expensiveAtPlayhead() const;      // a clip with effects or changed speed plays here (RenderCache::isExpensive)
    void createMulticamClip(const QStringList& paths);
    void addMulticamMenu(QMenu* menu); // "Switch Angle" + "Flatten" for selected multicam segments
    int selectedMulticamSequence() const;
    // Auto-Reframe (MainWindowReframe.cpp): the selected video clips (none = the whole timeline) fill the open
    // timeline's shape and follow their subject; a timeline as a 9:16 copy, reframed (Media Pool)
    void autoReframe();
    void reframedCopy(int sequenceId);
    // What a reframe from the app keeps above the picture (menu switch "reframe/keepKillFeed", default on)
    Reframe::KeepChoice reframeKeepChoice() const;
    // Measures the clips with a progress dialog (window modal); false = cancelled. feeds: the kill feed lines of the
    // clips a reframe keeps a kill feed of (engine/KillFeedLines)
    bool measureReframe(const QVector<Clip>& clips, const ProjectFormat& format, QHash<int, QVector<Reframe::Key>>* paths,
                        QHash<QString, QRectF>* contents = nullptr, QHash<int, Reframe::Feed>* feeds = nullptr);
    // Clip menu: crop the black bars baked into the selected clips (engine/BarDetect), show them as stretched 4:3
    void cropBlackBars();
    void setStretched43(bool on);
    // Hide player names (MainWindowNameBlur.cpp): the selected video clips (none = the whole timeline) get windows
    // that blur or pixelate the text found in them
    void blurNames();

    Engine* m_engine;
    PlayheadMove::Gesture m_playheadMove; // a click/drag of the playhead in the ruler, overview strip, Studio lanes
    Project* m_project;
    Selection* m_selection;
    Editor* m_editor;

    MediaPool* m_mediaPool = nullptr;
    class BackgroundJobs* m_backgroundJobs = nullptr;
    bool m_renderCacheJob = false; // the render cache's job is shown as running (updateRenderCacheJob)
    EffectsLibrary* m_effects = nullptr;
    MediaStorage* m_storage = nullptr;
    Viewer* m_viewer = nullptr;
    class MulticamView* m_multicam = nullptr;
    class QAction* m_multicamAction = nullptr;
    bool m_multicamMode = false;
    bool m_multicamSyncing = false, m_multicamSyncAgain = false;
    QTimer m_engineTimer; // bundles model changes -> engine
    QElapsedTimer m_engineSent; // since the last timeline handed to the engine
    PlaybackMeter m_playMeter;
    QElapsedTimer m_playClock;
    bool m_slow = false;     // the last measurement found playback too slow (the hint decides what to offer)
    QStringList m_slowFiles; // its video files (proxies)
    // Every file had its proxy (or proxies are off) when it was measured: proxies did not help, a smaller preview may
    bool m_slowWithProxies = false;
    // The viewer's resolution choice when it was measured (changed since: the measurement says nothing about a smaller
    // preview; a viewer resize does not count, the hint bar itself takes some of its height)
    PreviewResolution::Choice m_slowRes = PreviewResolution::Choice::Auto;
    enum class HintOffer { None, SmallerPreview, RenderCache, Proxies };
    HintOffer m_hintOffer = HintOffer::None; // what the hint's button does
    // SmallerPreview: the choice the hint named and the one it was offered from (changed since: offered anew)
    PreviewResolution::Choice m_offeredRes = PreviewResolution::Choice::Auto, m_offeredFrom = PreviewResolution::Choice::Auto;
    void restartPlayMeter(); // after the hint's action while playing: seconds from before it do not count
    double m_slowRate = 0, m_slowFps = 0;
    bool m_proxyHintClosed = false; // closed: not again in this session
    // Files imported since the last pass to ProxyManager::importedFiles (proxies on import): one probe per import
    // of several files
    QStringList m_importedMedia;
    bool m_slowEffects = false;     // clips with effects played: the hint offers the smart render cache first
    Inspector* m_inspector = nullptr;
    ColorPanel* m_colorPanel = nullptr;
    ShapeOverlay* m_shapeOverlay = nullptr; // the selected shape's box over the viewer (Edit page, 0.22)
    // Painting in the viewer (tool "Malen", key P, 0.22): the Edit page's overlay (the shape box below it) and the
    // brush bar over the viewer while the tool is on
    class DrawOverlay* m_drawOverlay = nullptr;
    class DrawBar* m_drawBar = nullptr;
    QAction* m_drawAction = nullptr;
    void setDrawTool(bool on);
    void drawToolChanged(bool on);
    QPointer<class KeyTrainer> m_keyTrainer;     // Help › Practice keys, while open
    QPointer<class KeyColorPicker> m_keyPicker; // while the pipette is on
    QPointer<class KeyPickWindow> m_keyPickWindow; // its large picture, while the pipette is on
    TranscriptPanel* m_transcript = nullptr;
    ScopesPanel* m_scopes = nullptr; // Color page, right of the wheels (like DaVinci)
    Mixer* m_mixer = nullptr;
    TimelinePanel* m_timeline = nullptr;
    DeliverPanel* m_deliver = nullptr;
    class CompPage* m_compPage = nullptr; // built on the first showing (startup stays fast)
    class StudioPage* m_studioPage = nullptr; // built on the first showing (startup stays fast)
    class DirectPage* m_directPage = nullptr;   // built on the first showing (startup stays fast)
    std::unique_ptr<class DirectBackend> m_directBackend;
    std::function<void()> m_directCancel; // the running suggestion job's cancel (empty = none)
    std::function<void()> m_directApplyCancel; // the running apply job's cancel (empty = none)
    int m_directApplyRun = 0;
    QJsonObject m_directApplied; // the last apply's result and session (for the short's finish)
    // A wish of the page runs as a chat turn: its first chat entry
    bool m_directWishTurn = false;
    int m_directWishFirst = 0;
    bool m_directWishVoid = false;   // the page was reset during the AI's request: its suggestions/apply not shown
    bool m_directAppliedByAi = false; // m_directApplied came from an AI's `direct` apply
    class QAction* m_directRemoveSuggestion = nullptr; // the page's Delete keys, registered at the start (buildActions)
    class QAction* m_directRemoveMaterial = nullptr;
    QList<int> m_directCenterSizes; // viewer | recipes while the viewer is on another page
    int m_directRun = 0;                  // counts suggestion jobs: a late reply of an older one is dropped
    int m_directStopAt = -1;              // a previewed suggestion stops at this source frame (-1 = none)
    QString m_directStopFile;             // ... of this file (another file in the viewer: no stop)
    bool m_directSized = false;
    bool m_directTracksDirty = false; // the timeline changed while the Regie page was hidden (updateDirectTracks)
    // The Studio page's sizes are set once (its defaults or the user's), then it keeps them: a round trip to another
    // page changes nothing (the defaults depend on the strips' widths, which settle only once the page showed). The
    // viewer's height in its column is kept while the viewer is on another page. Reset: a tour, "reset sizes".
    bool m_studioSized = false;
    QList<int> m_studioSideSizes;

    QStackedWidget* m_pages = nullptr;
    QSplitter* m_editTop = nullptr;
    QSplitter* m_editLeft = nullptr;   // Media Pool above Effects Library
    QSplitter* m_editMain = nullptr;   // the top row above the timeline
    // The Edit page: the main column beside the right column (Inspector above the Mixer, only while the Mixer is on).
    // Each has its own height there: making the timeline taller does not drag the mixer along
    QSplitter* m_editRow = nullptr;
    QSplitter* m_editRight = nullptr;
    QWidget* m_inspectorSlot = nullptr; // the Inspector's place in the top row while it is in the right column
    QSplitter* m_mediaPage = nullptr;
    QSplitter* m_mediaTop = nullptr;
    QSplitter* m_deliverPage = nullptr;
    QSplitter* m_deliverRight = nullptr;
    QSplitter* m_colorPage = nullptr; // Viewer, timeline, color wheels (like DaVinci Color)
    QSplitter* m_colorBottom = nullptr; // color wheels | scopes
    QHash<QSplitter*, QList<int>> m_defaultSizes;
    QButtonGroup* m_pageButtons = nullptr;
    class ElidedLabel* m_statusText = nullptr; // left in the page bar: status messages (the status bar is hidden)
    class QHBoxLayout* m_pageBarRight = nullptr; // right in the page bar: background work
    PageVisibility m_pageVisibility{{"media", "edit", "comp", "color", "studio", "direct", "deliver"}}; // bar order
    QToolButton* m_poolToggle = nullptr;
    QToolButton* m_effectsToggle = nullptr;
    QToolButton* m_storageToggle = nullptr;
    QToolButton* m_inspectorToggle = nullptr;
    QToolButton* m_mixerToggle = nullptr;
    QToolButton* m_transcriptToggle = nullptr;
    Page m_page = Page::Edit;
    struct LargeViewer {
        bool active = false;
        QVector<QPointer<QWidget>> hidden;                     // panels it hid, shown again when it ends
        QVector<QPair<QPointer<QSplitter>, QList<int>>> sizes; // splitter sizes before
        QVector<QPair<QPointer<class QScrollBar>, int>> scrolls; // scroll positions in the hidden panels
    } m_large;
    QAction* m_largeAction = nullptr;
    class QShortcut* m_largeEscape = nullptr; // Esc ends the large viewer (only enabled while it is on)
    class QShortcut* m_drawEscape = nullptr;  // Esc ends painting (only enabled while it is on)
    void updateEscapeKeys(); // which Esc shortcut is on: recording, then painting, then the large viewer
    // Esc with the focus in a text/number field: handed to the field and the focus taken out of it (true); false = not in one
    bool escapeToFocusedField();
    QAction* m_fullScreenAction = nullptr;
    QAction* m_appFullScreenAction = nullptr;
    bool m_maximizedBeforeFullScreen = true; // the window state setAppFullScreen(false) brings back
    QPointer<class FullScreenViewer> m_fullScreen; // while the full-screen viewer is on
    QPointer<QWidget> m_focusBeforeFullScreen;      // gets the keyboard focus back when it ends
    // Second monitor (MainWindowLayout.cpp)
    void screensChanged(class QScreen* removed); // the plan learns the screens; the preview comes back if its went
    void feedPreviewOutput(const QImage& frame);
    // The largest wish of the viewer, the full-screen viewer and the output, each after its PreviewResolution choice
    // (Engine::setPreviewEdge: one preview, the larger size wins)
    void updatePreviewEdge();
    void hideViewerArea();    // the viewer and the splitters left empty by it go, their sizes are remembered
    void restoreViewerArea(); // back as it was before hideViewerArea, but with what the user dragged meanwhile
    void watchViewerAwaySplitters(); // notes the splitters of m_viewerAway the user drags meanwhile
    void updateViewerArea();  // hidden or back, as the mode and the output say
    QAction* m_secondScreenAction = nullptr;
    QAction* m_secondScreenModeActions[2] = {}; // Workspace › Main Window with a Second Monitor, by Mode
    QMenu* m_viewerScreenModeMenu = nullptr;     // the same on the viewer's right-click (only while the output is on)
    QPointer<class PreviewOutput> m_output;
    SecondScreenPlan m_screenPlan;
    SecondScreenPlan::Mode m_secondScreenMode = SecondScreenPlan::storedMode();
    // Read in the constructor with the format known at start (an old "half" becomes the fixed size nearest to half of it)
    PreviewResolution::Choice m_viewerRes = PreviewResolution::Choice::Auto;
    PreviewResolution::Choice m_monitorRes = PreviewResolution::Choice::Auto;
    // [Target][Choice]: the menu's entries (check marks, disabled above the project, tooltips with the size)
    QAction* m_resolutionActions[2][PreviewResolution::kChoiceCount] = {};
    void updateResolutionMenus(); // texts, enabled state and tooltips for the current project
    QImage m_programFrame; // the engine's last frame not of a source clip (the output starts with it)
    struct ViewerAway {
        bool active = false;
        QVector<QPointer<QWidget>> hidden;                     // the viewer, then the splitters left empty
        QVector<QPair<QPointer<QSplitter>, QList<int>>> sizes; // their sizes before, innermost first
        bool timelineUp = false; // Edit page: the timeline took the viewer's place in the top row
        QSizePolicy timelinePolicy; // its own before (the viewer's stretch while up there)
        QSet<QSplitter*> moved; // of `sizes`: dragged by the user meanwhile (their new sizes stay)
        QVector<QMetaObject::Connection> watches; // splitterMoved of `sizes`
    } m_viewerAway;

    ProbeCache m_probeCache; // drop preview, Viewer of a file not in the pool, import of a dropped file
    QSet<QString> m_probing; // probeLater() running for these
    QAction* m_findMissingAction = nullptr;              // File › Find Missing Media… (also in the Media Pool's menu)
    std::shared_ptr<Relink::Progress> m_relinkJob;    // the search running (null = none)
    QWidget* m_relinkStatus = nullptr;                   // its line in the status bar (folders searched, Cancel)
    QHash<QString, int> m_sourcePos; // source playhead per file (this session only)
    QHash<int, int> m_sequencePos;   // playhead per timeline/compound clip (this session only)
    QHash<int, ViewState> m_sequenceView; // zoom/scroll per timeline/compound clip (saved in the project: views)
    QVector<SavedView> savedViews() const; // every timeline's zoom/scroll/playhead for the project file
    void restoreViews(const ProjectData& data);
    int m_shownSequence = 0;         // last shown sequence (for m_sequencePos)
    class MediaCache* m_mediaCache = nullptr;

    QString m_projectPath; // empty = never saved yet
    QString m_saveSuggestion; // never saved: where Save As suggests (a video opened as project: beside it)
    // New from a content template: its folder. Saving copies the files of it the project uses next to the project
    // ("<name> media/"), so the saved file never points into the template (MainWindowTemplates.cpp)
    QString m_templateDir;
    QHash<QString, QString> m_templateCopies; // template file -> its copy (this session)
    class QFileSystemWatcher* m_projectWatcher = nullptr;
    QTimer m_projectCheck;            // waits until an outside save is done
    QByteArray m_projectFingerprint;  // project file as last loaded/saved here (ProjectFile::fingerprint)
    QMenu* m_recentMenu = nullptr;
    QMenu* m_templateMenu = nullptr;
    class ElidedLabel* m_titleLabel = nullptr; // project name in the top bar
    class QTimer* m_autosaveTimer = nullptr;
    bool m_autosaveDisabled = false;
    int m_autosaveSlot = 0; // 0 = none assigned yet
    std::unique_ptr<QLockFile> m_autosaveLock;
    bool m_restartRequested = false;

    // AI indicator in the top bar (MainWindowLive.cpp)
    void updateLiveIndicator();
    AiSettings m_aiSettings; // Settings › AI

    // Update hint: bar below the top bar with "Now / Later / Don't ask again" (MainWindowSettings.cpp)
    QWidget* buildUpdateBar();
    class UpdateCheck* m_updates = nullptr;
    QWidget* m_updateBar = nullptr;
    QWidget* m_scaleBar = nullptr; // offerUiScale
    class QLabel* m_updateText = nullptr;
    QUrl m_updatePage;
    QStringList m_updateNews; // the bar's newer version and the running one (rebuildUi shows it again)
    double m_scaleOffer = 0;  // the factor offerUiScale's bar offers
    static QString liveOffReason(); // for the AI: connections are off in the settings
    class AiLog* m_aiLog = nullptr;
    class AiLogView* m_aiLogView = nullptr;
    void aiLogActivated(int entryId); // select the entry's clips, jump there (a click of the user)
    void applyAiMarkStyle();          // Settings › AI -> timeline highlight
    std::function<QJsonObject(bool)> m_claudeSetup;
    QWidget* buildLiveIndicator();
    QWidget* m_liveBox = nullptr;
    QToolButton* m_liveLabel = nullptr; // click: AI log
    QToolButton* m_liveButton = nullptr;
    QTimer m_liveLinger; // keeps "connected" visible a moment after a one-shot command
    bool m_liveConnected = false;
    bool m_liveRefused = false; // the user disconnected the AI
    QString m_liveClient;
    std::function<void(const QString&)> m_liveDisconnect;
    std::function<void()> m_liveAllow;
    std::function<void(int)> m_liveIdleLimit;

    // AI chat (MainWindowChat.cpp)
    void buildChat();      // panel, chat and the splitter beside the pages (before the top bar)
    void setupChat();      // connections, after the actions
    void placeChat(bool left);
    void applyChatWidth();
    void updateChatPanel(); // setup page or chat, the provider line
    bool sendChat(const QString& text, const QString& note = {}); // note: with this request only (AiChat::addNote)
    bool chatReady(); // a provider is set up (the panel shows the chat, not its setup)
    // The panel's setup page is needed (nothing chosen, a key or the address missing): the provider and why
    bool chatSetupNeeded(QString* provider = nullptr, QString* message = nullptr);
    bool chatEndpoint(Ai::Endpoint& e, QString* problem);
    QString chatKeyHint(const QString& provider);
    void chatTurnFinished();
    void chatUndoChanged();
    void acceptChatChanges();
    void discardChatChanges();
    void runChatPlan(const QString& id, const QVector<int>& steps); // plan card: the ticked steps, one undo step
    void discardChatPlan(const QString& id);
    std::function<QJsonObject(const QString&, const QJsonObject&)> m_chatRunner;
    std::function<void(const QString&)> m_liveWaiting;
    QString m_liveTourMessage; // a tour runs: AIs wait
    // Hands m_liveWaiting what AIs hear now: the tour's message, else "reading media" while the Media Pool probes an
    // import (its wait runs an event loop that would deliver live requests), else nothing
    void updateLiveWaiting();
    UserRunner m_userRunner;
    class AiChat* m_chat = nullptr;
    class AiChatPanel* m_chatPanel = nullptr;
    QSplitter* m_chatSplit = nullptr;
    QToolButton* m_chatToggle = nullptr;
    class QHBoxLayout* m_topBarLayout = nullptr;
    QWidget* m_topBar = nullptr;  // the desktop's bars (hidden in the phone layout)
    QWidget* m_pageBar = nullptr;
    class PhoneShell* m_phone = nullptr; // the phone layout (nullptr = the normal one)
    void phoneShowPage(Page page);  // showPage in the phone layout: a page becomes its panel's sheet
    void phoneTogglePanel(QWidget* panel, bool on); // a panel toggle (menu, key, workspace) in the phone layout
    QWidget* buildPhoneTopBar();
    // The background work indicator (ui/BackgroundJobs) into the phone's top bar, compact (the page bar is hidden)
    void phoneAdoptBackgroundJobs();
    QWidget* m_phoneTopBar = nullptr; // the phone's top bar: a row in portrait, the rail's head in landscape
    void phoneArrangeTopBar(bool landscape);
    void enableShell(bool tablet);  // the phone/tablet arrangement from the desktop's
    void disableShell();            // back to the desktop's, everything where it was
    struct ShellHomes* m_shellHomes = nullptr; // where the shell's widgets live on the desktop (MainWindowPhone.cpp)
    void dropShellHomes(); // forgets them (rebuildUi: the widgets they name are gone)
    QWidget* shellHome(QWidget* w);            // records w's desktop place before the shell takes it; returns w
    class QTimer* m_formTimer = nullptr;       // a resize settles before the form is checked
    void checkLayoutForm();
    void snapshotDesktop(); // the desktop's splitter sizes while it is desktop-wide (what comes back later)
    // Adding material the phone's way: schneidi's gallery (ui/MediaGallery; Android asks for the media permission
    // first, refused = the system's file picker), the picked files into the pool and onto the timeline's end
    void openGallery();
    void pickWithSystem();
    void addPicked(const QStringList& paths);
    class QAbstractButton* m_addMediaButton = nullptr; // the timeline's "+ Videos hinzufügen" (phone, tablet)
    void updateAddMediaButton();
    // The timeline's playhead fixed in the middle: setting timeline/centerPlayhead on/off, else on in the phone and
    // tablet forms (TimelineView::setCenterPlayhead)
    void applyCenterPlayhead();
    void applyScrollIndicators(); // phone form: thin indicators instead of scroll bars
    class ScrollIndicators* m_scrollIndicators = nullptr;
    QList<QAction*> m_shellShortcuts; // the hidden menu bar's actions on the window itself (shortcuts keep working)
    void setShellShortcuts(bool on);
    std::unique_ptr<Ai::Keys> m_aiKeys;
    bool m_chatCommand = false; // a command of the own chat runs in the app (its undo steps)
    // Review mode: the undo index before the request, whether something else changed the history meanwhile, and
    // whether the bar waits for the user
    struct ChatReview {
        bool active = false;
        int start = 0;
        bool broken = false;
        bool pending = false;
    } m_review;
    // The project as the chat's AI left it (after its last answer, or after accepting or discarding it): the next
    // message tells it what was changed outside the chat since - the user's own trims and moves, which a rework
    // must not undo. Empty before the first answer and after a new chat.
    std::optional<Timeline> m_chatSeen;
    int m_chatSeenSequence = 0;
    QString m_chatSeenProject;
    void rememberChatState();
    QString chatChangesNote() const; // empty: nothing changed (or another project is open)

    void setupWatchFolders(); // after the layout (Media Pool, Media Storage)
    class FolderWatch* m_folderWatch = nullptr;
    bool m_liveCommand = false; // a command of an AI runs in the app (watch folders wait with their import)

    // Voice-over recording (app/VoiceoverRecorder.h, MainWindowVoiceover.cpp)
    void createVoiceover(); // the recorder (once, before the widgets)
    void setupVoiceover(); // after the layout (Mixer, Viewer)
    void buildVoiceoverActions(QMenu* play, QMenu* audioMenu);
    void toggleVoiceover();
    // A take still running is placed before the project is left or the app quits ("save changes?" must see it)
    void endVoiceover();
    class VoiceoverRecorder* m_voiceover = nullptr;
    class QAction* m_voiceoverAction = nullptr; // record / stop (key, menu, timeline toolbar)

    // In-app tours (MainWindowTours.cpp)
    void setupTours(QMenu* help); // target names, the manager, Help › Tours
    void loadTourSample();        // the built-in sample clips into the Media Pool (a tour on an empty project)
    // Compositing tour and guides: the selected video clip (else the one at the playhead, the first one) gets a built-in
    // recipe's graph ("pip_frame", "greenscreen"), the Comp page shows it
    void tourCompExample(const QString& recipe);
    bool compHasMerge() const;    // the Comp page's clip has a merge node (what the Compositing tour explains)
    bool timelineHas(const QString& nodeType) const; // a clip of the timeline has a node of that type (the guides)
    QStringList tourSampleFiles();  // the sample clips, copied out of the resources (nothing is downloaded)
    // Tour mode (TourManager::Host enter/leave): the user's project, page, panels and splitter sizes kept aside, the
    // tour's practice project and a fixed layout shown; leave gives everything back as it was (unsaved state too).
    // While it is on, the panel toggles and dragged splitters never write the settings and the defaults count.
    void enterTourMode(const Tour& tour);
    void leaveTourMode();
    void buildPracticeProject(const QString& practice); // Tour::practice: "pool", "timeline", "stack"
    class TourManager* m_tours = nullptr;
    struct TourSaved {
        bool active = false;
        ProjectData data; // the user's project (with playhead and views)
        QString path, saveSuggestion, templateDir;
        QHash<QString, QString> templateCopies;
        bool modified = false;
        QSet<int> selection;
        Page page = Page::Edit;
        bool large = false;
        QVector<bool> toggles; // tourToggles(), in that order
        bool scopes = true;
        QVector<QPair<QPointer<QSplitter>, QList<int>>> sizes;
        QSize window; // the window's size (a page's minimum could have grown it during the tour)
        int tool = 0; // the timeline tool (TimelineView::Tool): a step may switch it
        QByteArray fingerprint; // the project file as last loaded/saved here: an outside change meanwhile is noticed
        int compClip = 0;       // the clip the Comp page showed
        std::shared_ptr<const Editor::Clipboards> clipboards; // the user's copied clips and grade
        QVector<SavedView> views; // the timelines' zoom and scroll, set again once the layout is back
        std::optional<QPointF> studioView; // the Studio lanes' zoom (x) and left frame (y), if the page was built
        bool studioFitPending = false;     // the lanes were still to be fitted (a project opened while hidden)
        bool studioSized = false;          // its sizes were set (m_studioSized) and the viewer's height there
        QList<int> studioSideSizes;
    } m_tourSaved;
    bool m_tourSwap = false; // the user's project and the practice project trade places: no "another project" effects
    bool m_reloadingSame = false; // checkProjectFile reads the open project's file again: not a new view (core rule)
    void closePopups(); // open context menus closed (before the project under them changes)
    QVector<QToolButton*> tourToggles() const; // the panel toggles the tour sets (pool, effects, storage, ...)
    class QShortcut* m_voiceoverEscape = nullptr;

    // Building the widgets (again: rebuildUi, MainWindowRebuild.cpp)
    void buildUi();               // layout, actions, chat panel and their connections (MainWindow.cpp)
    void tearDownUi();            // every widget, menu, action and UI connection gone; the model untouched
    QObject* m_ui = nullptr;      // context of the UI's connections to what outlives it (deleted with the widgets)
    bool m_rebuilding = false;
    bool m_rebuildQueued = false; // applyLookChange/a postponed rebuild waits for its turn
    int m_uiGeneration = 0;
    class RenderQueue* m_keptQueue = nullptr; // a running render goes on in the new Deliver page
    QPointer<class BackgroundJobs> m_oldJobs; // its jobs move to the new indicator
    bool m_resumeQuit = false;    // restartRestoring: the project is in the resume file, quitting asks nothing
    QString m_resumeFile;
};
