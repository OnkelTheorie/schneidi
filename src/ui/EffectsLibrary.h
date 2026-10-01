#pragma once
#include "core/Types.h"

#include "core/EffectFolders.h"

#include <QHash>
#include <QPixmap>
#include <QWidget>

class QFileSystemWatcher;
class QLineEdit;
class QTimer;
class QListWidget;
class QTreeWidget;

// Effects Library (Edit-Page wie DaVinci): links Kategorien (Toolbox → Video Transitions, Audio Transitions,
// Titles; Open FX → Filters; schneidi → Übergänge, LUTs (mitgeliefert); LUTs und Übergänge = eigener Effekte-Ordner,
// Unterordner als Unterkategorien), rechts die Einträge mit Vorschausymbol. Kategorien lassen sich zuklappen
// (gespeichert). „Effekte importieren…“ öffnet den Effekte-Ordner, neue Dateien erscheinen von selbst. Ziehen auf einen Schnitt/eine Clipkante legt
// den Übergang an, "Text" landet als Titel auf der Videospur, ein Filter auf einem Videoclip hängt den Effekt an;
// Doppelklick wendet den Eintrag am Playhead/auf die Auswahl an.
class EffectsLibrary : public QWidget {
    Q_OBJECT
public:
    // Drag-Daten eines Übergangs: "video:<Art-ID>", "luma:<Bild>" (Verlaufsblende) bzw. "audio:<Kurven-ID>"
    // (Titel nutzen MediaPool::MimeType)
    static constexpr const char* MimeType = "application/x-schneidi-transition";
    static bool parseTransition(const QByteArray& data, TrackKind* kind, TransitionStyle* style);
    // Drag-Daten eines Filters (Open FX): id aus der EffectRegistry, bzw. "lut:<Pfad>" für eine LUT
    static constexpr const char* EffectMimeType = "application/x-schneidi-effect";

    explicit EffectsLibrary(QWidget* parent = nullptr);

signals:
    void transitionRequested(TrackKind kind, const TransitionStyle& style);
    void titleRequested();
    void effectRequested(const QString& effectId);

private:
    void rebuild();
    void rescanFolders(); // eigene LUTs/Übergänge neu einlesen (Ordner geändert), Unterkategorien neu
    void fillGroups(class QTreeWidgetItem* root, int groupCategory, const QVector<EffectFolders::LutEntry>& entries);
    QPixmap fileIcon(const QString& path, bool lut, QString* error); // LUT: Musterbild, Übergang: A -> B
    void saveCollapsed();

    QTreeWidget* m_categories;
    QLineEdit* m_search;
    QListWidget* m_list;
    class QTreeWidgetItem* m_lutRoot = nullptr;
    class QTreeWidgetItem* m_transRoot = nullptr;
    QVector<EffectFolders::LutEntry> m_userLuts, m_userTransitions;
    QFileSystemWatcher* m_watcher;
    QTimer* m_rescan;
    struct IconEntry {
        qint64 stamp = 0; // Änderungszeit + Größe der Datei
        QPixmap icon;
        QString error;
    };
    QHash<QString, IconEntry> m_lutIcons;
};
