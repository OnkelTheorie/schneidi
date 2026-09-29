#pragma once
#include <QHash>
#include <QPixmap>
#include <QWidget>

class Project;
class Engine;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QPainter;
class QRectF;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

// Media Pool (oben links wie in DaVinci): links die Bin-Liste (Master mit Unter-Bins), rechts die Clips des
// gewählten Bins mit Vorschaubild (Symbol- oder Listenansicht). Doppelklick -> im Viewer als Quelle ansehen,
// Ziehen -> in die Timeline bzw. auf einen Bin (verschieben). Rechtsklick: Clipfarbe, Flags, Proxy-Medien.
class MediaPool : public QWidget {
    Q_OBJECT
public:
    static constexpr const char* MimeType = "application/x-schneidi-media";
    // Alle gezogenen Media-Pool-Clips (Pfade, eine Zeile je Clip) – zum Verschieben in einen Bin
    static constexpr const char* ListMimeType = "application/x-schneidi-media-list";
    // Gezogener Bin (id) – auf einen anderen Bin ziehen = dorthin verschieben
    static constexpr const char* BinMimeType = "application/x-schneidi-bin";
    // Eintrag "Text" (Titel-Generator) zum Reinziehen; steht statt eines Pfads in den Drag-Daten
    static constexpr const char* TitleItem = "schneidi:title";

    enum class ViewMode { Thumbnails, List };
    enum class SortKey { Name, Date, Duration };

    MediaPool(Project* project, Engine* engine, QWidget* parent = nullptr);

    // Ausgewählte Medien (ohne den Titel-Eintrag)
    QStringList selectedMedia() const;
    int currentBin() const { return m_currentBin; }
    void setCurrentBin(int id);
    void setViewMode(ViewMode mode);
    void setSort(SortKey key, bool ascending);
    // Suche wie DaVinci (Lupe oben): zeigt passende Clips (Name enthält Text) im Bin und seinen Unter-Bins
    void setSearch(const QString& text);
    // Ansicht/Sortierung nicht in den Einstellungen speichern (Testläufe)
    void setSaveSettings(bool on) { m_saveSettings = on; }

    // Rechtsklick-Menüs „Clipfarbe“ und „Flags“ (auch in der Timeline) für die Clips dieser Medien
    static void addClipColorMenu(QMenu* menu, Project* project, const QStringList& paths);
    static void addFlagsMenu(QMenu* menu, Project* project, const QStringList& paths);
    // Kleine Fahne wie DaVinci (Stange + Wimpel) in r
    static void drawFlag(QPainter& p, const QRectF& r, const QColor& color);

public slots:
    void importDialog();
    void importFiles(const QStringList& paths); // landet im gewählten Bin
    // Wie DaVinci „Add Folder and SubFolders into Media Pool (Create Bins)“: Bin mit dem Ordnernamen im gewählten Bin,
    // Unterordner mit Medien als Unter-Bins; ein Undo-Schritt für die Bins
    void importFolder(const QString& dir);
    void newBin();                               // im gewählten Bin, Name gleich bearbeiten (Strg+Shift+N)

signals:
    void sourceRequested(const QString& path);
    // Untertiteldateien (.srt) importiert/hineingezogen: werden zu Untertitelspuren, nicht zu Medien
    void subtitleFilesImported(const QStringList& paths);

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override; // Esc im Suchfeld

private:
    void rebuild();      // Medienliste geändert: Vorschaubilder neu, dann Ansicht
    void rebuildBins();  // Bin-Liste neu (Auswahl/aufgeklappte Bins bleiben)
    void rebuildClips(); // Clips des gewählten Bins (sortiert)
    void showContextMenu(const QPoint& pos);
    void showBinMenu(const QPoint& pos);
    void removeBin(int id); // mit Rückfrage, wenn Clips darin liegen
    QStringList addFiles(const QStringList& paths, int bin); // liefert die nicht lesbaren Dateinamen
    bool importFolderInto(const QString& dir, int parentBin, int depth, QStringList* failed);
    void updateItem(const QString& path); // Vorschaubild mit Clipfarbe, Flags, Proxy-Symbol bzw. Fortschritt
    void updateProxyStatus();
    void applyViewMode();
    void saveSettings() const;

    Project* m_project;
    Engine* m_engine;
    QTreeWidget* m_bins;
    QLabel* m_binTitle;
    QListWidget* m_list;
    QLabel* m_proxyStatus;
    QToolButton* m_proxyCancel;
    QToolButton* m_thumbView;
    QToolButton* m_listView;
    QToolButton* m_sortBtn;
    QToolButton* m_searchBtn;
    QLineEdit* m_search;
    QHash<QString, QListWidgetItem*> m_items; // Pfad -> Eintrag
    QHash<QString, QPixmap> m_thumbs;         // Vorschaubild ohne Anzeigen
    int m_currentBin = 0;
    int m_editBin = 0; // neu angelegt: nach dem Neuaufbau Namen bearbeiten
    ViewMode m_viewMode = ViewMode::Thumbnails;
    SortKey m_sortKey = SortKey::Name;
    bool m_sortAscending = true;
    bool m_saveSettings = true;
    bool m_rebuildQueued = false;
};
