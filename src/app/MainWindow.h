#pragma once
#include "core/Editor.h"
#include "core/LiveHost.h"
#include "core/ProjectFile.h"
#include "core/Types.h"
#include "ui/timeline/ViewState.h"

#include <QHash>
#include <QLockFile>
#include <QMainWindow>
#include <QTimer>
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

class MainWindow : public QMainWindow, public LiveHost {
    Q_OBJECT
public:
    explicit MainWindow(Engine* engine, QWidget* parent = nullptr);
    // placeOnTimeline (test helper --demo): one after another on V1/A1; adoptFormat = project format from the first clip
    void importFiles(const QStringList& paths, bool placeOnTimeline = false, bool adoptFormat = true);

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
    bool liveSave(QString* error) override;
    void liveGenerateProxies(const QStringList& files) override;
    QJsonObject liveProxyState(const QString& file) const override;
    void liveConnectionChanged(bool connected, const QString& client) override;
    void liveClipsChanged(const QVector<int>& clipIds) override;
    // Buttons of the AI indicator in the top bar: end the session (and refuse new ones) / allow again
    void setLiveControls(const std::function<void()>& disconnect, const std::function<void()>& allow);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void buildLayout();
    QWidget* buildTopBar();
    QWidget* buildPageBar();
    void updateLeftColumn(); // show the left column only if Media Pool or Effects is on
    void onDrop(const QStringList& paths, int frame, int track);
    // Subtitles (SRT): every file becomes a new subtitle track; export of the visible track
    void importSubtitleFiles(const QStringList& paths);
    void importSubtitlesDialog();
    void exportSubtitlesDialog();
    void transcribeDialog();
    MediaInfo probeCached(const QString& path);
    // Like DaVinci with the first clip in an empty timeline: adapt the project to the clip's resolution/frame rate
    // (ask = ask first, otherwise take over directly)
    void offerClipFormat(const QStringList& paths, bool ask);
    void onFormatChanged();
    void projectSettingsDialog();
    void grabStill();
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
    Inspector* m_inspector = nullptr;
    ColorPanel* m_colorPanel = nullptr;
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
    QWidget* buildLiveIndicator();
    QWidget* m_liveBox = nullptr;
    class QLabel* m_liveLabel = nullptr;
    QToolButton* m_liveButton = nullptr;
    QTimer m_liveLinger; // keeps "connected" visible a moment after a one-shot command
    bool m_liveConnected = false;
    bool m_liveRefused = false; // the user disconnected the AI
    QString m_liveClient;
    std::function<void()> m_liveDisconnect, m_liveAllow;
};
