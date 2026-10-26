#pragma once
#include "app/AiSettings.h"
#include "core/Editor.h"
#include "core/LiveHost.h"
#include "core/PlaybackMeter.h"
#include "core/ProjectFile.h"
#include "core/Types.h"

#include <QElapsedTimer>
#include "ui/timeline/ViewState.h"

#include <QHash>
#include <QLockFile>
#include <QMainWindow>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <functional>
#include <memory>

class Engine;
class Project;
class Selection;
class MediaPool;
class EffectsLibrary;
class MediaStorage;
class Viewer;
class Inspector;
class ColorPanel;
class TranscriptPanel;
class ScopesPanel;
class Mixer;
class TimelinePanel;
class QMenu;
class QSplitter;
class QStackedWidget;
class QButtonGroup;
class QToolButton;
class DeliverPanel;

namespace Ai {
struct Endpoint;
class Keys;
} // namespace Ai

class MainWindow : public QMainWindow, public LiveHost {
    Q_OBJECT
public:
    explicit MainWindow(Engine* engine, QWidget* parent = nullptr);
    ~MainWindow() override;
    // placeOnTimeline (test helper --demo): one after another on V1/A1; adoptFormat = project format from the first clip
    void importFiles(const QStringList& paths, bool placeOnTimeline = false, bool adoptFormat = true);
    // Timeline of another editor (FCP7 XML, OTIO, EDL) as a new timeline; false = not readable (message shown)
    bool importTimelineFile(const QString& path);

    // Pages like in DaVinci (switchable at the bottom). New page: add here + in showPage().
    enum class Page { Media, Edit, Deliver, Color };
    void showPage(Page page);

    // Project file (.schneidi): only references to the original media + edit
    bool openProject(const QString& path);
    bool offerAutosaveRestore(); // after a crash: offer the last automatic backup (true = restored)
    void disableAutosave();      // test runs: do not write/delete a backup
    // Set project settings (one undo step); test helper --format
    void setProjectFormat(const ProjectFormat& format);
    // Restart (design/language): main() restarts schneidi with these arguments after quitting
    bool restartRequested() const { return m_restartRequested; }
    QStringList restartArguments() const;

