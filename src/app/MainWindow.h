#pragma once
#include "core/Editor.h"
#include "core/ProjectFile.h"
#include "core/Types.h"

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
class Mixer;
class TimelinePanel;
class QMenu;
class QSplitter;
class QStackedWidget;
class QButtonGroup;
class QToolButton;
class DeliverPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Engine* engine, QWidget* parent = nullptr);
    // placeOnTimeline (Testhilfe --demo): hintereinander auf V1/A1; adoptFormat = Projektformat vom ersten Clip
    void importFiles(const QStringList& paths, bool placeOnTimeline = false, bool adoptFormat = true);

    // Seiten wie in DaVinci (unten umschaltbar). Neue Seite: hier + in showPage() ergänzen.
    enum class Page { Media, Edit, Deliver, Color };
    void showPage(Page page);

    // Projektdatei (.schneidi): nur Verweise auf die Originalmedien + Schnitt
    bool openProject(const QString& path);
    bool offerAutosaveRestore(); // nach Absturz: letzte automatische Sicherung anbieten (true = wiederhergestellt)
    void disableAutosave();      // Testläufe: keine Sicherung schreiben/löschen
    // Projekteinstellungen setzen (ein Undo-Schritt); Testhilfe --format
    void setProjectFormat(const ProjectFormat& format);
    // Neustart (Design/Sprache): main() startet schneidi nach dem Beenden mit diesen Argumenten neu
    bool restartRequested() const { return m_restartRequested; }
    QStringList restartArguments() const;

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void buildLayout();
    QWidget* buildTopBar();
    QWidget* buildPageBar();
    void updateLeftColumn(); // linke Spalte nur zeigen, wenn Media Pool oder Effects an ist
    void onDrop(const QStringList& paths, int frame, int track);
    // Untertitel (SRT): jede Datei wird eine neue Untertitelspur; Export der sichtbaren Spur
    void importSubtitleFiles(const QStringList& paths);
    void importSubtitlesDialog();
    void exportSubtitlesDialog();
    MediaInfo probeCached(const QString& path);
    // Wie DaVinci beim ersten Clip in leerer Timeline: Projekt an Auflösung/Framerate des Clips anpassen
    // (ask = nachfragen, sonst direkt übernehmen)
    void offerClipFormat(const QStringList& paths, bool ask);
    void onFormatChanged();
    void projectSettingsDialog();
    void grabStill();
    void updateRenderCacheBar(); // Render-Cache-Balken über der Timeline
    void clipSpeedDialog();
    void normalizeAudioDialog();
    void buildActions();
    QAction* makeAction(QMenu* menu, const QString& id, const QString& text, const QKeySequence& key,
                       const std::function<void()>& fn);
    // Quellansicht (Doppelklick im Media Pool): an der zuletzt gezeigten Stelle der Datei öffnen
    void showSource(const QString& path);
    bool sourceActive() const; // Viewer zeigt die Quelle -> I/O wirken auf die Quell-In/Out
    QString editSource() const; // Quelle für F9/F10 …: Quellansicht, sonst Auswahl im Media Pool
    void sourceEdit(Editor::SourceEditMode mode);
    void updateViewer();        // Scrubber-Bereich, In/Out und Titel des Viewers
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
    bool maybeSave(); // false = Abbrechen
    void offerRestart(const QString& message, const QString& now, const QString& later);
    bool applyLoaded(ProjectData data, const QString& path);
    void setProjectPath(const QString& path);
    void updateTitle();
    void addRecent(const QString& path);
    void rebuildRecentMenu();
    void autosave();
    void removeAutosave();
    // Jede laufende Instanz hat einen eigenen Sicherungs-Platz (QLockFile), damit sich zwei Fenster nicht
    // gegenseitig die Sicherung überschreiben oder löschen; Platz 1 = „autosave.schneidi“ wie bisher
    QString autosavePath();
    QString autosaveKey() const; // QSettings-Schlüssel: zu welchem Projekt die Sicherung gehört
    void createCompoundClip(); // Name abfragen wie DaVinci
    void openSelectedCompound();

    Engine* m_engine;
    Project* m_project;
    Selection* m_selection;
    Editor* m_editor;

    MediaPool* m_mediaPool = nullptr;
    EffectsLibrary* m_effects = nullptr;
    MediaStorage* m_storage = nullptr;
    Viewer* m_viewer = nullptr;
    QTimer m_engineTimer; // bündelt Modelländerungen -> Engine
    Inspector* m_inspector = nullptr;
    ColorPanel* m_colorPanel = nullptr;
    Mixer* m_mixer = nullptr;
    TimelinePanel* m_timeline = nullptr;
    DeliverPanel* m_deliver = nullptr;

    QStackedWidget* m_pages = nullptr;
    QSplitter* m_editTop = nullptr;
    QSplitter* m_editLeft = nullptr;   // Media Pool über Effects Library (wie DaVinci)
    QSplitter* m_editMain = nullptr;
    QSplitter* m_editBottom = nullptr; // Timeline | Mixer
    QSplitter* m_mediaPage = nullptr;
    QSplitter* m_mediaTop = nullptr;
    QSplitter* m_deliverPage = nullptr;
    QSplitter* m_deliverRight = nullptr;
    QSplitter* m_colorPage = nullptr; // Viewer, Timeline, Farbräder (wie DaVinci Color)
    QButtonGroup* m_pageButtons = nullptr;
    QToolButton* m_poolToggle = nullptr;
    QToolButton* m_effectsToggle = nullptr;
    QToolButton* m_storageToggle = nullptr;
    QToolButton* m_inspectorToggle = nullptr;
    QToolButton* m_mixerToggle = nullptr;
    Page m_page = Page::Edit;

    QHash<QString, MediaInfo> m_probeCache;
    QHash<QString, int> m_sourcePos; // Quell-Playhead je Datei (nur in dieser Sitzung)
    QHash<int, int> m_sequencePos;   // Playhead je Timeline/Compound Clip (nur in dieser Sitzung)
    int m_shownSequence = 0;         // zuletzt gezeigte Sequenz (für m_sequencePos)
    class MediaCache* m_mediaCache = nullptr;

    QString m_projectPath; // leer = noch nie gespeichert
    QMenu* m_recentMenu = nullptr;
    class QLabel* m_titleLabel = nullptr;
    class QTimer* m_autosaveTimer = nullptr;
    bool m_autosaveDisabled = false;
    int m_autosaveSlot = 0; // 0 = noch keiner belegt
    std::unique_ptr<QLockFile> m_autosaveLock;
    bool m_restartRequested = false;
};