    // Live connection (cli/Live.h, started in main.cpp): what an AI session sees and controls (MainWindowLive.cpp)
    Project* liveProject() override { return m_project; }
    QString liveProjectPath() const override { return m_projectPath; }
    Selection* liveSelection() override { return m_selection; }
    int livePlayhead() const override;
    bool livePlaying() const override;
    void liveSeek(int frame) override;
    void livePlay(bool play) override;
    bool liveSave(const QString& path, QString* error) override;
    void liveGenerateProxies(const QStringList& files) override;
    QJsonObject liveProxyState(const QString& file) const override;
    void liveConnectionChanged(bool connected, const QString& client) override;
    void liveChanged(const LiveChange& change) override;
    void liveActivity(const LiveActivity& activity) override;
    void liveCommandRunning(bool running, bool ownChat) override;
    // AI log (click on the AI indicator, Workspace › AI Log): popup under the indicator
    void showAiLog();
    class AiLog* aiLog() const { return m_aiLog; }
    // "Connect with Claude" in Settings › AI (cli/Setup.h; main.cpp hands it in, the app does not link the CLI)
    void setClaudeSetup(const std::function<QJsonObject(bool remove)>& setup) { m_claudeSetup = setup; }
    // Buttons of the AI indicator in the top bar and Settings › AI: end the session and refuse new ones (reason for
    // the AI; empty = the user disconnected it) / allow again / how long a silent AI keeps the app (ms, 0 = always)
    void setLiveControls(const std::function<void(const QString& reason)>& disconnect, const std::function<void()>& allow,
                         const std::function<void(int ms)>& idleLimit);
    // Settings (File › Settings…, MainWindowSettings.cpp); test runs apply them without saving
    void settingsDialog(int tab = 0);
    const AiSettings& aiSettings() const { return m_aiSettings; }
    void setAiSettings(const AiSettings& s);
    // AI chat (the "AI" panel, MainWindowChat.cpp): main.cpp hands in its tool server (`schneidi-cli mcp`; the app
    // does not link the command layer) with the environment of the live connection (Cli::Live::Server::chatEnvironment)
    void setAiChatTools(const QStringList& command, const QHash<QString, QString>& environment);
    void setAiChatOffline(bool on); // test runs: the chat never sends anything
    class AiChat* aiChat() const { return m_chat; }
    class AiChatPanel* aiChatPanel() const { return m_chatPanel; }
    void showAiChat(bool on);
    // Update hint (app/UpdateCheck.h): automatically at most once a day a few seconds after the start (main.cpp, not
    // in test runs); manual = Help › Check for Updates… (also says "up to date" or why it failed)
    void startUpdateCheck();
    void checkForUpdates(bool manual);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void buildLayout();
    QWidget* buildTopBar();
    QWidget* buildPageBar();
    void updateLeftColumn(); // show the left column only if Media Pool or Effects is on
    // Panel sizes per page are remembered (settings layout/<splitter>); defaults when nothing is saved yet
    void watchSizes(QSplitter* s, const QString& name);
    void applySizes(QSplitter* s, const QList<int>& defaults);
    void resetSizes();
    // Panel set of the Edit page (Workspace → Panels): which toggles are on
    void applyPanelSet(bool pool, bool effects, bool storage, bool transcript, bool mixer, bool inspector);
    void onDrop(const QStringList& paths, int frame, int track);
    // Subtitles (SRT): every file becomes a new subtitle track; export of the visible track
    void importSubtitleFiles(const QStringList& paths);
    void importSubtitlesDialog();
    void exportSubtitlesDialog();
    // Timelines of other editors (FCP7 XML, OpenTimelineIO, EDL), MainWindowExchange.cpp
    void importTimelineDialog();
    void contactSheetDialog(); // File › Save Contact Sheet: stills of the timeline as one picture
    void exportTimelineDialog();
    void transcribeDialog();
    MediaInfo probeCached(const QString& path);
    // Like DaVinci with the first clip in an empty timeline: adapt the project to the clip's resolution/frame rate
    // (ask = ask first, otherwise take over directly)
    void offerClipFormat(const QStringList& paths, bool ask);
    void onFormatChanged();
    void projectSettingsDialog();
    void grabStill();
    // Green Screen pipette (Inspector): the viewer shows the clip alone without its key until a click picks the color
    void startKeyPick(int clipId);
    void stopKeyPick();
    void updateRenderCacheBar(); // render cache bar above the timeline
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
    bool sourceActive() const; // Viewer shows the source -> I/O apply to the source In/Out
    QString editSource() const; // source for F9/F10 …: source view, otherwise selection in the Media Pool
    void sourceEdit(Editor::SourceEditMode mode);
    void updateViewer();        // scrubber range, In/Out and title of the Viewer
    void jumpToEdit(int direction);
    void jumpToMarker(int direction);
    void jumpToFrame(int frame);
    void jumpTo(const QVector<int>& points, int direction);
    void stepFrames(int frames);
    void shuttle(int direction);

    void newProject();
    void openProjectDialog();
    bool save();
    bool saveAs();
    void openBackupDialog();
    bool saveTo(const QString& path);
    bool maybeSave(); // false = cancel
    // Before New/Open: if the render queue is running, ask and cancel (otherwise it writes its status
    // into the queue of the next project), then maybeSave(). false = cancel
    bool maybeLeaveProject();
    void offerRestart(const QString& message, const QString& now, const QString& later);
    // ask = offer relinking missing media / list missing effects (not when reloading after an outside change)
    bool applyLoaded(ProjectData data, const QString& path, bool ask = true);
    // Project file changed outside (schneidi-cli, an AI, another window): reload; unsaved changes here ask first
    void checkProjectFile();
    void setProjectPath(const QString& path);
    void updateTitle();
    static QString appName(); // "schneidi 0.9.0" or "schneidi 0.9.0 → 0.10.0" (without the build state)
    void addRecent(const QString& path);
    void rebuildRecentMenu();
    void autosave();
    void removeAutosave();
    // Every running instance has its own backup slot (QLockFile) so that two windows do not
    // overwrite or delete each other's backup; slot 1 = "autosave.schneidi" as before
    QString autosavePath();
    QString autosaveKey() const; // QSettings key: which project the backup belongs to
    void createCompoundClip(); // ask for the name like DaVinci
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
    bool offersRenderCache() const; // the hint offers "Render-Cache: Smart" instead of proxies
    QStringList playingVideoFiles() const; // video files of what plays (timeline incl. nested clips, or the source)
    bool expensiveAtPlayhead() const;      // a clip with effects or changed speed plays here (RenderCache::isExpensive)
    void createMulticamClip(const QStringList& paths);
    void addMulticamMenu(QMenu* menu); // "Switch Angle" + "Flatten" for selected multicam segments
    int selectedMulticamSequence() const;

    Engine* m_engine;
    Project* m_project;
    Selection* m_selection;
    Editor* m_editor;

    MediaPool* m_mediaPool = nullptr;
    EffectsLibrary* m_effects = nullptr;
    MediaStorage* m_storage = nullptr;
    Viewer* m_viewer = nullptr;
    class MulticamView* m_multicam = nullptr;
    class QAction* m_multicamAction = nullptr;
    bool m_multicamMode = false;
    bool m_multicamSyncing = false, m_multicamSyncAgain = false;
    QTimer m_engineTimer; // bundles model changes -> engine
    PlaybackMeter m_playMeter;
    QElapsedTimer m_playClock;
    QStringList m_slowFiles; // playback was too slow with these files (proxy hint); empty = no hint
    double m_slowRate = 0, m_slowFps = 0;
    bool m_proxyHintClosed = false; // closed: not again in this session
    bool m_slowEffects = false;     // clips with effects played: the hint offers the smart render cache first
    Inspector* m_inspector = nullptr;
    ColorPanel* m_colorPanel = nullptr;
    QPointer<class KeyColorPicker> m_keyPicker; // while the pipette is on
    TranscriptPanel* m_transcript = nullptr;
    ScopesPanel* m_scopes = nullptr; // Color page, right of the wheels (like DaVinci)
    Mixer* m_mixer = nullptr;
    TimelinePanel* m_timeline = nullptr;
    DeliverPanel* m_deliver = nullptr;

    QStackedWidget* m_pages = nullptr;
    QSplitter* m_editTop = nullptr;
    QSplitter* m_editLeft = nullptr;   // Media Pool above Effects Library (like DaVinci)
    QSplitter* m_editMain = nullptr;
    QSplitter* m_editBottom = nullptr; // Timeline | Mixer
    QSplitter* m_mediaPage = nullptr;
    QSplitter* m_mediaTop = nullptr;
    QSplitter* m_deliverPage = nullptr;
    QSplitter* m_deliverRight = nullptr;
    QSplitter* m_colorPage = nullptr; // Viewer, timeline, color wheels (like DaVinci Color)
    QSplitter* m_colorBottom = nullptr; // color wheels | scopes
    QHash<QSplitter*, QList<int>> m_defaultSizes;
    QButtonGroup* m_pageButtons = nullptr;
    QToolButton* m_poolToggle = nullptr;
    QToolButton* m_effectsToggle = nullptr;
    QToolButton* m_storageToggle = nullptr;
    QToolButton* m_inspectorToggle = nullptr;
    QToolButton* m_mixerToggle = nullptr;
    QToolButton* m_transcriptToggle = nullptr;
    Page m_page = Page::Edit;

    QHash<QString, MediaInfo> m_probeCache;
    QHash<QString, int> m_sourcePos; // source playhead per file (this session only)
    QHash<int, int> m_sequencePos;   // playhead per timeline/compound clip (this session only)
    QHash<int, ViewState> m_sequenceView; // zoom/scroll per timeline/compound clip (this session only)
    int m_shownSequence = 0;         // last shown sequence (for m_sequencePos)
    class MediaCache* m_mediaCache = nullptr;

    QString m_projectPath; // empty = never saved yet
    class QFileSystemWatcher* m_projectWatcher = nullptr;
    QTimer m_projectCheck;            // waits until an outside save is done
    QByteArray m_projectFingerprint;  // project file as last loaded/saved here (ProjectFile::fingerprint)
    QMenu* m_recentMenu = nullptr;
    class QLabel* m_titleLabel = nullptr;
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
    class QLabel* m_updateText = nullptr;
    QUrl m_updatePage;
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
    bool sendChat(const QString& text);
    bool chatEndpoint(Ai::Endpoint& e, QString* problem);
    QString chatKeyHint(const QString& provider);
    void chatTurnFinished();
    void chatUndoChanged();
    void acceptChatChanges();
    void discardChatChanges();
    class AiChat* m_chat = nullptr;
    class AiChatPanel* m_chatPanel = nullptr;
    QSplitter* m_chatSplit = nullptr;
    QToolButton* m_chatToggle = nullptr;
    class QHBoxLayout* m_topBarLayout = nullptr;
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
};
